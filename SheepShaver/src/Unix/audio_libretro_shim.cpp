#include "audio_libretro_shim.h"

#ifdef LIBRETRO
#include "libretro_bridge.h" /* sheepbridge_store_audio_samples, sheepbridge_set_sample_rate */
#endif

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Convert and forward */
void send_audio_to_host(const void *buf, size_t frames, int sample_size, int channels)
{
    if (!buf || frames == 0) return;

#ifdef LIBRETRO
    /* Output must be s16 interleaved stereo */
    size_t out_samples = frames * 2; /* stereo -> 2 samples per frame */
    int16_t *out = (int16_t*)malloc(out_samples * sizeof(int16_t));
    if (!out) return;

    if (sample_size == 2 && channels == 2) {
        /* likely already s16 stereo: copy directly */
        memcpy(out, buf, out_samples * sizeof(int16_t));
    } else if (sample_size == 2 && channels == 1) {
        /* s16 mono -> duplicate to stereo */
        const int16_t *in = (const int16_t *)buf;
        for (size_t f = 0; f < frames; ++f) {
            int16_t v = in[f];
            out[2*f] = v;
            out[2*f + 1] = v;
        }
    } else if (sample_size == 4 && channels == 2) {
        /* float32 stereo -> s16 stereo */
        const float *in = (const float *)buf;
        for (size_t f = 0; f < frames; ++f) {
            float l = in[2*f];
            float r = in[2*f + 1];
            if (l > 1.0f) l = 1.0f; if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f; if (r < -1.0f) r = -1.0f;
            out[2*f] = (int16_t)(l * 32767.0f);
            out[2*f + 1] = (int16_t)(r * 32767.0f);
        }
    } else if (sample_size == 4 && channels == 1) {
        /* float32 mono -> s16 stereo */
        const float *in = (const float *)buf;
        for (size_t f = 0; f < frames; ++f) {
            float s = in[f];
            if (s > 1.0f) s = 1.0f; if (s < -1.0f) s = -1.0f;
            int16_t v = (int16_t)(s * 32767.0f);
            out[2*f] = v;
            out[2*f + 1] = v;
        }
    } else if (sample_size == 1) {
        /* 8-bit unsigned to signed 16-bit stereo */
        if (channels == 1) {
            const uint8_t *in = (const uint8_t *)buf;
            for (size_t f = 0; f < frames; ++f) {
                int16_t v = ((int)in[f] - 128) << 8;
                out[2*f] = v;
                out[2*f + 1] = v;
            }
        } else {
            const uint8_t *in = (const uint8_t *)buf;
            for (size_t f = 0; f < frames; ++f) {
                int16_t l = ((int)in[2*f] - 128) << 8;
                int16_t r = ((int)in[2*f + 1] - 128) << 8;
                out[2*f] = l;
                out[2*f + 1] = r;
            }
        }
    } else {
        /* Unknown format: produce silence to avoid undefined behavior */
        memset(out, 0, out_samples * sizeof(int16_t));
    }

    /* Forward to libretro bridge */
    sheepbridge_store_audio_samples(out, frames);
    free(out);
#else
    (void)buf; (void)frames; (void)sample_size; (void)channels;
#endif
}

void send_s16_stereo_to_host(const int16_t *samples, size_t frames)
{
    if (!samples || frames == 0) return;
#ifdef LIBRETRO
    sheepbridge_store_audio_samples(samples, frames);
#else
    (void)samples; (void)frames;
#endif
}