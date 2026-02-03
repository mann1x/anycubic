/*
 * Hardware VENC-based Timelapse Encoding
 *
 * Uses RV1106 hardware H.264 encoder directly for timelapse videos.
 * No external dependencies (no ffmpeg required).
 */

#ifndef TIMELAPSE_VENC_H
#define TIMELAPSE_VENC_H

#include <stdint.h>
#include <stddef.h>

/* Initialize VENC timelapse encoder
 * Returns 0 on success, -1 on failure
 */
int timelapse_venc_init(int width, int height, int fps);

/* Add a JPEG frame to the timelapse
 * Decodes JPEG, encodes to H.264, writes to MP4
 * Returns 0 on success, -1 on failure
 */
int timelapse_venc_add_frame(const uint8_t *jpeg_data, size_t jpeg_size);

/* Finish timelapse and write final MP4 file
 * output_path: Full path for the output MP4 file
 * Returns 0 on success, -1 on failure
 */
int timelapse_venc_finish(const char *output_path);

/* Cancel timelapse without creating output file
 * Cleans up all resources
 */
void timelapse_venc_cancel(void);

/* Check if VENC timelapse is currently active */
int timelapse_venc_is_active(void);

/* Get current frame count */
int timelapse_venc_get_frame_count(void);

#endif /* TIMELAPSE_VENC_H */
