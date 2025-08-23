#include "sheepshaver_api.h"
#include "main.h" /* for InitAll/ExitAll prototypes and global variables if needed */
#include <stdbool.h>

extern bool Unix_SheepShaver_Init(const char *rom_path, const char *vmdir, unsigned int ram_mb);
extern bool Unix_SheepShaver_StartEmulationThread(void);
extern void Unix_SheepShaver_StopEmulationThread(void);
extern void Unix_SheepShaver_Deinit(void);
extern bool Unix_SheepShaver_LoadROM(const char *rom_path);

extern "C" bool SheepShaver_Init(const char *rom_path, const char *vmdir, unsigned int ram_mb)
{
    return Unix_SheepShaver_Init(rom_path, vmdir, ram_mb);
}
extern "C" bool SheepShaver_StartEmulation(void)
{
    return Unix_SheepShaver_StartEmulationThread();
}
extern "C" void SheepShaver_StopEmulation(void)
{
    Unix_SheepShaver_StopEmulationThread();
}
extern "C" void SheepShaver_Deinit(void)
{
    Unix_SheepShaver_Deinit();
}
extern "C" bool SheepShaver_LoadROM(const char *rom_path)
{
    return Unix_SheepShaver_LoadROM(rom_path);
}