/*
 * libretro wrapper for SheepShaver
 *
 * This file implements the libretro API and forwards to the SheepShaver bridge.
 *
 * Minimal implementation — fills in the required libretro entry points and
 * forwards to the exposed SheepShaver bridge functions.
 *
 * See SheepShaver/src/Unix/libretro_bridge.{h,cpp} for the bridge implementation.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "libretro.h"

/* Bridge functions implemented in C++ */
#ifdef __cplusplus
extern "C" {
#endif
bool sheepbridge_init(const char *game_path, unsigned int ram_mb);
void sheepbridge_set_video_cb(retro_video_refresh_t cb);
void sheepbridge_set_audio_cb(retro_audio_sample_t cb, retro_audio_sample_batch_t cb_batch);
void sheepbridge_set_input_cb(retro_input_poll_t poll_cb, retro_input_state_t state_cb);
void sheepbridge_run_frame(void);
void sheepbridge_deinit(void);
#ifdef __cplusplus
}
#endif

/* libretro callbacks stored by the front-end */
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static struct retro_log_callback logging;
static retro_environment_t environ_cb = NULL;

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
    video_cb = cb;
    sheepbridge_set_video_cb(cb);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
    audio_cb = cb;
    sheepbridge_set_audio_cb(cb, audio_batch_cb);
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    audio_batch_cb = cb;
    sheepbridge_set_audio_cb(audio_cb, cb);
}

void retro_set_input_poll(retro_input_poll_t cb)
{
    input_poll_cb = cb;
    sheepbridge_set_input_cb(cb, input_state_cb);
}

void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
    sheepbridge_set_input_cb(input_poll_cb, cb);
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "sheepshaver_libretro";
    info->library_version = "0.1";
    info->valid_extensions = "iso|img|dsk|hfv|hdi|sheep"; /* suggestion */
    info->need_fullpath = false;
    info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    /* Set a reasonable default; the bridge can change this if needed via environ */
    struct retro_game_geometry geom = { 640, 480, 640, 480, 4.0/3.0f };
    struct retro_system_timing timing = { 60.0, 44100.0 };
    info->geometry = geom;
    info->timing = timing;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port;(void)device; }

void retro_init(void)
{
    /* No-op for now — libretro front-end will call retro_load_game */
}

void retro_deinit(void)
{
    sheepbridge_deinit();
}

bool retro_load_game(const struct retro_game_info *game)
{
    const char *path = NULL;
    if (game && game->path)
        path = game->path;

    /* Default RAM to 128MB, could be configurable by core options */
    unsigned int default_ram_mb = 128;

    if (!sheepbridge_init(path, default_ram_mb))
        return false;

    return true;
}

void retro_unload_game(void)
{
    sheepbridge_deinit();
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_reset(void)
{
    /* TODO: Add reset support in bridge */
}

size_t retro_serialize_size(void)
{
    return 0; /* TODO: support savestates if desired */
}
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }

/* The main per-frame call from libretro */
void retro_run(void)
{
    /* Poll input first */
    if (input_poll_cb)
        input_poll_cb();

    /* Run a single frame of the emulator */
    sheepbridge_run_frame();

    /* Video/Audio callbacks are invoked from SheepShaver internals via the bridge */
}