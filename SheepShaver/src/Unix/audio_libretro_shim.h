#ifndef AUDIO_LIBRETRO_SHIM_H
#define AUDIO_LIBRETRO_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert-and-forward helper for libretro builds.
 * - buf: input samples
 * - frames: number of frames (one frame = one sample per channel)
 * - sample_size: bytes per sample per channel (1,2,4)
 * - channels: number of channels in input (1 or 2)
 *
 * This converts many common formats to signed 16-bit interleaved stereo
 * and forwards to sheepbridge_store_audio_samples() when compiled with -DLIBRETRO.
 */
void send_audio_to_host(const void *buf, size_t frames, int sample_size, int channels);

/* Fast path if you already have s16 interleaved stereo frames */
void send_s16_stereo_to_host(const int16_t *samples, size_t frames);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_LIBRETRO_SHIM_H */