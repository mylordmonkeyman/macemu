/*
 * libretro bridge (frame-buffered + audio FIFO + input forwarding)
 *
 * This implementation:
 *  - Collects libretro input callbacks (poll & state)
 *  - On each sheepbridge_run_frame() call polls input and forwards it
 *    into SheepShaver's ADB input entry points
 *  - Implements D-pad -> relative mouse movement (configurable per-poll step)
 *  - Implements GUI toggle (Nuklear) using a toggle combo (START+SELECT)
 *
 * Notes:
 *  - This file intentionally forwards input into the existing ADB* functions
 *    (ADBKeyDown/Up, ADBMouseDown/Up, ADBMouseMoved) so the emulator reuses
 *    its existing input handling pipeline.
 *  - If the repo's nukleargui is available & linked, the externs declared
 *    below will be resolved. The bridge toggles the GUI via sheepbridge_nuklear_toggle()
 *    and calls sheepbridge_nuklear_handle() each frame while visible.
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
#include <algorithm>

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

/* Input state caches (to detect edges) */
static bool s_prev_btn_start = false;
static bool s_prev_btn_select = false;
static bool s_prev_btn_a = false;
static bool s_prev_btn_b = false;
static bool s_prev_btn_x = false;
static bool s_prev_btn_y = false;
static bool s_prev_mouse_left = false;
static bool s_prev_mouse_right = false;

/* Mouse emulation state (relative movement from D-pad) */
static int s_mouse_x = 0;
static int s_mouse_y = 0;
static int s_mouse_speed = 8; /* pixels per poll step - adjust for responsiveness */

/* GUI state (Nuklear) */
static bool s_gui_visible = false;

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
            /* drop oldest sample */
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
 * we will loop until FIFO empty.
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
            delete [] s_frame_buffer;
            s_frame_width = width;
            s_frame_height = height;
            s_frame_pitch = pitch;
            s_frame_buffer = new uint8_t[width * height * 4];
        }
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

/* ADB input entry points used by the emulator. These are defined in adb.cpp.
 * We call these to inject keyboard/mouse events into SheepShaver.
 */
extern "C" {
void ADBKeyDown(int key);
void ADBKeyUp(int key);
void ADBMouseDown(int button); /* 0=left,1=right,2=middle */
void ADBMouseUp(int button);
void ADBMouseMoved(int x, int y);
}

/* Optional Nukleargui externs (present in libretro/nukleargui/app.c).
 * If the nukleargui compilation unit is linked, these will be resolved.
 */
extern "C" {
/* Global used by nukleargui to show the virtual keyboard/GUI */
extern int SHOWKEY;
/* Toggle the nukleargui "virtual keyboard" or GUI state (implementation-specific) */
void app_vkb_handle(void);
void vkbd_key(int key, int pressed);
}

/* Set libretro input callbacks */
extern "C" void sheepbridge_set_input_cb(retro_input_poll_t poll_cb, retro_input_state_t state_cb)
{
    s_input_poll_cb = poll_cb;
    s_input_state_cb = state_cb;
}

/* Toggle Nuklear GUI visibility (wrapper) */
extern "C" void sheepbridge_nuklear_toggle(void)
{
    /* If nukleargui is present, flip SHOWKEY (1=visible, 0=hidden).
     * If the symbol isn't present at link time, this will be a no-op.
     */
#ifdef __GNUC__
    /* attempt to use SHOWKEY if available */
    if (&SHOWKEY) {
        SHOWKEY = !SHOWKEY;
        s_gui_visible = (SHOWKEY != 0);
    }
#else
    /* Fallback conservative toggle */
    s_gui_visible = !s_gui_visible;
#endif
}

/* Per-frame nuklear processing wrapper (if available) */
extern "C" void sheepbridge_nuklear_handle(void)
{
    /* If the nuklear handler is linked, run it to process GUI input & render.
     * The actual rendering of nuklear into the libretro output is expected to
     * be implemented by the nukleargui code in the repo.
     */
    app_vkb_handle();
}

/* Perform mapping: poll frontend and forward mapped events into emulator.
 * D-pad is used to move mouse relatively in the emulated Mac. Buttons map to
 * mouse buttons (A -> left, B -> right) and Start+Select toggles the nuklear GUI.
 */
static void sheepbridge_process_input()
{
    if (!s_input_poll_cb || !s_input_state_cb) return;

    /* Ask frontend to poll devices (libretro convention) */
    s_input_poll_cb();

    /* Read some useful controls from player 0 joystick */
    bool dpad_up = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
    bool dpad_down = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
    bool dpad_left = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
    bool dpad_right = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);

    bool btn_a = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
    bool btn_b = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
    bool btn_x = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
    bool btn_y = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);

    bool btn_start = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
    bool btn_select = !!s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);

    /* Toggle GUI on START+SELECT pressed together (rising edge) */
    if (btn_start && btn_select && (!s_prev_btn_start || !s_prev_btn_select)) {
        /* Toggle nuklear GUI visibility */
        sheepbridge_nuklear_toggle();
    }
    s_prev_btn_start = btn_start;
    s_prev_btn_select = btn_select;

    /* If GUI visible, steer input toward the GUI handling code (nuklear).
     * That code will handle virtual keyboard & GUI navigation. We must avoid
     * forwarding mouse/key input into the emulated Mac while GUI consumes it.
     */
    if (s_gui_visible || (SHOWKEY != 0)) {
        /* Forward a subset of controls to the nuklear vkbd if implemented:
         * - Map A/B/X/Y to some VKBD inputs (implementation-specific).
         * - Nuklear/UI should read Core_Key_Sate or have its own mapping.
         *
         * We'll generate vkbd_key calls for simple presses so the virtual keyboard
         * can react; mapping here is conservative and can be extended.
         */
        if (btn_a && !s_prev_btn_a) vkbd_key(1, 1); /* example: key id 1 pressed */
        if (!btn_a && s_prev_btn_a) vkbd_key(1, 0);
        if (btn_b && !s_prev_btn_b) vkbd_key(2, 1);
        if (!btn_b && s_prev_btn_b) vkbd_key(2, 0);
        if (btn_x && !s_prev_btn_x) vkbd_key(3, 1);
        if (!btn_x && s_prev_btn_x) vkbd_key(3, 0);
        if (btn_y && !s_prev_btn_y) vkbd_key(4, 1);
        if (!btn_y && s_prev_btn_y) vkbd_key(4, 0);

        s_prev_btn_a = btn_a;
        s_prev_btn_b = btn_b;
        s_prev_btn_x = btn_x;
        s_prev_btn_y = btn_y;

        /* Let the nuklear UI run if available */
        sheepbridge_nuklear_handle();

        /* Do not forward pointer/mouse into the emulated Mac while GUI is active */
        return;
    }

    /* --- D-pad -> relative mouse movement --- */
    int dx = 0, dy = 0;
    if (dpad_left) dx -= s_mouse_speed;
    if (dpad_right) dx += s_mouse_speed;
    if (dpad_up) dy -= s_mouse_speed;
    if (dpad_down) dy += s_mouse_speed;

    /* Update internal mouse pos. If we know emulated frame size, clamp; else allow free movement. */
    s_mouse_x += dx;
    s_mouse_y += dy;
    if (s_frame_width > 0 && s_frame_height > 0) {
        s_mouse_x = std::max(0, std::min<int>(s_mouse_x, (int)s_frame_width - 1));
        s_mouse_y = std::max(0, std::min<int>(s_mouse_y, (int)s_frame_height - 1));
    }

    /* Only generate a mouse move call if there was movement */
    if (dx != 0 || dy != 0) {
        ADBMouseMoved(s_mouse_x, s_mouse_y);
    }

    /* Map gamepad buttons to mouse button clicks */
    if (btn_a && !s_prev_mouse_left) { ADBMouseDown(0); s_prev_mouse_left = true; }
    if (!btn_a && s_prev_mouse_left) { ADBMouseUp(0); s_prev_mouse_left = false; }

    if (btn_b && !s_prev_mouse_right) { ADBMouseDown(1); s_prev_mouse_right = true; }
    if (!btn_b && s_prev_mouse_right) { ADBMouseUp(1); s_prev_mouse_right = false; }

    /* Optionally map X/Y to middle/other buttons or to keyboard events as desired */
    if (btn_x && !s_prev_btn_x) { ADBMouseDown(2); s_prev_btn_x = true; }
    if (!btn_x && s_prev_btn_x) { ADBMouseUp(2); s_prev_btn_x = false; }

    /* update simple button caches for A/B/X/Y */
    s_prev_btn_a = btn_a;
    s_prev_btn_b = btn_b;
    s_prev_btn_x = btn_x;
    s_prev_btn_y = btn_y;

    /* NOTE: We deliberately do not attempt to implement full keyboard mapping
     * (RETRO_DEVICE_KEYBOARD) here; that can be added later if you want full
     * physical keyboard support from the frontend. For now, the nuclear GUI's
     * virtual keyboard can be used to input text when visible.
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

    /* initialize mouse to center on first known frame size (or 0,0 until frame arrives) */
    s_mouse_x = 0;
    s_mouse_y = 0;

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

/* Minimal setter for video callback */
extern "C" void sheepbridge_set_video_cb(retro_video_refresh_t cb) { s_video_cb = cb; }