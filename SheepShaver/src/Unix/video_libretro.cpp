/*
 * Small helpers for platform video code.
 *
 * The platform backend should:
 *  1) Prepare a final 32-bit pixel buffer (RGBA/native) for the frame.
 *  2) Call sheepbridge_submit_frame(buf, w, h, pitch);
 *  3) Immediately call sheepbridge_signal_frame();
 *
 * If the platform provides frames in a non-32-bit format, convert once here
 * or convert in the platform code ahead of calling submit_frame.
 */

#include "libretro_bridge.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

/* This file intentionally small — it only provides helper stubs. Prefer calling
 * sheepbridge_submit_frame() directly from the platform backend once it has
 * assembled the final framebuffer.
 */

extern "C" {

/* If you want a convenience function that converts from SheepShaver's internal
 * framebuffer layout to 32-bit RGBA, implement it here and call it from video_x.cpp.
 *
 * Example signature:
 *   void video_libretro_submit_converted(const uint8_t *src, unsigned w, unsigned h, unsigned src_pitch);
 *
 * For simplicity we don't add conversions here; do the conversion in the caller
 * and call sheepbridge_submit_frame directly.
 */

} /* extern C */