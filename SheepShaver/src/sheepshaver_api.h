#ifndef SHEEPSHAVER_API_H
#define SHEEPSHAVER_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

/* Initialize SheepShaver backend for a headless/libretro run.
 * - rom_path: path to Mac ROM file (nullable; will use pref fallback)
 * - vmdir: path to XPRAM/vmdir if needed (nullable)
 * - ram_mb: requested RAM size in megabytes (0 to use default)
 *
 * Returns true on success (init completed and internal threads started).
 */
bool SheepShaver_Init(const char *rom_path, const char *vmdir, unsigned int ram_mb);

/* Start the emulation thread if not started by Init.
 * Usually Init will already start the emulation thread; this exists if you want
 * to decouple load/init from starting the CPU thread.
 */
bool SheepShaver_StartEmulation(void);

/* Request stop of emulator threads and subsystems.  Returns when threads are
 * requested to stop (may not have fully joined).
 */
void SheepShaver_StopEmulation(void);

/* Deinitialize and free resources created by SheepShaver_Init */
void SheepShaver_Deinit(void);

/* Convenience: load only the ROM into memory (returns true on success).
 * If you expose load_mac_rom() elsewhere, this wrapper can call it directly.
 */
bool SheepShaver_LoadROM(const char *rom_path);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SHEEPSHAVER_API_H */