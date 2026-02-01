/*
 * FLV Muxer Implementation
 *
 * Ported from Python FLVMuxer class in h264_server.py
 */

#include "flv_mux.h"
#include <stdlib.h>
#include <string.h>

/* Helper: write big-endian 16-bit value */
static inline void write_be16(uint8_t *p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

/* Helper: write big-endian 24-bit value */
static inline void write_be24(uint8_t *p, uint32_t v) {
    p[0] = (v >> 16) & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = v & 0xFF;
}

/* Helper: write big-endian 32-bit value */
static inline void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

/* Helper: write big-endian 64-bit double */
static inline void write_be_double(uint8_t *p, double v) {
    union { double d; uint64_t u; } conv;
    conv.d = v;
    for (int i = 0; i < 8; i++) {
        p[i] = (conv.u >> (56 - i * 8)) & 0xFF;
    }
}

void flv_muxer_init(FLVMuxer *mux, int width, int height, int fps) {
    memset(mux, 0, sizeof(*mux));
    mux->width = width;
    mux->height = height;
    mux->fps = fps > 0 ? fps : 10;
    mux->frame_duration = 1000 / mux->fps;
    mux->timestamp = 0;
    mux->has_sps_pps = 0;
    mux->sps = NULL;
    mux->sps_size = 0;
    mux->pps = NULL;
    mux->pps_size = 0;
}

void flv_muxer_cleanup(FLVMuxer *mux) {
    if (mux->sps) {
        free(mux->sps);
        mux->sps = NULL;
    }
    if (mux->pps) {
        free(mux->pps);
        mux->pps = NULL;
    }
}

void flv_muxer_reset(FLVMuxer *mux) {
    mux->timestamp = 0;
    mux->has_sps_pps = 0;
    /* Keep cached SPS/PPS */
}

size_t flv_create_header(uint8_t *buf, size_t buf_size) {
    if (buf_size < 13) {
        return 0;
    }

    uint8_t *p = buf;

    /* FLV signature */
    *p++ = 'F';
    *p++ = 'L';
    *p++ = 'V';

    /* Version */
    *p++ = 1;

    /* Flags: has video (0x01), no audio */
    *p++ = 0x01;

    /* Header length (9 bytes) */
    write_be32(p, 9);
    p += 4;

    /* PreviousTagSize0 */
    write_be32(p, 0);
    p += 4;

    return 13;
}

/* Create an FLV tag */
static size_t flv_create_tag(uint8_t *buf, size_t buf_size,
                             uint8_t tag_type, const uint8_t *data, size_t data_size,
                             uint32_t timestamp) {
    size_t tag_size = 11 + data_size + 4;  /* header + data + prev_tag_size */

    if (buf_size < tag_size) {
        return 0;
    }

    uint8_t *p = buf;

    /* Tag type */
    *p++ = tag_type;

    /* Data size (24-bit) */
    write_be24(p, (uint32_t)data_size);
    p += 3;

    /* Timestamp (24-bit lower + 8-bit upper) */
    write_be24(p, timestamp & 0xFFFFFF);
    p += 3;
    *p++ = (timestamp >> 24) & 0xFF;

    /* Stream ID (always 0) */
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;

    /* Data */
    memcpy(p, data, data_size);
    p += data_size;

    /* PreviousTagSize */
    write_be32(p, (uint32_t)(11 + data_size));

    return tag_size;
}

size_t flv_create_metadata(FLVMuxer *mux, uint8_t *buf, size_t buf_size) {
    uint8_t amf[FLV_MAX_METADATA_SIZE];
    uint8_t *p = amf;

    /* AMF0 String: "onMetaData" */
    *p++ = 0x02;  /* String type */
    write_be16(p, 10);  /* Length */
    p += 2;
    memcpy(p, "onMetaData", 10);
    p += 10;

    /* AMF0 ECMA Array */
    *p++ = 0x08;
    write_be32(p, 6);  /* Approximate number of elements */
    p += 4;

    /* width */
    write_be16(p, 5);
    p += 2;
    memcpy(p, "width", 5);
    p += 5;
    *p++ = 0x00;  /* Number type */
    write_be_double(p, (double)mux->width);
    p += 8;

    /* height */
    write_be16(p, 6);
    p += 2;
    memcpy(p, "height", 6);
    p += 6;
    *p++ = 0x00;
    write_be_double(p, (double)mux->height);
    p += 8;

    /* framerate */
    write_be16(p, 9);
    p += 2;
    memcpy(p, "framerate", 9);
    p += 9;
    *p++ = 0x00;
    write_be_double(p, (double)mux->fps);
    p += 8;

    /* videocodecid (7 = AVC/H.264) */
    write_be16(p, 12);
    p += 2;
    memcpy(p, "videocodecid", 12);
    p += 12;
    *p++ = 0x00;
    write_be_double(p, 7.0);
    p += 8;

    /* duration (0 for live) */
    write_be16(p, 8);
    p += 2;
    memcpy(p, "duration", 8);
    p += 8;
    *p++ = 0x00;
    write_be_double(p, 0.0);
    p += 8;

    /* encoder */
    write_be16(p, 7);
    p += 2;
    memcpy(p, "encoder", 7);
    p += 7;
    *p++ = 0x02;  /* String type */
    write_be16(p, 10);
    p += 2;
    memcpy(p, "rkmpi_enc", 9);
    p[9] = '\0';  /* Null padding to 10 chars */
    p += 10;

    /* End of object */
    *p++ = 0x00;
    *p++ = 0x00;
    *p++ = 0x09;

    size_t amf_size = p - amf;
    return flv_create_tag(buf, buf_size, FLV_TAG_TYPE_SCRIPT, amf, amf_size, 0);
}

/* Parse NAL units from Annex-B format */
void flv_parse_nal_units(const uint8_t *data, size_t size,
                         nal_callback_t cb, void *ctx) {
    if (!data || size < 4 || !cb) {
        return;
    }

    size_t i = 0;

    while (i < size - 4) {
        /* Find start code (00 00 00 01 or 00 00 01) */
        int start_code_len = 0;

        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 0 && data[i + 3] == 1) {
                start_code_len = 4;
            } else if (data[i + 2] == 1) {
                start_code_len = 3;
            }
        }

        if (start_code_len == 0) {
            i++;
            continue;
        }

        size_t nal_start = i + start_code_len;

        /* Find next start code */
        size_t nal_end = size;
        for (size_t j = nal_start; j < size - 3; j++) {
            if (data[j] == 0 && data[j + 1] == 0) {
                if ((data[j + 2] == 0 && j + 3 < size && data[j + 3] == 1) ||
                    data[j + 2] == 1) {
                    nal_end = j;
                    break;
                }
            }
        }

        if (nal_start < nal_end) {
            const uint8_t *nal = data + nal_start;
            size_t nal_size = nal_end - nal_start;
            int nal_type = nal[0] & 0x1F;
            cb(nal, nal_size, nal_type, ctx);
        }

        i = nal_end;
    }
}

/* Context for NAL parsing callback */
typedef struct {
    FLVMuxer *mux;
    uint8_t *out_buf;
    size_t out_size;
    size_t out_written;
    int found_sps;
    int found_pps;
    int found_idr;
    /* Temporary buffer for video NALs */
    uint8_t *video_nals;
    size_t video_nals_size;
    size_t video_nals_cap;
} MuxContext;

static void nal_collect_callback(const uint8_t *nal, size_t size, int nal_type, void *ctx) {
    MuxContext *mc = (MuxContext *)ctx;
    FLVMuxer *mux = mc->mux;

    switch (nal_type) {
        case NAL_TYPE_SPS:
            /* Cache SPS */
            if (mux->sps) free(mux->sps);
            mux->sps = malloc(size);
            if (mux->sps) {
                memcpy(mux->sps, nal, size);
                mux->sps_size = size;
            }
            mc->found_sps = 1;
            break;

        case NAL_TYPE_PPS:
            /* Cache PPS */
            if (mux->pps) free(mux->pps);
            mux->pps = malloc(size);
            if (mux->pps) {
                memcpy(mux->pps, nal, size);
                mux->pps_size = size;
            }
            mc->found_pps = 1;
            break;

        case NAL_TYPE_IDR:
            mc->found_idr = 1;
            /* Fall through to add to video NALs */
            /* FALLTHROUGH */

        case NAL_TYPE_SLICE:
        case NAL_TYPE_SEI:
        default:
            /* Add to video NALs buffer (length-prefixed) */
            if (nal_type != NAL_TYPE_SPS && nal_type != NAL_TYPE_PPS) {
                size_t needed = 4 + size;
                if (mc->video_nals_size + needed <= mc->video_nals_cap) {
                    /* Write 4-byte length prefix (big-endian) */
                    uint8_t *p = mc->video_nals + mc->video_nals_size;
                    write_be32(p, (uint32_t)size);
                    memcpy(p + 4, nal, size);
                    mc->video_nals_size += needed;
                }
            }
            break;
    }
}

/* Create AVC decoder configuration record */
static size_t create_avc_decoder_config(FLVMuxer *mux, uint8_t *buf, size_t buf_size) {
    if (!mux->sps || mux->sps_size < 4 || !mux->pps || mux->pps_size == 0) {
        return 0;
    }

    size_t config_size = 11 + mux->sps_size + mux->pps_size;
    if (buf_size < config_size) {
        return 0;
    }

    uint8_t *p = buf;

    /* AVC decoder configuration record */
    *p++ = 0x01;                /* configurationVersion */
    *p++ = mux->sps[1];         /* AVCProfileIndication */
    *p++ = mux->sps[2];         /* profile_compatibility */
    *p++ = mux->sps[3];         /* AVCLevelIndication */
    *p++ = 0xFF;                /* 6 bits reserved + 2 bits NAL length size - 1 */

    /* SPS */
    *p++ = 0xE1;                /* 3 bits reserved + 5 bits SPS count */
    write_be16(p, (uint16_t)mux->sps_size);
    p += 2;
    memcpy(p, mux->sps, mux->sps_size);
    p += mux->sps_size;

    /* PPS */
    *p++ = 0x01;                /* PPS count */
    write_be16(p, (uint16_t)mux->pps_size);
    p += 2;
    memcpy(p, mux->pps, mux->pps_size);
    p += mux->pps_size;

    return p - buf;
}

size_t flv_mux_h264(FLVMuxer *mux, const uint8_t *h264_data, size_t h264_size,
                    uint8_t *out_buf, size_t out_size) {
    if (!mux || !h264_data || h264_size == 0 || !out_buf || out_size == 0) {
        return 0;
    }

    /* Temporary buffer for video NALs */
    uint8_t *video_nals = malloc(h264_size + 1024);
    if (!video_nals) {
        return 0;
    }

    MuxContext mc = {
        .mux = mux,
        .out_buf = out_buf,
        .out_size = out_size,
        .out_written = 0,
        .found_sps = 0,
        .found_pps = 0,
        .found_idr = 0,
        .video_nals = video_nals,
        .video_nals_size = 0,
        .video_nals_cap = h264_size + 1024
    };

    /* Parse all NAL units */
    flv_parse_nal_units(h264_data, h264_size, nal_collect_callback, &mc);

    uint8_t *out_ptr = out_buf;
    size_t out_remaining = out_size;

    /* Send AVC decoder config if we have SPS/PPS and haven't sent it yet */
    if (mux->sps && mux->pps && !mux->has_sps_pps) {
        uint8_t config[256];
        size_t config_size = create_avc_decoder_config(mux, config, sizeof(config));

        if (config_size > 0) {
            /* Create video tag with decoder config */
            uint8_t video_data[512];
            uint8_t *vp = video_data;

            *vp++ = 0x17;           /* Keyframe + AVC */
            *vp++ = 0x00;           /* AVC sequence header */
            *vp++ = 0x00;           /* Composition time (3 bytes) */
            *vp++ = 0x00;
            *vp++ = 0x00;
            memcpy(vp, config, config_size);
            vp += config_size;

            size_t tag_size = flv_create_tag(out_ptr, out_remaining,
                                             FLV_TAG_TYPE_VIDEO, video_data,
                                             vp - video_data, 0);
            if (tag_size > 0) {
                out_ptr += tag_size;
                out_remaining -= tag_size;
                mux->has_sps_pps = 1;
            }
        }
    }

    /* Send video NALs if we have any */
    if (mc.video_nals_size > 0) {
        /* Create video tag */
        size_t video_data_size = 5 + mc.video_nals_size;
        uint8_t *video_data = malloc(video_data_size);

        if (video_data) {
            uint8_t *vp = video_data;

            /* Frame type + codec */
            *vp++ = mc.found_idr ? 0x17 : 0x27;  /* Keyframe or inter-frame + AVC */
            *vp++ = 0x01;                         /* AVC NALU */
            *vp++ = 0x00;                         /* Composition time (3 bytes) */
            *vp++ = 0x00;
            *vp++ = 0x00;

            /* Copy length-prefixed NALs */
            memcpy(vp, mc.video_nals, mc.video_nals_size);

            size_t tag_size = flv_create_tag(out_ptr, out_remaining,
                                             FLV_TAG_TYPE_VIDEO, video_data,
                                             video_data_size, mux->timestamp);
            if (tag_size > 0) {
                out_ptr += tag_size;
                mux->timestamp += mux->frame_duration;
            }

            free(video_data);
        }
    }

    free(video_nals);
    return out_ptr - out_buf;
}
