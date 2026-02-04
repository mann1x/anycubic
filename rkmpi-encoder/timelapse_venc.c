/*
 * Hardware VENC-based Timelapse Encoding
 *
 * Uses RV1106 hardware H.264 encoder directly for timelapse videos.
 * Flow: JPEG -> NV12 (TurboJPEG) -> H.264 (VENC) -> MP4 (minimp4)
 */

#include "timelapse_venc.h"
#include "timelapse.h"   /* For g_timelapse.temp_dir */
#include "turbojpeg.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_mb.h"
#include "rk_comm_venc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Enable minimp4 implementation */
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

/* VENC channel for timelapse - use channel 2 to avoid conflicts */
#define VENC_CHN_TIMELAPSE  3  /* Channel 0=H.264, 1=JPEG, 2=Display, 3=Timelapse */

/* Logging */
#define TL_LOG(fmt, ...) fprintf(stderr, "[TIMELAPSE_VENC] " fmt, ##__VA_ARGS__)

/*
 * Validate JPEG data before decoding.
 * Returns 1 if valid, 0 if corrupt.
 *
 * Performs thorough validation to catch internally corrupt JPEGs:
 * - Check SOI/EOI markers
 * - Scan for multiple SOI markers (indicates concatenated frames)
 * - Verify no premature EOI markers
 */
static int validate_jpeg(const uint8_t *data, size_t size) {
    /* Minimum JPEG size: SOI (2) + APP0/JFIF minimum + SOF + EOI */
    if (!data || size < 100) {
        TL_LOG("Invalid JPEG: too small (%zu bytes)\n", size);
        return 0;
    }

    /* Check SOI marker (Start of Image): FFD8 */
    if (data[0] != 0xFF || data[1] != 0xD8) {
        TL_LOG("Invalid JPEG: bad SOI (0x%02x%02x)\n", data[0], data[1]);
        return 0;
    }

    /* Check EOI marker (End of Image): FFD9 at end */
    if (data[size - 2] != 0xFF || data[size - 1] != 0xD9) {
        TL_LOG("Invalid JPEG: bad EOI at end (0x%02x%02x)\n",
               data[size - 2], data[size - 1]);
        return 0;
    }

    /* Scan for multiple SOI/SOF markers - indicates concatenated/corrupt data
     * SOI = FFD8 (Start of Image)
     * SOF = FFC0-FFCF (Start of Frame, various types)
     * EOI = FFD9 (End of Image)
     */
    int soi_count = 0;
    int sof_count = 0;
    int eoi_count = 0;

    for (size_t i = 0; i < size - 1; i++) {
        if (data[i] == 0xFF) {
            uint8_t marker = data[i + 1];

            if (marker == 0xD8) {
                /* SOI - Start of Image */
                soi_count++;
                if (soi_count > 1) {
                    TL_LOG("Invalid JPEG: multiple SOI markers (%d at offset %zu)\n",
                           soi_count, i);
                    return 0;
                }
            } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                /* SOF markers: C0-C3 (baseline/extended/progressive/lossless)
                 * C5-C7, C9-CB, CD-CF (differential variants)
                 * Exclude C4 (DHT), C8 (JPG), CC (DAC) which are not SOF */
                sof_count++;
                if (sof_count > 1) {
                    TL_LOG("Invalid JPEG: multiple SOF markers (%d at offset %zu, type=0x%02x)\n",
                           sof_count, i, marker);
                    return 0;
                }
            } else if (marker == 0xD9) {
                /* EOI - End of Image */
                eoi_count++;
                /* EOI should only appear at the very end */
                if (i < size - 2) {
                    TL_LOG("Invalid JPEG: premature EOI at offset %zu (size=%zu)\n",
                           i, size);
                    return 0;
                }
            }
            /* Skip over marker payload to avoid false positives in data */
            if (marker >= 0xC0 && marker <= 0xFE && marker != 0xD8 && marker != 0xD9 &&
                !(marker >= 0xD0 && marker <= 0xD7)) {  /* Skip RST markers */
                /* Markers with length field (2 bytes big-endian) */
                if (i + 3 < size) {
                    uint16_t len = (data[i + 2] << 8) | data[i + 3];
                    if (len >= 2 && i + 1 + len < size) {
                        i += len;  /* Skip marker payload */
                    }
                }
            }
        }
    }

    if (soi_count != 1) {
        TL_LOG("Invalid JPEG: SOI count=%d\n", soi_count);
        return 0;
    }
    if (sof_count != 1) {
        TL_LOG("Invalid JPEG: SOF count=%d\n", sof_count);
        return 0;
    }
    if (eoi_count != 1) {
        TL_LOG("Invalid JPEG: EOI count=%d\n", eoi_count);
        return 0;
    }

    return 1;
}

/* State for VENC-based encoding */
typedef struct {
    int initialized;
    int width;
    int height;
    int fps;
    int frame_count;

    /* TurboJPEG decoder */
    tjhandle tj_handle;

    /* NV12 buffer for decoded frames */
    uint8_t *nv12_buffer;
    size_t nv12_size;

    /* RKMPI memory pool and block for VENC input */
    MB_POOL mb_pool;
    MB_BLK mb_blk;

    /* Temp file for MP4 during encoding */
    FILE *temp_file;
    char temp_path[512];

    /* minimp4 muxer */
    MP4E_mux_t *mp4_mux;
    mp4_h26x_writer_t mp4_writer;

    /* Timestamp tracking (90kHz timescale) */
    uint32_t timestamp;
    uint32_t frame_duration;

} TimelapseVENCState;

static TimelapseVENCState g_state = {0};

/* File write callback for minimp4 */
static int mp4_write_callback(int64_t offset, const void *buffer, size_t size, void *token) {
    FILE *f = (FILE *)token;
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        return 1;
    }
    return fwrite(buffer, 1, size, f) != size;
}

/* Initialize VENC channel for timelapse H.264 encoding */
static int init_venc_timelapse(int width, int height, int fps) {
    RK_S32 ret;
    VENC_CHN_ATTR_S attr;
    VENC_RECV_PIC_PARAM_S recv_param;

    memset(&attr, 0, sizeof(attr));

    /* H.264 encoding */
    attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;  /* NV12 */
    attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
    attr.stVencAttr.u32PicWidth = width;
    attr.stVencAttr.u32PicHeight = height;
    attr.stVencAttr.u32VirWidth = width;
    attr.stVencAttr.u32VirHeight = height;
    attr.stVencAttr.u32StreamBufCnt = 2;
    attr.stVencAttr.u32BufSize = width * height * 3 / 2;

    /* VBR rate control - good quality for timelapse */
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
    attr.stRcAttr.stH264Vbr.u32Gop = fps;  /* GOP = 1 second */
    attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum = fps;
    attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen = 1;
    attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum = fps;
    attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen = 1;
    attr.stRcAttr.stH264Vbr.u32BitRate = 4000;  /* 4Mbps for timelapse */
    attr.stRcAttr.stH264Vbr.u32MaxBitRate = 8000;
    attr.stRcAttr.stH264Vbr.u32MinBitRate = 1000;

    attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    attr.stGopAttr.s32VirIdrLen = 0;

    ret = RK_MPI_VENC_CreateChn(VENC_CHN_TIMELAPSE, &attr);
    if (ret != RK_SUCCESS) {
        TL_LOG("RK_MPI_VENC_CreateChn failed: 0x%x\n", ret);
        return -1;
    }

    memset(&recv_param, 0, sizeof(recv_param));
    recv_param.s32RecvPicNum = -1;

    ret = RK_MPI_VENC_StartRecvFrame(VENC_CHN_TIMELAPSE, &recv_param);
    if (ret != RK_SUCCESS) {
        TL_LOG("RK_MPI_VENC_StartRecvFrame failed: 0x%x\n", ret);
        RK_MPI_VENC_DestroyChn(VENC_CHN_TIMELAPSE);
        return -1;
    }

    TL_LOG("VENC initialized: %dx%d @ %dfps\n", width, height, fps);
    return 0;
}

static void cleanup_venc_timelapse(void) {
    RK_MPI_VENC_StopRecvFrame(VENC_CHN_TIMELAPSE);
    RK_MPI_VENC_DestroyChn(VENC_CHN_TIMELAPSE);
}

int timelapse_venc_init(int width, int height, int fps) {
    if (g_state.initialized) {
        TL_LOG("Already initialized\n");
        return -1;
    }

    memset(&g_state, 0, sizeof(g_state));
    g_state.width = width;
    g_state.height = height;
    g_state.fps = fps;

    /* Calculate frame duration in 90kHz timescale */
    g_state.frame_duration = 90000 / fps;

    /* Initialize TurboJPEG decoder */
    g_state.tj_handle = tjInitDecompress();
    if (!g_state.tj_handle) {
        TL_LOG("tjInitDecompress failed\n");
        return -1;
    }

    /* Allocate NV12 buffer */
    g_state.nv12_size = width * height * 3 / 2;
    g_state.nv12_buffer = (uint8_t *)malloc(g_state.nv12_size);
    if (!g_state.nv12_buffer) {
        TL_LOG("Failed to allocate NV12 buffer\n");
        tjDestroy(g_state.tj_handle);
        return -1;
    }

    /* Create RKMPI memory pool for VENC input */
    MB_POOL_CONFIG_S pool_cfg;
    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.u64MBSize = g_state.nv12_size;
    pool_cfg.u32MBCnt = 2;  /* Double buffer */
    pool_cfg.enAllocType = MB_ALLOC_TYPE_DMA;
    pool_cfg.bPreAlloc = RK_TRUE;

    g_state.mb_pool = RK_MPI_MB_CreatePool(&pool_cfg);
    if (g_state.mb_pool == MB_INVALID_POOLID) {
        TL_LOG("RK_MPI_MB_CreatePool failed\n");
        free(g_state.nv12_buffer);
        tjDestroy(g_state.tj_handle);
        return -1;
    }

    /* Get a memory block from the pool */
    g_state.mb_blk = RK_MPI_MB_GetMB(g_state.mb_pool, g_state.nv12_size, RK_TRUE);
    if (g_state.mb_blk == MB_INVALID_HANDLE) {
        TL_LOG("RK_MPI_MB_GetMB failed\n");
        RK_MPI_MB_DestroyPool(g_state.mb_pool);
        free(g_state.nv12_buffer);
        tjDestroy(g_state.tj_handle);
        return -1;
    }

    /* Initialize VENC */
    if (init_venc_timelapse(width, height, fps) != 0) {
        RK_MPI_MB_ReleaseMB(g_state.mb_blk);
        RK_MPI_MB_DestroyPool(g_state.mb_pool);
        free(g_state.nv12_buffer);
        tjDestroy(g_state.tj_handle);
        return -1;
    }

    /* Create temp file for MP4 in the same directory as frames
     * (avoids /tmp which may be in RAM and run out of space for long timelapses) */
    const char *temp_dir = g_timelapse.temp_dir[0] ? g_timelapse.temp_dir : "/tmp";
    snprintf(g_state.temp_path, sizeof(g_state.temp_path),
             "%s/timelapse.mp4.tmp", temp_dir);
    g_state.temp_file = fopen(g_state.temp_path, "wb+");
    if (!g_state.temp_file) {
        TL_LOG("Failed to create temp file: %s\n", strerror(errno));
        cleanup_venc_timelapse();
        RK_MPI_MB_ReleaseMB(g_state.mb_blk); RK_MPI_MB_DestroyPool(g_state.mb_pool);
        free(g_state.nv12_buffer);
        tjDestroy(g_state.tj_handle);
        return -1;
    }

    /* Initialize minimp4 muxer */
    g_state.mp4_mux = MP4E_open(0, 0, g_state.temp_file, mp4_write_callback);
    if (!g_state.mp4_mux) {
        TL_LOG("MP4E_open failed\n");
        fclose(g_state.temp_file);
        unlink(g_state.temp_path);
        cleanup_venc_timelapse();
        RK_MPI_MB_ReleaseMB(g_state.mb_blk); RK_MPI_MB_DestroyPool(g_state.mb_pool);
        free(g_state.nv12_buffer);
        tjDestroy(g_state.tj_handle);
        return -1;
    }

    /* Initialize H.264 writer - use h264 codec (not h265) */
    if (mp4_h26x_write_init(&g_state.mp4_writer, g_state.mp4_mux, width, height, 0) != MP4E_STATUS_OK) {
        TL_LOG("mp4_h26x_write_init failed\n");
        MP4E_close(g_state.mp4_mux);
        fclose(g_state.temp_file);
        unlink(g_state.temp_path);
        cleanup_venc_timelapse();
        RK_MPI_MB_ReleaseMB(g_state.mb_blk); RK_MPI_MB_DestroyPool(g_state.mb_pool);
        free(g_state.nv12_buffer);
        tjDestroy(g_state.tj_handle);
        return -1;
    }

    TL_LOG("MP4 writer initialized, temp file: %s\n", g_state.temp_path);
    g_state.initialized = 1;
    g_state.frame_count = 0;
    g_state.timestamp = 0;

    TL_LOG("Initialized: %dx%d @ %dfps\n", width, height, fps);
    return 0;
}

int timelapse_venc_add_frame(const uint8_t *jpeg_data, size_t jpeg_size) {
    if (!g_state.initialized) {
        TL_LOG("Not initialized\n");
        return -1;
    }

    /* Validate JPEG data before processing */
    if (!validate_jpeg(jpeg_data, jpeg_size)) {
        TL_LOG("Invalid JPEG data (size=%zu, starts=%02x%02x)\n",
               jpeg_size,
               jpeg_size > 0 ? jpeg_data[0] : 0,
               jpeg_size > 1 ? jpeg_data[1] : 0);
        return -1;
    }

    int tj_width, tj_height, tj_subsamp, tj_colorspace;

    /* Pre-validate JPEG header before full decompression
     * This catches many corrupt JPEGs without crashing the decoder */
    if (tjDecompressHeader3(g_state.tj_handle, jpeg_data, jpeg_size,
                            &tj_width, &tj_height, &tj_subsamp, &tj_colorspace) != 0) {
        TL_LOG("JPEG header invalid: %s\n", tjGetErrorStr());
        return -1;
    }

    /* Verify dimensions match expected values */
    if (tj_width != g_state.width || tj_height != g_state.height) {
        TL_LOG("Frame size mismatch: got %dx%d, expected %dx%d\n",
               tj_width, tj_height, g_state.width, g_state.height);
        return -1;
    }

    /* Verify subsamp is valid (YUV420 or similar) */
    if (tj_subsamp != TJSAMP_420 && tj_subsamp != TJSAMP_422 && tj_subsamp != TJSAMP_444) {
        TL_LOG("Unsupported JPEG subsampling: %d\n", tj_subsamp);
        return -1;
    }

    /* Decode JPEG to YUV (I420 planar) then convert to NV12 */
    /* TurboJPEG outputs I420, we need NV12 for VENC */
    int y_size = g_state.width * g_state.height;
    int uv_size = y_size / 4;

    /* Temp buffer for I420 */
    uint8_t *i420_buf = (uint8_t *)malloc(g_state.nv12_size);
    if (!i420_buf) {
        TL_LOG("Failed to allocate I420 buffer\n");
        return -1;
    }

    /* Decode to I420 */
    uint8_t *planes[3] = {
        i420_buf,                    /* Y */
        i420_buf + y_size,           /* U */
        i420_buf + y_size + uv_size  /* V */
    };
    int strides[3] = {
        g_state.width,      /* Y stride */
        g_state.width / 2,  /* U stride */
        g_state.width / 2   /* V stride */
    };

    if (tjDecompressToYUVPlanes(g_state.tj_handle, jpeg_data, jpeg_size,
                                 planes, g_state.width, strides, g_state.height,
                                 TJFLAG_FASTDCT) != 0) {
        TL_LOG("tjDecompressToYUVPlanes failed: %s\n", tjGetErrorStr());
        free(i420_buf);
        return -1;
    }

    /* Convert I420 to NV12 (interleave U and V) */
    /* Y plane: copy directly */
    memcpy(g_state.nv12_buffer, i420_buf, y_size);

    /* UV plane: interleave U and V */
    uint8_t *nv12_uv = g_state.nv12_buffer + y_size;
    uint8_t *i420_u = i420_buf + y_size;
    uint8_t *i420_v = i420_buf + y_size + uv_size;
    for (int i = 0; i < uv_size; i++) {
        nv12_uv[i * 2] = i420_u[i];
        nv12_uv[i * 2 + 1] = i420_v[i];
    }

    free(i420_buf);

    /* Copy NV12 to RKMPI memory block */
    void *mb_vaddr = RK_MPI_MB_Handle2VirAddr(g_state.mb_blk);
    if (!mb_vaddr) {
        TL_LOG("RK_MPI_MB_Handle2VirAddr returned NULL\n");
        return -1;
    }
    memcpy(mb_vaddr, g_state.nv12_buffer, g_state.nv12_size);

    /* Sync memory for hardware access */
    RK_MPI_SYS_MmzFlushCache(g_state.mb_blk, RK_FALSE);

    /* Create video frame for VENC */
    VIDEO_FRAME_INFO_S frame;
    memset(&frame, 0, sizeof(frame));
    frame.stVFrame.u32Width = g_state.width;
    frame.stVFrame.u32Height = g_state.height;
    frame.stVFrame.u32VirWidth = g_state.width;
    frame.stVFrame.u32VirHeight = g_state.height;
    frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
    frame.stVFrame.pMbBlk = g_state.mb_blk;

    /* Send frame to VENC */
    RK_S32 ret = RK_MPI_VENC_SendFrame(VENC_CHN_TIMELAPSE, &frame, 1000);
    if (ret != RK_SUCCESS) {
        TL_LOG("RK_MPI_VENC_SendFrame failed: 0x%x\n", ret);
        return -1;
    }

    /* Get encoded H.264 stream - MUST allocate pstPack before calling GetStream */
    VENC_STREAM_S stream;
    memset(&stream, 0, sizeof(stream));
    stream.pstPack = malloc(sizeof(VENC_PACK_S));
    if (!stream.pstPack) {
        TL_LOG("Failed to allocate stream pack\n");
        return -1;
    }

    ret = RK_MPI_VENC_GetStream(VENC_CHN_TIMELAPSE, &stream, 1000);
    if (ret != RK_SUCCESS) {
        TL_LOG("RK_MPI_VENC_GetStream failed: 0x%x\n", ret);
        free(stream.pstPack);
        return -1;
    }

    /* Write H.264 NAL units to MP4 */
    if (stream.pstPack && stream.u32PackCount > 0) {
        for (RK_U32 i = 0; i < stream.u32PackCount; i++) {
            VENC_PACK_S *pack = &stream.pstPack[i];

            /* Get data pointer - use base address directly (not offset) like main encoder */
            void *pData = RK_MPI_MB_Handle2VirAddr(pack->pMbBlk);
            int size = pack->u32Len;

            /* Invalidate cache to ensure we read the encoder's output */
            RK_MPI_SYS_MmzFlushCache(pack->pMbBlk, RK_TRUE);  /* RK_TRUE = invalidate (read) */

            uint8_t *data = (uint8_t *)pData;

            /* Log first frame (keyframe with SPS/PPS) and every 10th frame */
            if (g_state.frame_count == 0) {
                TL_LOG("First frame: size=%d bytes (keyframe)\n", size);
            }

            /* Write NAL to MP4 with constant frame duration (not accumulating timestamp) */
            int mp4_ret = mp4_h26x_write_nal(&g_state.mp4_writer, data, size,
                                              g_state.frame_duration);
            if (mp4_ret != MP4E_STATUS_OK) {
                TL_LOG("mp4_h26x_write_nal failed: %d\n", mp4_ret);
            }
        }
    }

    /* Release stream buffer */
    RK_MPI_VENC_ReleaseStream(VENC_CHN_TIMELAPSE, &stream);
    free(stream.pstPack);

    g_state.frame_count++;

    if (g_state.frame_count % 10 == 0) {
        TL_LOG("Frame %d encoded\n", g_state.frame_count);
    }

    return 0;
}

int timelapse_venc_finish(const char *output_path) {
    if (!g_state.initialized) {
        TL_LOG("Not initialized\n");
        return -1;
    }

    TL_LOG("Finishing timelapse: %d frames, output=%s\n", g_state.frame_count, output_path);

    /* Check temp file size before close */
    long temp_size = 0;
    if (g_state.temp_file) {
        fflush(g_state.temp_file);
        fseek(g_state.temp_file, 0, SEEK_END);
        temp_size = ftell(g_state.temp_file);
        TL_LOG("Temp file size before close: %ld bytes\n", temp_size);
    }

    /* Close MP4 writer and muxer */
    TL_LOG("Closing MP4 writer...\n");
    mp4_h26x_write_close(&g_state.mp4_writer);

    TL_LOG("Closing MP4 muxer...\n");
    int mp4_ret = MP4E_close(g_state.mp4_mux);
    if (mp4_ret != MP4E_STATUS_OK) {
        TL_LOG("MP4E_close failed: %d\n", mp4_ret);
    }
    g_state.mp4_mux = NULL;

    /* Check temp file size after MP4 close */
    if (g_state.temp_file) {
        fflush(g_state.temp_file);
        fseek(g_state.temp_file, 0, SEEK_END);
        long final_size = ftell(g_state.temp_file);
        TL_LOG("Temp file size after MP4 close: %ld bytes\n", final_size);
    }

    /* Close temp file */
    TL_LOG("Closing temp file...\n");
    fclose(g_state.temp_file);
    g_state.temp_file = NULL;

    /* Move temp file to final location */
    if (rename(g_state.temp_path, output_path) != 0) {
        /* rename() may fail across filesystems, try copy */
        TL_LOG("rename failed, trying copy: %s\n", strerror(errno));

        FILE *src = fopen(g_state.temp_path, "rb");
        FILE *dst = fopen(output_path, "wb");
        if (src && dst) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
                fwrite(buf, 1, n, dst);
            }
            fclose(src);
            fclose(dst);
            unlink(g_state.temp_path);
            TL_LOG("Copied to %s\n", output_path);
        } else {
            if (src) fclose(src);
            if (dst) fclose(dst);
            TL_LOG("Copy failed\n");
            timelapse_venc_cancel();
            return -1;
        }
    } else {
        TL_LOG("Created %s\n", output_path);
    }

    /* Cleanup */
    cleanup_venc_timelapse();
    RK_MPI_MB_ReleaseMB(g_state.mb_blk);
    RK_MPI_MB_DestroyPool(g_state.mb_pool);
    free(g_state.nv12_buffer);
    tjDestroy(g_state.tj_handle);

    memset(&g_state, 0, sizeof(g_state));

    return 0;
}

void timelapse_venc_cancel(void) {
    if (!g_state.initialized) {
        return;
    }

    TL_LOG("Canceling timelapse\n");

    /* Close MP4 if open */
    if (g_state.mp4_mux) {
        mp4_h26x_write_close(&g_state.mp4_writer);
        MP4E_close(g_state.mp4_mux);
        g_state.mp4_mux = NULL;
    }

    /* Close and remove temp file */
    if (g_state.temp_file) {
        fclose(g_state.temp_file);
        g_state.temp_file = NULL;
    }
    if (g_state.temp_path[0]) {
        unlink(g_state.temp_path);
    }

    /* Cleanup VENC */
    cleanup_venc_timelapse();

    /* Free resources */
    if (g_state.mb_blk != MB_INVALID_HANDLE) {
        RK_MPI_MB_ReleaseMB(g_state.mb_blk);
    }
    if (g_state.mb_pool != MB_INVALID_POOLID) {
        RK_MPI_MB_DestroyPool(g_state.mb_pool);
    }
    if (g_state.nv12_buffer) {
        free(g_state.nv12_buffer);
    }
    if (g_state.tj_handle) {
        tjDestroy(g_state.tj_handle);
    }

    memset(&g_state, 0, sizeof(g_state));
}

int timelapse_venc_is_active(void) {
    return g_state.initialized;
}

int timelapse_venc_get_frame_count(void) {
    return g_state.frame_count;
}
