/*
 * libretro bridge (frame-buffered)
 *
 * This implementation stores one full frame (resizing as required) when the video
 * backend calls sheepbridge_store_frame(). The libretro thread (sheepbridge_run_frame)
 * will wait for a frame signal, then call the libretro video callback with the
 * stored buffer. This ensures libretro callbacks are invoked from retro_run().
 *
 * Audio has to be handled similarly (buffered) — not implemented here, see TODOs.
 */

#include "libretro_bridge.h"
#include "libretro.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cstring>
#include <cstdlib>

static retro_video_refresh_t s_video_cb = NULL;
static retro_audio_sample_t s_audio_cb = NULL;
static retro_audio_sample_batch_t s_audio_batch_cb = NULL;
static retro_input_poll_t s_input_poll_cb = NULL;
static retro_input_state_t s_input_state_cb = NULL;

/* Frame buffer storage (owned by bridge) */
static std::mutex s_frame_mutex;
static std::condition_variable s_frame_cv;
static bool s_frame_available = false;
static unsigned s_frame_width = 0;
static unsigned s_frame_height = 0;
static size_t s_frame_pitch = 0; /* bytes per row in our stored buffer */
static uint8_t *s_frame_buffer = nullptr; /* RGBA 8-bit per channel layout */

/* Lifecycle flag */
static std::atomic<bool> s_initialised(false);

extern "C" bool sheepbridge_init(const char *game_path, unsigned int ram_mb)
{
    (void)game_path;
    (void)ram_mb;
    if (s_initialised.load()) return true;

    /* TODO: call SheepShaver Init functions here. For now just mark init. */
    s_initialised.store(true);
    return true;
}

extern "C" void sheepbridge_deinit(void)
{
    if (!s_initialised.load()) return;
    s_initialised.store(false);

    /* Wake up any waiters so they can exit */
    {
        std::lock_guard<std::mutex> lk(s_frame_mutex);
        s_frame_available = true;
    }
    s_frame_cv.notify_all();

    /* free frame buffer */
    if (s_frame_buffer) {
        free(s_frame_buffer);
        s_frame_buffer = nullptr;
    }
    s_frame_width = s_frame_height = 0;
    s_frame_pitch = 0;
}

extern "C" void sheepbridge_set_video_cb(retro_video_refresh_t cb)
{
    s_video_cb = cb;
}

extern "C" void sheepbridge_set_audio_cb(retro_audio_sample_t cb, retro_audio_sample_batch_t cb_batch)
{
    s_audio_cb = cb;
    s_audio_batch_cb = cb_batch;
}

extern "C" void sheepbridge_set_input_cb(retro_input_poll_t poll_cb, retro_input_state_t state_cb)
{
    s_input_poll_cb = poll_cb;
    s_input_state_cb = state_cb;
}

/*
 * Called by the video backend to store a completed frame into the bridge.
 *
 * Requirements:
 *  - src: pointer to source pixels (one frame)
 *  - width, height: frame dimensions
 *  - src_pitch: bytes per source row
 *  - src_pixel_size: bytes per pixel in source buffer (commonly 4)
 *
 * The bridge stores frames in a 32-bit RGBA (8-bits per channel) buffer in host
 * native byte order. If the source is already 4 bytes per pixel and row size
 * equals width*4, we copy the whole block; otherwise we copy per row.
 *
 * NOTE: This routine is intended to be called from the emulator/video thread.
 * It must be fast; it performs a copy. If you want to optimize, arrange for
 * SheepShaver to produce 32-bit frames directly into the bridge's buffer.
 */
extern "C" void sheepbridge_store_frame(const void *src, unsigned width, unsigned height, size_t src_pitch, unsigned src_pixel_size)
{
    if (!s_initialised.load() || src == nullptr || width == 0 || height == 0) return;

    const uint8_t *s = reinterpret_cast<const uint8_t*>(src);

    /* Ensure internal buffer is large enough */
    size_t out_stride = (size_t)width * 4; /* RGBA */
    size_t needed = out_stride * (size_t)height;

    {
        std::lock_guard<std::mutex> lk(s_frame_mutex);
        if (!s_frame_buffer || width != s_frame_width || height != s_frame_height || s_frame_pitch != out_stride) {
            /* (re)allocate */
            if (s_frame_buffer) free(s_frame_buffer);
            s_frame_buffer = (uint8_t*)malloc(needed);
            if (!s_frame_buffer) {
                /* allocation failed — drop frame */
                s_frame_width = s_frame_height = 0;
                s_frame_pitch = 0;
                return;
            }
            s_frame_width = width;
            s_frame_height = height;
            s_frame_pitch = out_stride;
        }

        /* Copy / convert per-row.
         *
         * Current conservative behavior:
         *  - If src_pixel_size == 4 and src_pitch == width*4: do single block memcpy.
         *  - Otherwise copy min(src_pixel_size,width*4) bytes per row and zero the rest.
         *
         * This does not perform endian-channel reordering or 16-bit->32-bit color conversion.
         * If you need a specific conversion (BE ARGB -> RGBA or 16bpp->32bpp), implement it here.
         */
        uint8_t *dst = s_frame_buffer;
        if (src_pixel_size == 4 && src_pitch == out_stride) {
            memcpy(dst, s, needed);
        } else {
            for (unsigned y = 0; y < height; ++y) {
                const uint8_t *row_src = s + (size_t)y * src_pitch;
                uint8_t *row_dst = dst + (size_t)y * out_stride;
                size_t copy_bytes = (size_t)src_pixel_size * (size_t)width;
                if (copy_bytes > out_stride) copy_bytes = out_stride;
                memcpy(row_dst, row_src, copy_bytes);
                if (copy_bytes < out_stride) memset(row_dst + copy_bytes, 0, out_stride - copy_bytes);
            }
        }

        /* mark frame available */
        s_frame_available = true;
    }

    /* Notify the retro_run waiter */
    s_frame_cv.notify_one();
}

/* Called by the video backend after storing a frame — optionally useful, but
 * sheepbridge_store_frame already sets s_frame_available and notifies.
 * We keep it for compatibility with older snippets.
 */
extern "C" void sheepbridge_signal_frame(void)
{
    {
        std::lock_guard<std::mutex> lk(s_frame_mutex);
        s_frame_available = true;
    }
    s_frame_cv.notify_one();
}

/*
 * Called by retro_run — wait for a stored frame and then call the libretro
 * video callback from this thread. Timeout is small (1s) to avoid blocking forever.
 */
extern "C" void sheepbridge_run_frame(void)
{
    if (!s_initialised.load()) return;

    std::unique_lock<std::mutex> lk(s_frame_mutex);
    if (!s_frame_available) {
        using namespace std::chrono_literals;
        s_frame_cv.wait_for(lk, 1000ms);
    }

    if (!s_frame_available) {
        /* timed out — nothing to display */
        return;
    }

    /* If we have a video callback and a stored buffer, call it from this thread */
    if (s_video_cb && s_frame_buffer && s_frame_width > 0 && s_frame_height > 0) {
        /* Call the libretro video callback with our owned buffer */
        /* Note: libretro video callback expects const void* pixels, width, height, pitch */
        s_video_cb(s_frame_buffer, s_frame_width, s_frame_height, (unsigned)s_frame_pitch);
    }

    /* Mark consumed */
    s_frame_available = false;
}