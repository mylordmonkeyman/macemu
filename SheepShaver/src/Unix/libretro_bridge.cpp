/*
 * libretro bridge (frame-buffered + audio FIFO + input forwarding)
 *
 * This file implements:
 * - storage of libretro callbacks (video/audio/input)
 * - an audio FIFO that platform backends push into
 * - per-frame draining of audio + forwarding of input polled from the frontend
 *
 * Input forwarding:
 * - The libretro frontend provides two callbacks:
 *    - poll_cb: called to poll the input devices
 *    - state_cb: queried for each control (returns int state)
 *
 * - We call the poll callback at the start of each frame and sample a small
 *   set of controls (D-pad, common buttons, pointer) and forward edges & axis
 *   into SheepShaver via existing ADB entry points.
 *
 * NOTE: This implementation implements a minimal, useful mapping. You can
 * extend the mappings in sheepbridge_process_input() to cover more keys and
 * controls as you need.
 */

#include "libretro_bridge.h"
#include "libretro.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <vector>

/* libretro callbacks stored by the frontend */
static retro_video_refresh_t s_video_cb = NULL;
static retro_audio_sample_t s_audio_cb = NULL; /* single-sample callback (rare) */
static retro_audio_sample_batch_t s_audio_batch_cb = NULL; /* preferred */
static retro_input_poll_t s_input_poll_cb = NULL;
static retro_input_state_t s_input_state_cb = NULL;

/* Frame/video state */
static std::mutex s_frame_mutex;
static std::condition_variable s_frame_cv;
static bool s_frame_available = false;
static unsigned s_frame_width = 0;
static unsigned s_frame_height = 0;
static size_t s_frame_pitch = 0;
static uint8_t *s_frame_buffer = nullptr;

/* Audio FIFO (s16 interleaved stereo) */
static std::mutex s_audio_mutex;
static std::vector<int16_t> s_audio_buf; /* ring buffer storage (samples) */
static size_t s_audio_buf_size_frames = 0; /* capacity in frames (stereo frames) */
static size_t s_audio_read_idx = 0;  /* index in samples */
static size_t s_audio_write_idx = 0; /* index in samples */
static unsigned s_sample_rate = 44100; /* default sample rate */

/* input state caches (to detect edges) */
static bool s_prev_dpad_up = false;
static bool s_prev_dpad_down = false;
static bool s_prev_dpad_left = false;
static bool s_prev_dpad_right = false;
static bool s_prev_btn_a = false;
static bool s_prev_btn_b = false;
static bool s_prev_btn_x = false;
static bool s_prev_btn_y = false;
static bool s_prev_mouse_left = false;
static bool s_prev_mouse_right = false;
static int  s_last_mouse_x = 0;
static int  s_last_mouse_y = 0;

/* lifecycle flag */
static std::atomic<bool> s_initialised(false);

/* Helper: convert frames -> samples (samples == frames * 2 for stereo) */
static inline size_t frames_to_samples(size_t frames) { return frames * 2; }

/* Ensure audio capacity (frames) */
static void ensure_audio_capacity_frames(size_t frames_cap)
{
    std::lock_guard<std::mutex> lk(s_audio_mutex);
    if (frames_cap <= s_audio_buf_size_frames) return;
    size_t new_frames = frames_cap;
    s_audio_buf_size_frames = new_frames;
    s_audio_buf.assign(frames_to_samples(s_audio_buf_size_frames), 0);
    s_audio_read_idx = s_audio_write_idx = 0;
}

/* Called by platform audio backend to push audio into the bridge (s16 stereo) */
extern "C" void sheepbridge_store_audio_samples(const int16_t *samples, size_t frames)
{
    if (!samples || frames == 0) return;
    std::lock_guard<std::mutex> lk(s_audio_mutex);
    if (s_audio_buf.empty()) return;
    size_t samples_to_write = frames_to_samples(frames);
    for (size_t i = 0; i < samples_to_write; ++i) {
        s_audio_buf[s_audio_write_idx] = samples[i];
        s_audio_write_idx = (s_audio_write_idx + 1) % s_audio_buf.size();
        /* If buffer full (write catches up to read), advance read to avoid overwrite */
        if (s_audio_write_idx == s_audio_read_idx) {
            /* drop oldest sample (advance read by 1 sample) */
            s_audio_read_idx = (s_audio_read_idx + 1) % s_audio_buf.size();
        }
    }
}

/* set sample rate (optional) */
extern "C" void sheepbridge_set_sample_rate(unsigned rate)
{
    s_sample_rate = rate;
}

/* Set audio callbacks from frontend */
extern "C" void sheepbridge_set_audio_cb(retro_audio_sample_t cb, retro_audio_sample_batch_t cb_batch)
{
    s_audio_cb = cb;
    s_audio_batch_cb = cb_batch;
}

/* Drain up to `max_frames` frames from FIFO into dest (dest must be large enough).
 * Returns number of frames drained.
 */
static size_t drain_audio_frames(int16_t *dest, size_t max_frames)
{
    std::lock_guard<std::mutex> lk(s_audio_mutex);
    if (s_audio_buf.empty()) return 0;

    /* compute available samples */
    size_t available_samples;
    if (s_audio_write_idx >= s_audio_read_idx)
        available_samples = s_audio_write_idx - s_audio_read_idx;
    else
        available_samples = s_audio_buf.size() - s_audio_read_idx + s_audio_write_idx;
    size_t available_frames = available_samples / 2;
    size_t take_frames = (max_frames < available_frames) ? max_frames : available_frames;
    size_t samples_to_copy = frames_to_samples(take_frames);

    for (size_t i = 0; i < samples_to_copy; ++i) {
        dest[i] = s_audio_buf[(s_audio_read_idx + i) % s_audio_buf.size()];
    }
    s_audio_read_idx = (s_audio_read_idx + samples_to_copy) % s_audio_buf.size();
    return take_frames;
}

/* On each retro_run call we will drain available audio and pass to libretro.
 * We aim to send reasonable batch sizes (e.g. up to 2048 frames / call), but
 * we will loop until FIFO empty or until we've sent a configured limit.
 */
static void drain_and_send_audio()
{
    if (!s_audio_batch_cb && !s_audio_cb) return;
    const size_t MAX_SEND_FRAMES = 2048;
    int16_t tmpbuf[MAX_SEND_FRAMES * 2]; /* stereo */

    while (true) {
        size_t drained = drain_audio_frames(tmpbuf, MAX_SEND_FRAMES);
        if (drained == 0) break;

        if (s_audio_batch_cb) {
            s_audio_batch_cb(tmpbuf, drained);
        } else if (s_audio_cb) {
            for (size_t f = 0; f < drained; ++f) {
                s_audio_cb(tmpbuf[2 * f], tmpbuf[2 * f + 1]);
            }
        }
    }
}

/* Forward video into libretro (called by platform code) */
extern "C" void sheepbridge_submit_frame(const void *src, unsigned width, unsigned height, size_t pitch)
{
    {
        std::lock_guard<std::mutex> lk(s_frame_mutex);
        if (!s_frame_buffer || width * height * 4 > s_frame_width * s_frame_height * 4) {
            /* allocate or reallocate a frame buffer large enough */
            delete [] s_frame_buffer;
            s_frame_width = width;
            s_frame_height = height;
            s_frame_pitch = pitch;
            s_frame_buffer = new uint8_t[width * height * 4];
        }
        /* copy incoming frame (assume already in acceptable format) */
        if (src && s_frame_buffer) {
            memcpy(s_frame_buffer, src, height * pitch);
        }
        s_frame_available = true;
    }
    s_frame_cv.notify_one();
}

/* Notify bridge that a frame is ready (no copy) */
extern "C" void sheepbridge_signal_frame(void)
{
    {
        std::lock_guard<std::mutex> lk(s_frame_mutex);
        s_frame_available = true;
    }
    s_frame_cv.notify_one();
}

/* Input injection helpers: these call into SheepShaver input stack.
 * We declare the minimal externs used by the existing SDL input code so
 * forwarding ends up in the same code paths (ADB* functions).
 *
 * These externs are present elsewhere in the SheepShaver tree (adb.cpp).
 * We don't include adb headers here to avoid header dependency; declare as extern.
 */
extern "C" {
/* keycodes expected by ADBKeyDown/Up are SheepShaver internal Mac keycodes */
void ADBKeyDown(int key);
void ADBKeyUp(int key);
void ADBMouseDown(int button); /* 0=left,1=right,2=middle */
void ADBMouseUp(int button);
void ADBMouseMoved(int x, int y);
}

/* Called by wrapper to set the libretro input callbacks (poll + state) */
extern "C" void sheepbridge_set_input_cb(retro_input_poll_t poll_cb, retro_input_state_t state_cb)
{
    s_input_poll_cb = poll_cb;
    s_input_state_cb = state_cb;
}

/* Simple input forwarding implementation.
 * - Calls the poll callback (if present)
 * - Samples a small set of controls (D-pad/up/down/left/right, A/B/X/Y)
 * - Samples pointer device (if available)
 * - Generates edge events for key/button down/up and axis events for mouse
 *
 * This mapping is intentionally conservative and focuses on the controls
 * that are most useful to drive the Mac UI. You can extend this mapping.
 */
static void sheepbridge_process_input()
{
    if (!s_input_poll_cb || !s_input_state_cb) return;

    /* Ask frontend to poll devices (libretro convention) */
    s_input_poll_cb();

    /* --- Gamepad / D-pad / buttons mapping --- */
    bool dpad_up = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
    bool dpad_down = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
    bool dpad_left = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
    bool dpad_right = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);

    bool btn_a = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    bool btn_b = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
    bool btn_x = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
    bool btn_y = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);

    /* Map D-pad to Mac cursor keys (Apple keycodes):
     * Up: 0x7E, Down: 0x7D, Left: 0x7B, Right: 0x7C
     * Map A button -> Return (0x24), B -> Space (0x31)
     *
     * These Mac keycodes are the same values used throughout the existing
     * event->ADB mapping in the SDL backend; adjust if you rely on a different map.
     */
    const int MAC_KEY_UP     = 0x7E;
    const int MAC_KEY_DOWN   = 0x7D;
    const int MAC_KEY_LEFT   = 0x7B;
    const int MAC_KEY_RIGHT  = 0x7C;
    const int MAC_KEY_RETURN = 0x24;
    const int MAC_KEY_SPACE  = 0x31;

    /* D-pad edges */
    if (dpad_up && !s_prev_dpad_up) ADBKeyDown(MAC_KEY_UP);
    if (!dpad_up && s_prev_dpad_up) ADBKeyUp(MAC_KEY_UP);
    if (dpad_down && !s_prev_dpad_down) ADBKeyDown(MAC_KEY_DOWN);
    if (!dpad_down && s_prev_dpad_down) ADBKeyUp(MAC_KEY_DOWN);
    if (dpad_left && !s_prev_dpad_left) ADBKeyDown(MAC_KEY_LEFT);
    if (!dpad_left && s_prev_dpad_left) ADBKeyUp(MAC_KEY_LEFT);
    if (dpad_right && !s_prev_dpad_right) ADBKeyDown(MAC_KEY_RIGHT);
    if (!dpad_right && s_prev_dpad_right) ADBKeyUp(MAC_KEY_RIGHT);

    s_prev_dpad_up = dpad_up;
    s_prev_dpad_down = dpad_down;
    s_prev_dpad_left = dpad_left;
    s_prev_dpad_right = dpad_right;

    /* Buttons edges */
    if (btn_a && !s_prev_btn_a) ADBKeyDown(MAC_KEY_RETURN);
    if (!btn_a && s_prev_btn_a) ADBKeyUp(MAC_KEY_RETURN);
    if (btn_b && !s_prev_btn_b) ADBKeyDown(MAC_KEY_SPACE);
    if (!btn_b && s_prev_btn_b) ADBKeyUp(MAC_KEY_SPACE);

    s_prev_btn_a = btn_a;
    s_prev_btn_b = btn_b;

    /* --- Pointer (mouse) support --- */
    /* Sample pointer device if frontend exposes it. Many frontends expose a pointer
     * device with RETRO_DEVICE_POINTER and IDs RETRO_DEVICE_ID_POINTER_X/Y. If not
     * available, these calls will usually return 0.
     */
    int mx = 0, my = 0;
#if defined(RETRO_DEVICE_POINTER)
    /* libretro pointer values are typically returned as integer coordinates */
    mx = s_input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
    my = s_input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
#endif

    /* Buttons: some frontends provide RETRO_DEVICE_MOUSE device */
#if defined(RETRO_DEVICE_MOUSE)
    bool mleft = !!s_input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
    bool mright = !!s_input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
    bool mmiddle = !!s_input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE);
#else
    /* fallback: map A/B to left/right mouse */
    bool mleft = btn_a;
    bool mright = btn_b;
    bool mmiddle = btn_x;
#endif

    /* Forward mouse movement if changed (simple absolute coordinates) */
    if (mx != s_last_mouse_x || my != s_last_mouse_y) {
        ADBMouseMoved(mx, my);
        s_last_mouse_x = mx; s_last_mouse_y = my;
    }

    /* Mouse button edges */
    if (mleft && !s_prev_mouse_left) ADBMouseDown(0);
    if (!mleft && s_prev_mouse_left) ADBMouseUp(0);
    if (mright && !s_prev_mouse_right) ADBMouseDown(1);
    if (!mright && s_prev_mouse_right) ADBMouseUp(1);

    s_prev_mouse_left = mleft;
    s_prev_mouse_right = mright;

    /* NOTE:
     * - Keyboard handling (full keyboard scancode mapping) is intentionally
     *   not attempted here because SheepShaver's internal mapping pipeline
     *   expects Mac scan-codes and the project's SDL key translation tables
     *   are already fairly involved. If you require full keyboard support via
     *   libretro's RETRO_DEVICE_KEYBOARD, we can add a mapping layer that
     *   translates libretro keycodes to the Mac scancodes used by ADBKeyDown.
     */
}

/* Process video frame & audio inside the libretro thread.
 * We added input forwarding at the start of the frame so input is sampled
 * from the frontend and forwarded into SheepShaver before the emulation step.
 */
extern "C" void sheepbridge_run_frame(void)
{
    if (!s_initialised.load()) return;

    /* Poll and forward input first (so the emulation sees the latest state) */
    sheepbridge_process_input();

    /* Wait for a frame to present (video) but with a timeout in case video stalls */
    {
        std::unique_lock<std::mutex> lk(s_frame_mutex);
        if (!s_frame_available) {
            using namespace std::chrono_literals;
            s_frame_cv.wait_for(lk, 1000ms);
        }
        /* present video if available */
        if (s_frame_available && s_video_cb && s_frame_buffer && s_frame_width > 0 && s_frame_height > 0) {
            s_video_cb(s_frame_buffer, s_frame_width, s_frame_height, (unsigned)s_frame_pitch);
        }
        s_frame_available = false;
    }

    /* Now drain audio FIFO and call the libretro audio batch callback from libretro thread */
    drain_and_send_audio();
}

/* Initialize bridge (alloc audio buffer etc.) */
extern "C" bool sheepbridge_init(const char *game_path, unsigned int ram_mb)
{
    (void)game_path; (void)ram_mb;
    if (s_initialised.load()) return true;

    /* allocate initial audio buffer capacity (frames) */
    {
        std::lock_guard<std::mutex> lk(s_audio_mutex);
        s_audio_buf_size_frames = 16384; /* 16k frames default */
        s_audio_buf.assign(frames_to_samples(s_audio_buf_size_frames), 0);
        s_audio_read_idx = s_audio_write_idx = 0;
    }

    s_initialised.store(true);
    return true;
}

/* Deinit bridge */
extern "C" void sheepbridge_deinit(void)
{
    {
        std::lock_guard<std::mutex> lk(s_audio_mutex);
        s_audio_buf.clear();
        s_audio_buf_size_frames = 0;
        s_audio_read_idx = s_audio_write_idx = 0;
    }

    {
        std::lock_guard<std::mutex> lk(s_frame_mutex);
        delete [] s_frame_buffer;
        s_frame_buffer = nullptr;
        s_frame_width = s_frame_height = 0;
        s_frame_pitch = 0;
        s_frame_available = false;
    }

    s_input_poll_cb = NULL;
    s_input_state_cb = NULL;
    s_audio_cb = NULL;
    s_audio_batch_cb = NULL;
    s_video_cb = NULL;
    s_initialised.store(false);
}

/* Minimal setters for video and input callbacks (kept for completeness) */
extern "C" void sheepbridge_set_video_cb(retro_video_refresh_t cb) { s_video_cb = cb; }