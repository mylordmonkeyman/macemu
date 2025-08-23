/*
 * libretro bridge - background emulation + frame signal implementation
 *
 * This file implements:
 *  - submission of frames to the libretro video callback
 *  - a condition variable used by the libretro thread to wait until a frame
 *    is produced by the existing SheepShaver video backend
 *
 * Build: compile this cpp into the libretro core and link with SheepShaver.
 * Make sure to add -I... include path to find libretro.h and other headers.
 */

#include "libretro_bridge.h"
#include "libretro.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cstring>

static retro_video_refresh_t s_video_cb = NULL;
static retro_audio_sample_t s_audio_cb = NULL;
static retro_audio_sample_batch_t s_audio_batch_cb = NULL;
static retro_input_poll_t s_input_poll_cb = NULL;
static retro_input_state_t s_input_state_cb = NULL;

/* Frame signaling state */
static std::mutex s_frame_mutex;
static std::condition_variable s_frame_cv;
static bool s_frame_available = false;
static std::atomic<bool> s_initialised(false);

/* Lifecycle stubs - replace or extend with actual SheepShaver init/shutdown */
extern "C" bool sheepbridge_init(const char *game_path, unsigned int ram_mb)
{
    (void)game_path;
    (void)ram_mb;
    if (s_initialised.load()) return true;

    /* TODO: initialize SheepShaver internals (prefs, ROMs, disk images, threads).
     * For approach #1 we typically start the regular SheepShaver emulation thread(s)
     * so that the engine runs continuously and the video backend will call
     * sheepbridge_submit_frame() when a frame is ready.
     */
    s_initialised.store(true);
    return true;
}

extern "C" void sheepbridge_deinit(void)
{
    if (!s_initialised.load()) return;

    /* TODO: shut down SheepShaver threads cleanly */
    s_initialised.store(false);

    /* Wake any waiter so it can exit gracefully */
    {
        std::lock_guard<std::mutex> lk(s_frame_mutex);
        s_frame_available = true;
    }
    s_frame_cv.notify_all();
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

/* Called by video backend to send the completed frame to the frontend */
extern "C" void sheepbridge_submit_frame(const void *pixels, unsigned width, unsigned height, size_t pitch)
{
    if (s_video_cb) {
        /* libretro expects the buffer to be stable for the duration of callback.
         * We forward directly; if the source buffer is reused afterwards, make a copy
         * before calling this function, or arrange to call this function before reuse.
         */
        s_video_cb(pixels, width, height, pitch);
    }
}

/* Signal that a frame has been submitted (wake any waiting retro_run) */
extern "C" void sheepbridge_signal_frame(void)
{
    {
        std::lock_guard<std::mutex> lk(s_frame_mutex);
        s_frame_available = true;
    }
    s_frame_cv.notify_one();
}

/* Wait for a frame to be produced (called from retro_run). Returns after a frame
 * has been signaled or after a timeout.
 *
 * Timeout is set to 1000ms to avoid locking forever if something goes wrong.
 * You can adjust this to match the expected framerate / responsiveness.
 */
extern "C" void sheepbridge_run_frame(void)
{
    if (!s_initialised.load()) return;

    std::unique_lock<std::mutex> lk(s_frame_mutex);
    if (!s_frame_available) {
        /* Wait up to ~1s; typical VBL should arrive much faster (e.g. ~16ms). */
        using namespace std::chrono_literals;
        s_frame_cv.wait_for(lk, 1000ms);
    }
    s_frame_available = false;
}