/*
 * psp_cdrom_iso.c - Sony PlayStation Portable CD-ROM & ISO Stream Subsystem
 *
 * Implements:
 * 1. Asynchronous raw BIN/CUE / ISO sector streaming via a 256 KB ring buffer
 *    (110 raw 2352-byte sectors) and background reader thread.
 * 2. Hot-swapping between Disc 1 and Disc 2 via API and L+R+Select controller chord.
 * 3. PS1 Psy-Q Cdl API compatibility (CdControl, CdRead, CdSeekL, CdReady).
 * 4. Lightweight in-place PS1 MDEC macroblock IDCT decoder and 15 fps STR movie
 *    streamer writing directly to sceGu texture buffers without heap exhaustion.
 */

#include "psp_cdrom_iso.h"
#include "psp_sdl_input.h"
#include "psp_sdl_audio.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>
#include <SDL/SDL_mutex.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── CD-ROM Subsystem State ─────────────────────────────────────────────────── */

typedef struct PspCdromState {
    int          initialized;
    int          active_disc;          /* 1 or 2 */
    char         disc1_path[256];
    char         disc2_path[256];
    SceUID       fd;                   /* Native PSP kernel file descriptor */
    uint32_t     total_sectors;
    uint32_t     current_lba;
    uint32_t     target_lba;
    uint8_t      status;               /* Status flags (PSP_CD_STAT_*) */

    /* 256 KB Asynchronous Sector Ring Buffer */
    uint8_t      ring_buffer[PSP_CDROM_RING_BYTES];
    uint32_t     ring_lbas[PSP_CDROM_RING_SECTORS];
    uint32_t     ring_read_idx;
    uint32_t     ring_write_idx;
    uint32_t     ring_count;

    /* Background thread synchronization */
    SDL_Thread  *reader_thread;
    SDL_mutex   *lock;
    SDL_cond    *cond_data_avail;
    SDL_cond    *cond_space_avail;
    volatile int worker_running;
    volatile int seeking;

    /* Disc hot-swap debounce state */
    uint16_t     prev_chord_buttons;
} PspCdromState;

static PspCdromState s_cd;

/* ── MDEC Decoder State ─────────────────────────────────────────────────────── */

static PspMdecState s_mdec;

/* Standard JPEG/PS1 default luminance and chrominance quantization tables */
static const uint8_t s_default_luma_quant[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99
};

static const uint8_t s_default_chroma_quant[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

/* Zigzag order mapping for 8x8 blocks */
static const uint8_t s_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ── Asynchronous Background CD-ROM Reader Thread ────────────────────────────── */

static int cdrom_worker_thread(void *arg)
{
    (void)arg;

    while (s_cd.worker_running) {
        uint32_t next_lba;
        uint32_t write_slot;
        uint8_t temp_sector[PSP_CDROM_SECTOR_RAW];
        size_t read_bytes;

        SDL_LockMutex(s_cd.lock);

        /* Wait while ring buffer is completely full or paused */
        while (s_cd.worker_running && !s_cd.seeking && s_cd.ring_count >= PSP_CDROM_RING_SECTORS) {
            SDL_CondWait(s_cd.cond_space_avail, s_cd.lock);
        }

        if (!s_cd.worker_running) {
            SDL_UnlockMutex(s_cd.lock);
            break;
        }

        /* Check for seek requests */
        if (s_cd.seeking) {
            s_cd.current_lba = s_cd.target_lba;
            s_cd.ring_read_idx = 0;
            s_cd.ring_write_idx = 0;
            s_cd.ring_count = 0;
            s_cd.seeking = 0;
            if (s_cd.fd >= 0) {
                sceIoLseek(s_cd.fd, (SceOff)s_cd.current_lba * PSP_CDROM_SECTOR_RAW, PSP_SEEK_SET);
            }
        }

        next_lba = s_cd.current_lba;
        if (s_cd.fd < 0 || next_lba >= s_cd.total_sectors) {
            s_cd.status &= (uint8_t)~PSP_CD_STAT_READING;
            s_cd.status |= PSP_CD_STAT_IDLE;
            SDL_CondWait(s_cd.cond_space_avail, s_cd.lock);
            SDL_UnlockMutex(s_cd.lock);
            continue;
        }

        write_slot = s_cd.ring_write_idx;
        SDL_UnlockMutex(s_cd.lock);

        /* Perform physical file read outside of lock to prevent stalling main thread */
        read_bytes = (size_t)sceIoRead(s_cd.fd, temp_sector, PSP_CDROM_SECTOR_RAW);

        SDL_LockMutex(s_cd.lock);
        if (read_bytes == PSP_CDROM_SECTOR_RAW && !s_cd.seeking) {
            memcpy(s_cd.ring_buffer + (write_slot * PSP_CDROM_SECTOR_RAW), temp_sector, PSP_CDROM_SECTOR_RAW);
            s_cd.ring_lbas[write_slot] = next_lba;
            s_cd.ring_write_idx = (s_cd.ring_write_idx + 1u) % PSP_CDROM_RING_SECTORS;
            s_cd.ring_count++;
            s_cd.current_lba++;
            s_cd.status |= PSP_CD_STAT_READING;
            SDL_CondSignal(s_cd.cond_data_avail);
        }
        SDL_UnlockMutex(s_cd.lock);
    }

    return 0;
}

/* ── CD-ROM Subsystem Lifecycle ─────────────────────────────────────────────── */

/* Search candidate paths for Disc 1 and Disc 2 (hardcoded filenames and exact fallback array) */
static const char * const s_disc1_search_paths[] = {
    "disc/PE_DISC1.bin",
    "ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC1.bin",
    "./PE_DISC1.bin",
    "./disc/PE_DISC1.bin",
    "host0:/dist/PSP/GAME/ParasiteEve/disc/PE_DISC1.bin",
    NULL
};

static const char * const s_disc2_search_paths[] = {
    "disc/PE_DISC2.bin",
    "ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC2.bin",
    "./PE_DISC2.bin",
    "./disc/PE_DISC2.bin",
    "host0:/dist/PSP/GAME/ParasiteEve/disc/PE_DISC2.bin",
    NULL
};

static void fatal_disc_not_found(int disc_num, const char * const candidates[], int last_error)
{
    const char *disc_name = (disc_num == 2) ? "PE_DISC2.bin" : "PE_DISC1.bin";
    char log_buf[512];
    int len;

    /* a) Write [FATAL ERROR] FILE NOT FOUND to stderr via sceIoWrite(2, ...) */
    len = snprintf(log_buf, sizeof(log_buf),
                   "\n[FATAL ERROR] FILE NOT FOUND: %s (last sceIoOpen error: 0x%08X)\nAttempted paths:\n",
                   disc_name, (unsigned int)last_error);
    sceIoWrite(2, log_buf, len);

    for (int i = 0; candidates[i] != NULL; i++) {
        len = snprintf(log_buf, sizeof(log_buf), "  [%d] %s\n", i + 1, candidates[i]);
        sceIoWrite(2, log_buf, len);
    }

    /* b) Initialize PSP hardware debug screen with red background */
    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0xFF000080); /* Dark red background */
    pspDebugScreenSetTextColor(0xFF0000FF); /* Bright red text (ABGR) */
    pspDebugScreenClear();

    pspDebugScreenPrintf("\n");
    pspDebugScreenPrintf("  ==================================================\n");
    pspDebugScreenPrintf("         PARASITE EVE PSP PORT - FATAL ERROR       \n");
    pspDebugScreenPrintf("  ==================================================\n\n");
    pspDebugScreenPrintf("  ERROR: File not found: %s\n\n", disc_name);
    pspDebugScreenPrintf("  Place PE_DISC1.bin and PE_DISC2.bin in:\n");
    pspDebugScreenPrintf("    ms0:/PSP/GAME/ParasiteEve/disc/\n\n");
    pspDebugScreenPrintf("  Attempted paths (last sceIoOpen err: 0x%08X):\n", (unsigned int)last_error);
    for (int i = 0; candidates[i] != NULL; i++) {
        pspDebugScreenPrintf("   - %s\n", candidates[i]);
    }
    pspDebugScreenPrintf("\n  Press START to exit to PSP Home Menu.\n");

    /* c) Loop cleanly with sceDisplayWaitVblankStart() and allow exit via START or HOME */
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    while (1) {
        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & (PSP_CTRL_START | PSP_CTRL_HOME)) {
            break;
        }
        sceDisplayWaitVblankStart();
    }

    sceKernelExitGame();
}

int psp_verify_disc_files(void)
{
    int disc1_found = 0;
    int disc2_found = 0;
    int last_err1 = -1;
    int last_err2 = -1;

    for (int i = 0; s_disc1_search_paths[i] != NULL; i++) {
        SceUID fd = sceIoOpen(s_disc1_search_paths[i], PSP_O_RDONLY, 0);
        if (fd >= 0) {
            sceIoClose(fd);
            disc1_found = 1;
            break;
        } else {
            last_err1 = (int)fd;
        }
    }

    for (int i = 0; s_disc2_search_paths[i] != NULL; i++) {
        SceUID fd = sceIoOpen(s_disc2_search_paths[i], PSP_O_RDONLY, 0);
        if (fd >= 0) {
            sceIoClose(fd);
            disc2_found = 1;
            break;
        } else {
            last_err2 = (int)fd;
        }
    }

    if (!disc1_found || !disc2_found) {
        char err_msg[512];
        int len;

        /* Write to stderr via sceIoWrite(2, ...) */
        len = snprintf(err_msg, sizeof(err_msg),
            "\n[FATAL ERROR] File not found -> PE_DISC1.bin or PE_DISC2.bin\n"
            "Disc 1 found: %s (last err: 0x%08X)\n"
            "Disc 2 found: %s (last err: 0x%08X)\n"
            "Attempted search locations:\n"
            "  1. disc/PE_DISC1.bin | disc/PE_DISC2.bin\n"
            "  2. ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC1.bin | ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC2.bin\n"
            "  3. ./PE_DISC1.bin | ./PE_DISC2.bin\n",
            disc1_found ? "YES" : "NO", (unsigned int)last_err1,
            disc2_found ? "YES" : "NO", (unsigned int)last_err2);
        sceIoWrite(2, err_msg, len);

        /* Initialize PSP debug screen with bright red text */
        pspDebugScreenInit();
        pspDebugScreenSetBackColor(0xFF000080);
        pspDebugScreenSetTextColor(0xFF0000FF);
        pspDebugScreenClear();

        pspDebugScreenPrintf("\n");
        pspDebugScreenPrintf("  ======================================================\n");
        pspDebugScreenPrintf("          PARASITE EVE PSP PORT - FATAL ERROR           \n");
        pspDebugScreenPrintf("  ======================================================\n\n");
        pspDebugScreenPrintf("  FATAL ERROR: File not found -> PE_DISC1.bin or PE_DISC2.bin\n\n");
        pspDebugScreenPrintf("  Status:\n");
        pspDebugScreenPrintf("    - PE_DISC1.bin: %s\n", disc1_found ? "FOUND" : "MISSING");
        pspDebugScreenPrintf("    - PE_DISC2.bin: %s\n\n", disc2_found ? "FOUND" : "MISSING");
        pspDebugScreenPrintf("  Please place PE_DISC1.bin and PE_DISC2.bin into:\n");
        pspDebugScreenPrintf("    ms0:/PSP/GAME/ParasiteEve/disc/\n\n");
        pspDebugScreenPrintf("  Attempted search paths:\n");
        for (int i = 0; s_disc1_search_paths[i] != NULL; i++) {
            pspDebugScreenPrintf("    - %s\n", s_disc1_search_paths[i]);
        }
        pspDebugScreenPrintf("\n  Press START to exit to PSP Home Menu.\n");

        sceCtrlSetSamplingCycle(0);
        sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

        while (1) {
            SceCtrlData pad;
            sceCtrlReadBufferPositive(&pad, 1);
            if (pad.Buttons & (PSP_CTRL_START | PSP_CTRL_HOME)) {
                break;
            }
            sceDisplayWaitVblankStart();
        }

        sceKernelExitGame();
        return 0;
    }

    return 1;
}

int PspCdrom_CheckDiscExists(int disc_num)
{
    const char * const *candidates = (disc_num == 2) ? s_disc2_search_paths : s_disc1_search_paths;
    for (int i = 0; candidates[i] != NULL; i++) {
        SceUID test_fd = sceIoOpen(candidates[i], PSP_O_RDONLY, 0777);
        if (test_fd >= 0) {
            sceIoClose(test_fd);
            return 1;
        }
    }
    return 0;
}

static int open_disc_file(int disc_num, const char *preferred_path)
{
    const char * const *candidates = (disc_num == 2) ? s_disc2_search_paths : s_disc1_search_paths;
    const char *disc_name = (disc_num == 2) ? "PE_DISC2.bin" : "PE_DISC1.bin";
    int last_error = -1;

    if (s_cd.fd >= 0) {
        sceIoClose(s_cd.fd);
        s_cd.fd = -1;
    }

    /* 1. Try preferred path */
    if (preferred_path && preferred_path[0]) {
        s_cd.fd = sceIoOpen(preferred_path, PSP_O_RDONLY, 0777);
        if (s_cd.fd >= 0) {
            snprintf(disc_num == 2 ? s_cd.disc2_path : s_cd.disc1_path, 256, "%s", preferred_path);
            goto found;
        } else {
            last_error = s_cd.fd;
        }
    }

    /* 2. Try candidate fallback search array */
    for (int i = 0; candidates[i] != NULL; i++) {
        s_cd.fd = sceIoOpen(candidates[i], PSP_O_RDONLY, 0777);
        if (s_cd.fd >= 0) {
            snprintf(disc_num == 2 ? s_cd.disc2_path : s_cd.disc1_path, 256, "%s", candidates[i]);
            goto found;
        } else {
            last_error = s_cd.fd;
        }
    }

    /* 3. None opened: trigger explicit visible error screen & terminal log */
    fatal_disc_not_found(disc_num, candidates, last_error);
    return 0;

found:
    {
        SceOff size = sceIoLseek(s_cd.fd, 0, PSP_SEEK_END);
        sceIoLseek(s_cd.fd, 0, PSP_SEEK_SET);

        s_cd.total_sectors = (uint32_t)(size / PSP_CDROM_SECTOR_RAW);
        s_cd.current_lba = 0;
        s_cd.target_lba = 0;
        s_cd.seeking = 1;
        s_cd.active_disc = disc_num;
        s_cd.status = 0x02; /* CdlStatStandby */

        char msg[256];
        int len = snprintf(msg, sizeof(msg),
                 "[PSP_CDROM] Opened %s from %s (%u sectors, %lu MB, fd=%d)\n",
                 disc_name,
                 (disc_num == 2 ? s_cd.disc2_path : s_cd.disc1_path),
                 (unsigned)s_cd.total_sectors,
                 (unsigned long)(size / (1024 * 1024)),
                 (int)s_cd.fd);
        sceIoWrite(1, msg, len);
    }
    return 1;
}

int PspCdrom_Init(const char *disc1_path, const char *disc2_path)
{
    if (s_cd.initialized) {
        return 1;
    }

    memset(&s_cd, 0, sizeof(s_cd));

    /* Configure Disc Paths with default fallbacks */
    snprintf(s_cd.disc1_path, sizeof(s_cd.disc1_path), "%s",
             (disc1_path && disc1_path[0]) ? disc1_path : PSP_DISC1_DEFAULT_PATH);
    snprintf(s_cd.disc2_path, sizeof(s_cd.disc2_path), "%s",
             (disc2_path && disc2_path[0]) ? disc2_path : PSP_DISC2_DEFAULT_PATH);

    s_cd.lock = SDL_CreateMutex();
    s_cd.cond_data_avail = SDL_CreateCond();
    s_cd.cond_space_avail = SDL_CreateCond();

    /* Attempt to open Disc 1 first using candidate search list */
    s_cd.active_disc = 1;
    open_disc_file(1, s_cd.disc1_path);

    s_cd.status = PSP_CD_STAT_IDLE;
    s_cd.worker_running = 1;
    s_cd.reader_thread = SDL_CreateThread(cdrom_worker_thread, NULL);

    /* Initialize MDEC Engine */
    PspMdec_Init();

    s_cd.initialized = 1;
    fprintf(stderr, "[PSP_CDROM] Initialized (Sectors: %u, Async Ring: 256 KB)\n", (unsigned)s_cd.total_sectors);
    return 1;
}

void PspCdrom_Shutdown(void)
{
    if (!s_cd.initialized) return;

    if (s_cd.worker_running) {
        s_cd.worker_running = 0;
        if (s_cd.cond_space_avail) SDL_CondSignal(s_cd.cond_space_avail);
        if (s_cd.cond_data_avail) SDL_CondSignal(s_cd.cond_data_avail);
        if (s_cd.reader_thread) {
            SDL_WaitThread(s_cd.reader_thread, NULL);
            s_cd.reader_thread = NULL;
        }
    }

    if (s_cd.lock) SDL_LockMutex(s_cd.lock);
    if (s_cd.fd >= 0) {
        sceIoClose(s_cd.fd);
        s_cd.fd = -1;
    }
    if (s_cd.lock) SDL_UnlockMutex(s_cd.lock);

    if (s_cd.cond_data_avail) SDL_DestroyCond(s_cd.cond_data_avail);
    if (s_cd.cond_space_avail) SDL_DestroyCond(s_cd.cond_space_avail);
    if (s_cd.lock) SDL_DestroyMutex(s_cd.lock);

    PspMdec_Shutdown();
    s_cd.initialized = 0;
    fprintf(stderr, "[PSP_CDROM] Subsystem shut down.\n");
}

int PspCdrom_IsInitialized(void)
{
    return s_cd.initialized;
}

/* ── Disc Hot-Swapping ──────────────────────────────────────────────────────── */

int PspCdrom_SwapDisc(int disc_number)
{
    const char *target_path;

    if (disc_number != 1 && disc_number != 2) return 0;
    if (disc_number == s_cd.active_disc && s_cd.fd >= 0) return 1;

    target_path = (disc_number == 1) ? s_cd.disc1_path : s_cd.disc2_path;

    SDL_LockMutex(s_cd.lock);

    /* Set shell open flag during swap transition */
    s_cd.status |= PSP_CD_STAT_SHELLOPEN;
    s_cd.status &= (uint8_t)~PSP_CD_STAT_READING;

    /* Flush ring buffer */
    s_cd.ring_read_idx = 0;
    s_cd.ring_write_idx = 0;
    s_cd.ring_count = 0;

    if (!open_disc_file(disc_number, target_path)) {
        fprintf(stderr, "[PSP_CDROM] Error: Disc swap failed to open disc %d: %s\n",
                disc_number, target_path);
        SDL_UnlockMutex(s_cd.lock);
        return 0;
    }

    s_cd.active_disc = disc_number;

    /* Shell closed */
    s_cd.status &= (uint8_t)~PSP_CD_STAT_SHELLOPEN;
    s_cd.status |= PSP_CD_STAT_IDLE;
    SDL_CondSignal(s_cd.cond_space_avail);

    SDL_UnlockMutex(s_cd.lock);
    fprintf(stderr, "[PSP_CDROM] Successfully swapped to Disc %d (Sectors: %u)\n",
            disc_number, (unsigned)s_cd.total_sectors);
    return 1;
}

int PspCdrom_GetActiveDisc(void)
{
    return s_cd.active_disc;
}

void PspCdrom_CheckHotSwapChord(uint16_t buttons)
{
    /* Hotkey Chord: Select + L + R held simultaneously */
    uint16_t chord_mask = HOST_PAD_SELECT | HOST_PAD_L1 | HOST_PAD_R1;
    uint16_t alt_chord  = HOST_PAD_SELECT | HOST_PAD_L2 | HOST_PAD_R2;

    int chord_pressed = ((buttons & chord_mask) == chord_mask) ||
                        ((buttons & alt_chord) == alt_chord);

    if (chord_pressed && !(s_cd.prev_chord_buttons & (HOST_PAD_L1 | HOST_PAD_R1 | HOST_PAD_L2 | HOST_PAD_R2))) {
        int next_disc = (s_cd.active_disc == 1) ? 2 : 1;
        fprintf(stderr, "[PSP_CDROM] In-Game Chord Triggered: Hot-swapping to Disc %d\n", next_disc);
        PspCdrom_SwapDisc(next_disc);
    }

    s_cd.prev_chord_buttons = buttons;
}

/* ── Sector Navigation & Reads ──────────────────────────────────────────────── */

int PspCdrom_SeekLba(uint32_t lba)
{
    if (lba >= s_cd.total_sectors) return 0;

    SDL_LockMutex(s_cd.lock);
    s_cd.target_lba = lba;
    s_cd.seeking = 1;
    s_cd.status |= PSP_CD_STAT_SEEKING;
    SDL_CondSignal(s_cd.cond_space_avail);
    SDL_UnlockMutex(s_cd.lock);

    return 1;
}

int PspCdrom_ReadRawSector(uint32_t lba, uint8_t *out_raw)
{
    if (!out_raw || lba >= s_cd.total_sectors) return 0;

    SDL_LockMutex(s_cd.lock);

    /* 1. Fast path: check if sector is already present in 256 KB ring buffer */
    for (uint32_t i = 0; i < s_cd.ring_count; i++) {
        uint32_t slot = (s_cd.ring_read_idx + i) % PSP_CDROM_RING_SECTORS;
        if (s_cd.ring_lbas[slot] == lba) {
            memcpy(out_raw, s_cd.ring_buffer + (slot * PSP_CDROM_SECTOR_RAW), PSP_CDROM_SECTOR_RAW);
            /* Advance read pointer if reading head */
            if (i == 0) {
                s_cd.ring_read_idx = (s_cd.ring_read_idx + 1u) % PSP_CDROM_RING_SECTORS;
                s_cd.ring_count--;
                SDL_CondSignal(s_cd.cond_space_avail);
            }
            SDL_UnlockMutex(s_cd.lock);
            return 1;
        }
    }

    /* 2. Slow path: sector missed in prefetch ring, seek and synchronous read */
    s_cd.target_lba = lba + 1u;
    s_cd.seeking = 1;
    sceIoLseek(s_cd.fd, (SceOff)lba * PSP_CDROM_SECTOR_RAW, PSP_SEEK_SET);
    int n = sceIoRead(s_cd.fd, out_raw, PSP_CDROM_SECTOR_RAW);
    SDL_CondSignal(s_cd.cond_space_avail);
    SDL_UnlockMutex(s_cd.lock);

    return n == PSP_CDROM_SECTOR_RAW;
}

int PspCdrom_ReadUserSector(uint32_t lba, uint8_t *out_user)
{
    uint8_t raw[PSP_CDROM_SECTOR_RAW];
    if (!PspCdrom_ReadRawSector(lba, raw)) return 0;
    memcpy(out_user, raw + PSP_CDROM_USER_OFFSET, PSP_CDROM_SECTOR_USER);
    return 1;
}

int PspCdrom_ReadUserData(uint32_t lba, uint32_t offset, void *out, uint32_t len)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t remaining = len;

    while (remaining > 0) {
        uint8_t user[PSP_CDROM_SECTOR_USER];
        if (!PspCdrom_ReadUserSector(lba, user)) return 0;
        uint32_t chunk = PSP_CDROM_SECTOR_USER - offset;
        if (chunk > remaining) chunk = remaining;
        memcpy(dst, user + offset, chunk);
        dst += chunk;
        remaining -= chunk;
        offset = 0;
        lba++;
    }

    return 1;
}

uint32_t PspCdrom_GetSectorCount(void)
{
    return s_cd.total_sectors;
}

/* ── Psy-Q Cdl Compatibility Functions ──────────────────────────────────────── */

int CdPosToInt(const unsigned char *msf)
{
    int m = (msf[0] >> 4) * 10 + (msf[0] & 0x0F);
    int s = (msf[1] >> 4) * 10 + (msf[1] & 0x0F);
    int f = (msf[2] >> 4) * 10 + (msf[2] & 0x0F);
    return (m * 60 + s) * 75 + f - 150;
}

void CdIntToPos(int lba, unsigned char *msf)
{
    int total = lba + 150;
    int f = total % 75;
    total /= 75;
    int s = total % 60;
    int m = total / 60;
    msf[0] = (unsigned char)(((m / 10) << 4) | (m % 10));
    msf[1] = (unsigned char)(((s / 10) << 4) | (s % 10));
    msf[2] = (unsigned char)(((f / 10) << 4) | (f % 10));
}

int CdControl(unsigned char com, const unsigned char *param, unsigned char *result)
{
    if (result) *result = (s_cd.status != 0) ? s_cd.status : 0x02; /* 0x02 = CdlStatStandby */

    switch (com) {
    case CdlNop:
        return 1;
    case CdlSetloc:
        if (param) {
            uint32_t lba = (uint32_t)CdPosToInt(param);
            s_cd.current_lba = lba;
            PspCdrom_SeekLba(lba);
        }
        return 1;
    case CdlSeekL:
    case CdlSeekP:
        return 1;
    case CdlStandby:
    case CdlPause:
    case CdlStop:
        s_cd.status &= (uint8_t)~PSP_CD_STAT_READING;
        s_cd.status |= PSP_CD_STAT_IDLE;
        return 1;
    case CdlSetmode:
        return 1;
    default:
        return 1;
    }
}

int CdControlF(unsigned char com, const unsigned char *param)
{
    return CdControl(com, param, NULL);
}

int CdControlB(unsigned char com, const unsigned char *param, unsigned char *result)
{
    return CdControl(com, param, result);
}

typedef void (*CdlCB)(unsigned char, unsigned char *);
static CdlCB s_cd_sync_cb = NULL;
static CdlCB s_cd_ready_cb = NULL;
static CdlCB s_cd_read_cb = NULL;

extern CdlCB g_CdSyncCallback __attribute__((weak));
extern CdlCB g_CdReadyCallback __attribute__((weak));
extern CdlCB g_CdReadCallback __attribute__((weak));

CdlCB CdSyncCallback(CdlCB func)
{
    CdlCB prev = s_cd_sync_cb;
    s_cd_sync_cb = func;
    return prev;
}

CdlCB CdReadyCallback(CdlCB func)
{
    CdlCB prev = s_cd_ready_cb;
    s_cd_ready_cb = func;
    return prev;
}

CdlCB CdReadCallback(CdlCB func)
{
    CdlCB prev = s_cd_read_cb;
    s_cd_read_cb = func;
    return prev;
}

static void fire_cd_sync_callback(void)
{
    unsigned char res[8];
    memset(res, 0, sizeof(res));
    res[0] = s_cd.status; /* CdlStatStandby (0x02) */
    if (s_cd_sync_cb) {
        s_cd_sync_cb(0x02 /* CdlComplete */, res);
    }
    if (g_CdSyncCallback) {
        g_CdSyncCallback(0x02 /* CdlComplete */, res);
    }
}

static void fire_cd_ready_callback(void)
{
    unsigned char res[8];
    memset(res, 0, sizeof(res));
    res[0] = s_cd.status; /* CdlStatStandby (0x02) */
    if (s_cd_ready_cb) {
        s_cd_ready_cb(0x01 /* CdlDataReady */, res);
    }
    if (g_CdReadyCallback) {
        g_CdReadyCallback(0x01 /* CdlDataReady */, res);
    }
}

static void fire_cd_read_callback(void)
{
    unsigned char res[8];
    memset(res, 0, sizeof(res));
    res[0] = s_cd.status; /* CdlStatStandby (0x02) */
    if (s_cd_read_cb) {
        s_cd_read_cb(0x02 /* CdlComplete */, res);
    }
    if (g_CdReadCallback) {
        g_CdReadCallback(0x02 /* CdlComplete */, res);
    }
}

int CdRead(int sectors, unsigned long *buf, int mode)
{
    (void)mode;
    uint8_t *dst = (uint8_t *)buf;
    for (int i = 0; i < sectors; i++) {
        if (!PspCdrom_ReadUserSector(s_cd.current_lba, dst)) {
            memset(dst, 0, PSP_CDROM_SECTOR_USER);
        }
        dst += PSP_CDROM_SECTOR_USER;
        s_cd.current_lba++;
    }
    s_cd.status = 0x02; /* CdlStatStandby */
    fire_cd_sync_callback();
    fire_cd_read_callback();
    return 1;
}

int CdSync(int mode, unsigned char *result)
{
    (void)mode;
    s_cd.status = 0x02; /* CdlStatStandby */
    if (result) {
        result[0] = s_cd.status;
    }
    fire_cd_sync_callback();
    return 0; /* CdlComplete (0) */
}

int CdReadSync(int mode, unsigned char *result)
{
    (void)mode;
    s_cd.status = 0x02; /* CdlStatStandby */
    if (result) {
        result[0] = s_cd.status;
    }
    return 0; /* 0 sectors remaining / completed */
}

int CdReady(int mode, unsigned char *result)
{
    (void)mode;
    s_cd.status = 0x02; /* CdlStatStandby */
    if (result) {
        result[0] = s_cd.status;
    }
    fire_cd_ready_callback();
    return 1; /* Data ready in ring buffer */
}

int CdSeekL(const unsigned char *msf)
{
    if (!msf) return 0;
    uint32_t lba = (uint32_t)CdPosToInt(msf);
    s_cd.current_lba = lba;
    return PspCdrom_SeekLba(lba);
}

int CdGetSector(void *madr, int size)
{
    int ret = PspCdrom_ReadRawSector(s_cd.current_lba, (uint8_t *)madr) ? size : 0;
    s_cd.current_lba++;
    return ret;
}

/* ── PS1 MDEC & STR Movie Streaming (In-Place IDCT) ─────────────────────────── */

static inline int32_t clamp_val(int32_t val, int32_t low, int32_t high)
{
    return (val < low) ? low : (val > high) ? high : val;
}

static inline int32_t signed_10bit(uint16_t val)
{
    val &= 0x03FFu;
    return (val & 0x0200u) ? (int32_t)val - 1024 : (int32_t)val;
}

static void init_idct_table(void)
{
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            double c = (y == 0) ? 0.7071067811865475 : 1.0;
            double val = c * cos((2.0 * x + 1.0) * y * 3.141592653589793 / 16.0);
            s_mdec.idct_scale[x + y * 8] = (int16_t)(val * 4096.0 + 0.5);
        }
    }
}

int PspMdec_Init(void)
{
    memset(&s_mdec, 0, sizeof(s_mdec));
    memcpy(s_mdec.quant_luma, s_default_luma_quant, 64);
    memcpy(s_mdec.quant_chroma, s_default_chroma_quant, 64);
    init_idct_table();
    s_mdec.initialized = 1;
    return 1;
}

void PspMdec_Shutdown(void)
{
    s_mdec.initialized = 0;
}

void PspMdec_Reset(void)
{
    s_mdec.decoded_frames = 0;
}

void PspMdec_SetQuantTables(const uint8_t *luma_quant, const uint8_t *chroma_quant)
{
    if (luma_quant) memcpy(s_mdec.quant_luma, luma_quant, 64);
    if (chroma_quant) memcpy(s_mdec.quant_chroma, chroma_quant, 64);
}

/*
 * 8x8 2D IDCT using fixed-point row/column separable passes
 */
static void mdec_idct_8x8(int32_t block[64])
{
    int32_t temp[64];

    /* Pass 1: Columns */
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) {
                sum += (int64_t)block[x + z * 8] * (s_mdec.idct_scale[y + z * 8] >> 3);
            }
            temp[x + y * 8] = (int32_t)((sum + 4095) >> 13);
        }
    }

    /* Pass 2: Rows */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) {
                sum += (int64_t)temp[z + y * 8] * (s_mdec.idct_scale[x + z * 8] >> 3);
            }
            int32_t val = (int32_t)((sum + 4095) >> 13);
            /* Clamp to 9-bit signed */
            block[x + y * 8] = clamp_val(val, -256, 255);
        }
    }
}

/*
 * Decode a single 8x8 DCT block from the 16-bit word bitstream
 */
static int decode_block(const uint16_t *input, size_t input_words, size_t *pos,
                        int32_t block[64], const uint8_t quant[64])
{
    memset(block, 0, 64 * sizeof(int32_t));
    if (*pos >= input_words) return 0;

    /* Skip leading EOB or padding */
    while (*pos < input_words && input[*pos] == 0xFE00u) (*pos)++;
    if (*pos >= input_words) return 0;

    uint16_t code = input[(*pos)++];
    unsigned scale = code >> 10u;
    unsigned k = 0;

    for (;;) {
        int32_t val = signed_10bit(code);
        if (!scale) {
            val *= 2;
        } else if (k == 0) {
            val *= quant[0];
        } else {
            val = (int32_t)(((int64_t)val * quant[k] * scale + 4) >> 3);
        }

        block[scale ? s_zigzag[k] : k] = clamp_val(val, -1024, 1023);

        if (*pos >= input_words) break;
        code = input[(*pos)++];
        if (code == 0xFE00u) break; /* End of block marker */

        k += (code >> 10u) + 1u;
        if (k >= 64) break;
    }

    mdec_idct_8x8(block);
    return 1;
}

/*
 * Decode one STR video frame into sceGu texture buffer in-place
 */
int PspStr_DecodeFrame(const uint8_t *str_chunk_stream, size_t total_bytes,
                       void *dst_texture, int tex_width, int tex_height, int is_16bit)
{
    const uint16_t *words = (const uint16_t *)str_chunk_stream;
    size_t num_words = total_bytes / 2u;
    size_t word_pos = 0;

    int mb_cols = tex_width / 16;
    int mb_rows = tex_height / 16;

    /* Single scratch buffer per macroblock (6 blocks of 64 int32_t = 1.5 KB) */
    int32_t mb_blocks[6][64];

    for (int mb_y = 0; mb_y < mb_rows; mb_y++) {
        for (int mb_x = 0; mb_x < mb_cols; mb_x++) {
            /* 1. Decode Cr (block 0) and Cb (block 1) */
            if (!decode_block(words, num_words, &word_pos, mb_blocks[0], s_mdec.quant_chroma)) return 0;
            if (!decode_block(words, num_words, &word_pos, mb_blocks[1], s_mdec.quant_chroma)) return 0;

            /* 2. Decode 4 Luma blocks Y1, Y2, Y3, Y4 (blocks 2..5) */
            for (int b = 2; b < 6; b++) {
                if (!decode_block(words, num_words, &word_pos, mb_blocks[b], s_mdec.quant_luma)) return 0;
            }

            /* 3. Convert 16x16 macroblock from 4:2:0 YCbCr directly into destination texture */
            for (int py = 0; py < 16; py++) {
                for (int px = 0; px < 16; px++) {
                    int uv_idx = (py / 2) * 8 + (px / 2);
                    int y_block_idx = 2 + (py / 8) * 2 + (px / 8);
                    int y_pixel_idx = (py % 8) * 8 + (px % 8);

                    int32_t lum = mb_blocks[y_block_idx][y_pixel_idx];
                    int32_t cr  = mb_blocks[0][uv_idx];
                    int32_t cb  = mb_blocks[1][uv_idx];

                    int32_t r = lum + ((359 * cr + 128) >> 8);
                    int32_t g = lum + ((-88 * cb - 183 * cr + 128) >> 8);
                    int32_t b = lum + ((454 * cb + 128) >> 8);

                    uint8_t red   = (uint8_t)clamp_val(r + 128, 0, 255);
                    uint8_t green = (uint8_t)clamp_val(g + 128, 0, 255);
                    uint8_t blue  = (uint8_t)clamp_val(b + 128, 0, 255);

                    int dst_x = mb_x * 16 + px;
                    int dst_y = mb_y * 16 + py;

                    if (is_16bit) {
                        /* 16-bit RGB5551 */
                        uint16_t *tex16 = (uint16_t *)dst_texture;
                        uint16_t col = (uint16_t)((red >> 3) | ((green >> 3) << 5) | ((blue >> 3) << 10) | 0x8000u);
                        tex16[dst_y * tex_width + dst_x] = col;
                    } else {
                        /* 32-bit RGBA8888 */
                        uint32_t *tex32 = (uint32_t *)dst_texture;
                        uint32_t col = (uint32_t)(red | (green << 8) | (blue << 16) | 0xFF000000u);
                        tex32[dst_y * tex_width + dst_x] = col;
                    }
                }
            }
        }
    }

    s_mdec.decoded_frames++;
    return 1;
}

/*
 * Stream STR video sectors from the active disc directly to sceGu texture at 15 fps
 */
int PspStr_StreamVideo(uint32_t start_lba, uint32_t sector_count,
                       void *dst_texture, int tex_width, int tex_height, int is_16bit)
{
    uint8_t sector_raw[PSP_CDROM_SECTOR_RAW];
    uint8_t frame_buffer[65536]; /* 64 KB compressed frame chunk accumulator */
    size_t  frame_bytes = 0;
    uint32_t current_lba = start_lba;
    uint32_t end_lba = start_lba + sector_count;

    while (current_lba < end_lba) {
        if (!PspCdrom_ReadRawSector(current_lba++, sector_raw)) {
            break;
        }

        /* Check Form 2 Submode byte at offset 18 */
        uint8_t submode = sector_raw[18];

        /* Check for XA Audio sector (submode bit 0x04) */
        if (submode & 0x04u) {
            /* Hand audio to SPU CD streaming queue */
            PspAudio_PushCdAudio((const int16_t *)(sector_raw + 24), 2016 / 4);
            continue;
        }

        /* Check for Video sector (submode bit 0x08 / 0x20) */
        if (submode & 0x08u) {
            const uint8_t *payload = sector_raw + PSP_CDROM_USER_OFFSET;

            /* Check STR Chunk Header magic (0x0160) */
            uint16_t magic = (uint16_t)(payload[0] | (payload[1] << 8));
            if (magic == 0x0160u) {
                uint16_t chunk_idx   = (uint16_t)(payload[4] | (payload[5] << 8));
                uint16_t num_chunks  = (uint16_t)(payload[6] | (payload[7] << 8));
                uint32_t chunk_bytes = (uint32_t)(payload[12] | (payload[13] << 8) |
                                                 (payload[14] << 16) | (payload[15] << 24));

                if (chunk_idx == 0) {
                    frame_bytes = 0;
                }

                if (frame_bytes + chunk_bytes <= sizeof(frame_buffer)) {
                    memcpy(frame_buffer + frame_bytes, payload + 32, chunk_bytes);
                    frame_bytes += chunk_bytes;
                }

                /* Complete frame assembled */
                if (chunk_idx + 1u == num_chunks && frame_bytes > 0) {
                    PspStr_DecodeFrame(frame_buffer, frame_bytes, dst_texture,
                                       tex_width, tex_height, is_16bit);

                    /* Write back D-cache for sceGu texture usage */
                    sceKernelDcacheWritebackInvalidateRange(dst_texture, (unsigned int)(tex_width * tex_height * (is_16bit ? 2 : 4)));

                    /* 15 fps movie delay: ~66 ms per frame */
                    SDL_Delay(66);
                    frame_bytes = 0;
                }
            }
        }
    }

    return 1;
}
