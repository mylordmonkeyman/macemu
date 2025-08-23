/*
 * Small helper for video backend to call into bridge.
 *
 * Video backend should:
 *  - Prepare/convert the source framebuffer into a contiguous pointer 'src'
 *    with known src_pixel_size and src_pitch.
 *  - Call sheepbridge_store_frame(src, width, height, src_pitch, src_pixel_size);
 *  - Optionally call sheepbridge_signal_frame() (store_frame already signals).
 *
 * Keep this helper simple — it just forwards to the bridge.
 */

#include "libretro_bridge.h"
#include <cstdint>
#include <cstddef>

extern "C" {

/* Convenience wrapper: submit raw frame pointer (no conversion); src_pixel_size in bytes */
void video_libretro_submit_frame_raw(const void *src, unsigned width, unsigned height, size_t src_pitch, unsigned src_pixel_size)
{
    sheepbridge_store_frame(src, width, height, src_pitch, src_pixel_size);
}

/* Convenience wrapper: if your platform has 'screen_base' and VModes[] info, call this
 * from video_x.cpp as video_libretro_submit_frame_raw(screen_base, w, h, row_bytes, bytes_per_pixel).
 */

} /* extern C */