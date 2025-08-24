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

/* Video callbacks (existing) */
void sheepbridge_set_video_cb(retro_video_refresh_t cb);
void sheepbridge_set_input_cb(retro_input_poll_t poll_cb, retro_input_state_t state_cb);
void sheepbridge_run_frame(void);
void sheepbridge_submit_frame(const void *pixels, unsigned width, unsigned height, size_t pitch);
void sheepbridge_signal_frame(void);

/* Audio callbacks: bridge will buffer audio pushed by platform backends.
 * - samples: pointer to signed 16-bit interleaved stereo samples (L R L R ...)
 * - frames: number of sample frames (one frame = 2 samples for stereo)
 */
void sheepbridge_set_audio_cb(retro_audio_sample_t cb, retro_audio_sample_batch_t cb_batch);
void sheepbridge_store_audio_samples(const int16_t *samples, size_t frames);
void sheepbridge_set_sample_rate(unsigned rate);

/* Input helper:
 * - The libretro wrapper will call sheepbridge_set_input_cb() with
 *   the libretro poll/state callbacks. The bridge will poll and forward
 *   selected input state into SheepShaver each frame.
 *
 * - Helper injection functions (optional) so other backends can inject
 *   synthetic input into the emulator via the same ADB entry points:
 */
void sheepbridge_inject_key(bool down, int mac_keycode);
void sheepbridge_inject_mouse(int x, int y, unsigned buttons_mask);

/* Nuklear GUI integration:
 * The repo includes a nuklear-based GUI (libretro/nukleargui). The bridge
 * exposes an API to toggle the GUI from the libretro input mapping and to
 * call the GUI per-frame. These functions are optionally implemented in
 * the nukleargui build units and are weak-linked here.
 *
 * - sheepbridge_nuklear_toggle() toggles visibility (if available)
 * - sheepbridge_nuklear_handle() runs per-frame GUI processing (if available)
 */
void sheepbridge_nuklear_toggle(void);
void sheepbridge_nuklear_handle(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBRETRO_BRIDGE_H */