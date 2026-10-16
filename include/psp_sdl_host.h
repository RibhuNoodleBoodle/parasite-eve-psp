#ifndef PSP_SDL_HOST_H
#define PSP_SDL_HOST_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PSP Host Platform Layer
 *
 * Controls Sony PlayStation Portable hardware initialization, system clocks,
 * homebrew exit callback threads, SDL 1.2 initialization (WITHOUT video),
 * and PS1 Memory Card ("bu00:") mapping to the PSP Memory Stick.
 */

#define PSP_SAVEDATA_BASE_DIR "ms0:/PSP/SAVEDATA"
#define PSP_GAME_SAVE_DIR     "ms0:/PSP/SAVEDATA/SLUS00662"
#define PSP_MEMCARD_FILE      "SLUS00662.mcd"
#define PS1_MEMCARD_SIZE      0x20000u  /* 128 KiB (131072 bytes) */
#define PS1_SECTOR_SIZE       0x2000u   /* 8 KiB (8192 bytes, 16 sectors) */
#define PS1_FRAME_SIZE        0x80u     /* 128 bytes (1024 frames) */
#define PS1_CARD_SECTORS      16u
#define PS1_CARD_FRAMES       1024u

/*
 * Lifecycle Management:
 * - Locks PSP CPU/Bus clock to 333/166 MHz via scePowerSetClockFrequency
 * - Sets up PSP exit callback thread for safe HOME button exit
 * - Initializes SDL with AUDIO | JOYSTICK | TIMER | NOPARACHUTE (NO VIDEO)
 * - Initializes PSP Input, Audio, and Save Data subsystems
 */
int  PspHost_Init(void);
void PspHost_Shutdown(void);
int  PspHost_IsRunning(void);
void psx_main(void);

/* Timing helpers */
uint32_t PspHost_GetTicks(void);
void     PspHost_Delay(uint32_t ms);

/*
 * PS1 Memory Card ("bu00:") Save System on PSP Memory Stick:
 * Maps sector/frame reads and writes to ms0:/PSP/SAVEDATA/SLUS00662/
 */
int         PspSave_Init(const char *dir_path, const char *card_file);
int         PspSave_ReadSector(uint32_t sector_idx, void *dest);
int         PspSave_WriteSector(uint32_t sector_idx, const void *src);
int         PspSave_ReadFrame(uint32_t frame_idx, void *dest);
int         PspSave_WriteFrame(uint32_t frame_idx, const void *src);
const char *PspSave_GetCardPath(void);
int         PspSave_Flush(void);
int         PspSave_FormatCard(void);

#ifdef __cplusplus
}
#endif

#endif /* PSP_SDL_HOST_H */
