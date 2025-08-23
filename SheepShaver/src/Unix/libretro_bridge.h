#ifndef LIBRETRO_BRIDGE_H
#define LIBRETRO_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "libretro.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Basic lifecycle */
bool sheepbridge_init(const char *game_path, unsigned int ram_mb);
void sheepbridge_deinit(void);

/* Per-frame control for the background-emulation approach:
 * - sheepbridge_run_frame waits until a frame has been presented by the
 *   video backend (signaled with sheepbridge_signal_frame()) and then returns.
 *   This is called by the libretro core's retro_run path.
 * - sheepbridge_signal_frame is intended to be called by the video backend
 *   immediately after it calls sheepbridge_submit_frame().
 */
void sheepbridge_set_video_cb(retro_video_refresh_t cb);
void sheepbridge_set_audio_cb(retro_audio_sample_t cb, retro_audio_sample_batch_t cb_batch);
void sheepbridge_set_input_cb(retro_input_poll_t poll_cb, retro_input_state_t state_cb);

void sheepbridge_run_frame(void);

/* Called by platform video code to submit a frame to libretro.
 * - pixels: pointer to pixel data in 32-bit RGBA/native order (preferred)
 * - width/height: frame dimensions
 * - pitch: number of bytes per row (stride)
 *
 * The function will call the stored libretro video callback (if present).
 */
void sheepbridge_submit_frame(const void *pixels, unsigned width, unsigned height, size_t pitch);

/* Signal that a frame has been submitted and the libretro waiter should wake.
 * Typically the video backend calls sheepbridge_submit_frame(...); sheepbridge_signal_frame();
 */
void sheepbridge_signal_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBRETRO_BRIDGE_H */