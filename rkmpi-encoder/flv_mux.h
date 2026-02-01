/*
 * FLV Muxer for H.264 Streams
 *
 * Converts H.264 Annex-B NAL units into FLV format for streaming.
 * Compatible with gkcam's /flv endpoint format.
 */

#ifndef FLV_MUX_H
#define FLV_MUX_H

#include <stdint.h>
#include <stddef.h>

/* FLV tag types */
#define FLV_TAG_TYPE_AUDIO   8
#define FLV_TAG_TYPE_VIDEO   9
#define FLV_TAG_TYPE_SCRIPT  18

/* H.264 NAL unit types */
#define NAL_TYPE_SLICE       1
#define NAL_TYPE_IDR         5
#define NAL_TYPE_SEI         6
#define NAL_TYPE_SPS         7
#define NAL_TYPE_PPS         8

/* Maximum sizes */
#define FLV_MAX_HEADER_SIZE    256
#define FLV_MAX_METADATA_SIZE  512
#define FLV_MAX_TAG_SIZE       (256 * 1024)

/* FLV Muxer state */
typedef struct {
    int width;
    int height;
    int fps;
    uint32_t timestamp;         /* Current timestamp in ms */
    uint32_t frame_duration;    /* Duration per frame in ms */
    int has_sps_pps;            /* Have we sent decoder config? */
    uint8_t *sps;               /* Cached SPS NAL (without start code) */
    size_t sps_size;
    uint8_t *pps;               /* Cached PPS NAL (without start code) */
    size_t pps_size;
} FLVMuxer;

/* Initialize muxer with video parameters */
void flv_muxer_init(FLVMuxer *mux, int width, int height, int fps);

/* Cleanup muxer resources */
void flv_muxer_cleanup(FLVMuxer *mux);

/* Reset muxer state (for new connection) */
void flv_muxer_reset(FLVMuxer *mux);

/* Create FLV file header (13 bytes) */
size_t flv_create_header(uint8_t *buf, size_t buf_size);

/* Create FLV metadata tag (onMetaData) */
size_t flv_create_metadata(FLVMuxer *mux, uint8_t *buf, size_t buf_size);

/* Mux H.264 Annex-B data into FLV tags
 * Input: H.264 data with 00 00 00 01 start codes
 * Output: FLV tags (may be multiple: decoder config + video)
 * Returns: bytes written to output buffer, 0 if no output
 */
size_t flv_mux_h264(FLVMuxer *mux, const uint8_t *h264_data, size_t h264_size,
                    uint8_t *out_buf, size_t out_size);

/* Parse NAL units from Annex-B format
 * Callback is called for each NAL unit (without start code)
 */
typedef void (*nal_callback_t)(const uint8_t *nal, size_t size, int nal_type, void *ctx);
void flv_parse_nal_units(const uint8_t *data, size_t size, nal_callback_t cb, void *ctx);

#endif /* FLV_MUX_H */
