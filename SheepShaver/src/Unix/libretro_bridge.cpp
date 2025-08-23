/*
 * libretro bridge (frame-buffered + audio FIFO)
 *
 * - sheepbridge_store_audio_samples() is intended to be called by audio backends
 *   from the audio/driver thread. It copies samples into an internal FIFO.
 * - sheepbridge_run_frame() is called by retro_run and will drain available audio
 *   samples and call the libretro audio_batch callback from the libretro thread.
 *
 * NOTE: This uses a mutex for simplicity and safety. It is not lock-free but is
 * fast enough for typical use since it does only memory copies and small locks.
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

static retro_video_refresh_t s_video_cb = NULL;
static retro_audio_sample_t s_audio_cb = NULL; /* single-sample callback (rare) */
static retro_audio_sample_batch_t s_audio_batch_cb = NULL; /* preferred */
static retro_input_poll_t s_input_poll_cb = NULL;
static retro_input_state_t s_input_state_cb = NULL;

/* Frame stuff (kept from previous bridge) */
static std::mutex s_frame_mutex;
static std::condition_variable s_frame_cv;
static bool s_frame_available = false;
static unsigned s_frame_width = 0;
static unsigned s_frame_height = 0;
static size_t s_frame_pitch = 0;
static uint8_t *s_frame_buffer = nullptr;

/* Audio FIFO */
static std::mutex s_audio_mutex;
/* audio FIFO is stored as int16_t interleaved stereo: [L,R,L,R,...] */
static std::vector<int16_t> s_audio_buf; /* dynamic ring buffer storage */
static size_t s_audio_buf_size_frames = 0; /* capacity in frames (stereo frames) */
static size_t s_audio_read_idx = 0;  /* index in samples (not frames) */
static size_t s_audio_write_idx = 0; /* index in samples (not frames) */
static unsigned s_sample_rate = 44100; /* default sample rate */

/* lifecycle flag */
static std::atomic<bool> s_initialised(false);

/* Helper: ensure audio buffer capacity in samples (samples = frames * 2) */
static void ensure_audio_capacity_frames(size_t frames_cap)
{
    size_t samples_cap = frames_cap * 2; /* stereo -> 2 samples/frame */
    if (s_audio_buf_size_frames >= frames_cap)
        return;
    /* allocate new buffer with some headroom */
    size_t new_frames = frames_cap;
    size_t new_samples = new_frames * 2;
    std::vector<int16_t> new_buf(new_samples);
    /* If existing data, copy existing samples into new buffer linearly */
    if (!s_audio_buf.empty()) {
        size_t samples_used;
        if (s_audio_write_idx >= s_audio_read_idx)
            samples_used = s_audio_write_idx - s_audio_read_idx;
        else
            samples_used = s_audio_buf.size() - s_audio_read_idx + s_audio_write_idx;
        /* Copy used samples into new buffer start */
        for (size_t i = 0; i < samples_used; ++i) {
            new_buf[i] = s_audio_buf[(s_audio_read_idx + i) % s_audio_buf.size()];
        }
        s_audio_read_idx = 0;
        s_audio_write_idx = samples_used;
    } else {
        s_audio_read_idx = s_audio_write_idx = 0;
    }
    s_audio_buf.swap(new_buf);
    s_audio_buf_size_frames = new_frames;
}

/* Convert frame count to sample count (samples = frames * 2 for stereo) */
static inline size_t frames_to_samples(size_t frames) { return frames * 2; }

/* Called by platform audio backend to push audio into the bridge */
extern "C" void sheepbridge_store_audio_samples(const int16_t *samples, size_t frames)
{
    if (!s_initialised.load() || samples == nullptr || frames == 0) return;

    std::lock_guard<std::mutex> lk(s_audio_mutex);

    /* Ensure buffer has capacity for at least current data + frames */
    size_t need_frames = ( (s_audio_write_idx >= s_audio_read_idx)
                            ? ( (s_audio_write_idx - s_audio_read_idx) / 2 )
                            : ( (s_audio_buf.size() - s_audio_read_idx + s_audio_write_idx) / 2 ) )
                         + frames;
    if (need_frames > s_audio_buf_size_frames) {
        /* grow to nearest power-of-two-ish or add headroom: choose need_frames * 2 */
        size_t new_cap = need_frames * 2 + 1024;
        ensure_audio_capacity_frames(new_cap);
    }

    size_t samples_to_write = frames_to_samples(frames);
    /* Write samples into ring buffer */
    for (size_t i = 0; i < samples_to_write; ++i) {
        s_audio_buf[s_audio_write_idx] = samples[i];
        s_audio_write_idx = (s_audio_write_idx + 1) % s_audio_buf.size();
        /* If buffer full (write catches up to read), advance read to avoid overwrite */
        if (s_audio_write_idx == s_audio_read_idx) {
            /* drop oldest frame (advance read by 2 samples) */
            s_audio_read_idx = (s_audio_read_idx + 2) % s_audio_buf.size();
        }
    }
}

/* sets sample rate (optional) */
extern "C" void sheepbridge_set_sample_rate(unsigned rate)
{
    s_sample_rate = rate;
}

/* Set audio callbacks */
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

    /* compute available frames */
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
    /* maximum to send at once (frames) */
    const size_t MAX_SEND_FRAMES = 2048;
    /* allocate temporary buffer on stack if small, otherwise heap (avoid repeated allocs) */
    int16_t tmpbuf[MAX_SEND_FRAMES * 2]; /* stereo -> *2 samples */

    /* Loop draining batches */
    while (true) {
        size_t drained = drain_audio_frames(tmpbuf, MAX_SEND_FRAMES);
        if (drained == 0) break;

        if (s_audio_batch_cb) {
            /* audio_batch expects pointer to int16_t and number of frames (not samples) */
            s_audio_batch_cb(tmpbuf, (size_t)drained);
        } else if (s_audio_cb) {
            /* fallback: single-sample calls (inefficient) */
            for (size_t f = 0; f < drained; ++f) {
                /* each frame = two samples */
                s_audio_cb(tmpbuf[2 * f], tmpbuf[2 * f + 1]); /* retro_audio_sample_t expects two samples? Actually retro_audio_sample_t takes two ints: left and right */
            }
        }
    }
}

/* For completeness keep frame functions (video) from previous implementation */
extern "C" void sheepbridge_store_frame(const void *src, unsigned width, unsigned height, size_t src_pitch, unsigned src_pixel_size);
extern "C" void sheepbridge_signal_frame(void)
{
    {
        std::lock_guard<std::mutex> lk(s_frame_mutex);
        s_frame_available = true;
    }
    s_frame_cv.notify_one();
}

/* sheepbridge_run_frame now also drains audio. */
extern "C" void sheepbridge_run_frame(void)
{
    if (!s_initialised.load()) return;

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

/* Keep/implement other lifecycle functions (init/deinit) from your bridge file */
extern "C" bool sheepbridge_init(const char *game_path, unsigned int ram_mb)
{
    (void)game_path; (void)ram_mb;
    if (s_initialised.load()) return true;

    /* allocate initial audio buffer capacity (frames) */
    {
        std::lock_guard<std::mutex> lk(s_audio_mutex);
        s_audio_buf_size_frames = 16384; /* 16k frames ~ 16k @ 44100 ~ 0.36s buffer */
        s_audio_buf.assign(frames_to_samples(s_audio_buf_size_frames), 0);
        s_audio_read_idx = s_audio_write_idx = 0;
    }

    s_initialised.store(true);
    return true;
}

extern "C" void sheepbridge_deinit(void)
{
    if (!s_initialised.load()) return;
    s_initialised.store(false);

    /* free audio buffer */
    {
        std::lock_guard<std::mutex> lk(s_audio_mutex);
        s_audio_buf.clear();
        s_audio_buf_size_frames = 0;
        s_audio_read_idx = s_audio_write_idx = 0;
    }

    /* free frame buffer */
    if (s_frame_buffer) {
        free(s_frame_buffer);
        s_frame_buffer = nullptr;
    }
    s_frame_width = s_frame_height = 0;
    s_frame_pitch = 0;

    /* Wake frame waiter so it can exit */
    {
        std::lock_guard<std::mutex> lk(s_frame_mutex);
        s_frame_available = true;
    }
    s_frame_cv.notify_all();
}