#ifndef AUDIO_LIBRETRO_SHIM_H
#define AUDIO_LIBRETRO_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Send audio to libretro bridge.
 * - samples_or_buf: pointer to the frame buffer (format described by sample_size and channels)
 * - frames: number of frames (one frame = one sample per channel)
 * - sample_size: bytes per sample per channel (1,2,4)
 * - channels: channel count in input data (1 or 2)
 *
 * This function will convert to signed int16 interleaved stereo if necessary
 * and call sheepbridge_store_audio_samples() when built with -DLIBRETRO.
 */
void send_audio_to_host(const void *samples_or_buf, size_t frames, int sample_size, int channels);

/* Convenience wrapper for already-s16-stereo buffers */
void send_s16_stereo_to_host(const int16_t *samples, size_t frames);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_LIBRETRO_SHIM_H */