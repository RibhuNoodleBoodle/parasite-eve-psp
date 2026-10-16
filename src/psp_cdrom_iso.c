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
#include "psp_gpu_gu.h"

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
    uint32_t     sector_stride;        /* 2352 for raw Mode 2 Form 1, 2048 for ISO */
    uint32_t     user_offset;          /* 24 for raw Mode 2 Form 1, 0 for ISO */
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

    /* Lower thread priority so background reader never starves main thread */
    sceKernelChangeThreadPriority(0, 0x38);

    while (s_cd.worker_running) {
        uint32_t next_lba;
        uint32_t write_slot;
        uint8_t temp_sector[PSP_CDROM_SECTOR_RAW];
        size_t read_bytes;

        SDL_LockMutex(s_cd.lock);

        /* Wait while ring buffer is completely full, paused, or idle */
        while (s_cd.worker_running && !s_cd.seeking &&
               ((s_cd.ring_count >= PSP_CDROM_RING_SECTORS) || !(s_cd.status & PSP_CD_STAT_READING))) {
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
                uint32_t stride = (s_cd.sector_stride > 0) ? s_cd.sector_stride : PSP_CDROM_SECTOR_RAW;
                sceIoLseek(s_cd.fd, (SceOff)s_cd.current_lba * stride, PSP_SEEK_SET);
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
        uint32_t stride = (s_cd.sector_stride > 0) ? s_cd.sector_stride : PSP_CDROM_SECTOR_RAW;
        SDL_UnlockMutex(s_cd.lock);

        /* Perform physical file read outside of lock to prevent stalling main thread */
        read_bytes = (size_t)sceIoRead(s_cd.fd, temp_sector, stride);

        SDL_LockMutex(s_cd.lock);
        if (read_bytes == stride && !s_cd.seeking) {
            memcpy(s_cd.ring_buffer + (write_slot * PSP_CDROM_SECTOR_RAW), temp_sector, stride);
            s_cd.ring_lbas[write_slot] = next_lba;
            s_cd.ring_write_idx = (s_cd.ring_write_idx + 1u) % PSP_CDROM_RING_SECTORS;
            s_cd.ring_count++;
            s_cd.current_lba++;
            SDL_CondSignal(s_cd.cond_data_avail);
        }
        SDL_UnlockMutex(s_cd.lock);

        /* Yield briefly to give time to main user thread */
        sceKernelDelayThread(200);
    }

    return 0;
}

/* ── CD-ROM Subsystem Lifecycle ─────────────────────────────────────────────── */

/* Search candidate paths for Disc 1 and Disc 2 (supporting both .bin and .BIN extensions) */
static const char * const s_disc1_search_paths[] = {
    "disc/PE_DISC1.bin",
    "disc/PE_DISC1.BIN",
    "./PE_DISC1.bin",
    "./PE_DISC1.BIN",
    "./disc/PE_DISC1.bin",
    "./disc/PE_DISC1.BIN",
    "ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC1.bin",
    "ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC1.BIN",
    "disc0:/disc/PE_DISC1.bin",
    "disc0:/disc/PE_DISC1.BIN",
    "host0:/disc/PE_DISC1.bin",
    "host0:/disc/PE_DISC1.BIN",
    NULL
};

static const char * const s_disc2_search_paths[] = {
    "disc/PE_DISC2.bin",
    "disc/PE_DISC2.BIN",
    "./PE_DISC2.bin",
    "./PE_DISC2.BIN",
    "./disc/PE_DISC2.bin",
    "./disc/PE_DISC2.BIN",
    "ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC2.bin",
    "ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC2.BIN",
    "disc0:/disc/PE_DISC2.bin",
    "disc0:/disc/PE_DISC2.BIN",
    "host0:/disc/PE_DISC2.bin",
    "host0:/disc/PE_DISC2.BIN",
    NULL
};

static void fatal_disc_not_found(int disc_num, const char * const candidates[], int last_error)
{
    const char *disc_name = (disc_num == 2) ? "PE_DISC2.bin / PE_DISC2.BIN" : "PE_DISC1.bin / PE_DISC1.BIN";
    char log_buf[512];
    int len;

    /* a) Write [PE-PSP] FATAL ERROR: File not found to stderr */
    fprintf(stderr, "[PE-PSP] FATAL ERROR: File not found: %s (last sceIoOpen error: 0x%08X)\n",
            disc_name, (unsigned int)last_error);
    len = snprintf(log_buf, sizeof(log_buf),
                   "\n[PE-PSP] FATAL ERROR: File not found: %s (last sceIoOpen error: 0x%08X)\nAttempted paths:\n",
                   disc_name, (unsigned int)last_error);
    sceIoWrite(2, log_buf, len);

    for (int i = 0; candidates[i] != NULL; i++) {
        len = snprintf(log_buf, sizeof(log_buf), "  [%d] %s\n", i + 1, candidates[i]);
        sceIoWrite(2, log_buf, len);
    }

    /* b) Initialize PSP hardware debug screen with red text */
    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0xFF000080); /* Dark red background */
    pspDebugScreenSetTextColor(0xFF0000FF); /* Bright red text (ABGR) */
    pspDebugScreenClear();

    pspDebugScreenPrintf("\n");
    pspDebugScreenPrintf("  ======================================================\n");
    pspDebugScreenPrintf("          PARASITE EVE PSP PORT - FATAL ERROR           \n");
    pspDebugScreenPrintf("  ======================================================\n\n");
    pspDebugScreenPrintf("  FATAL ERROR: File not found: PE_DISC1.bin / PE_DISC1.BIN\n");
    pspDebugScreenPrintf("  Missing target: %s\n\n", disc_name);
    pspDebugScreenPrintf("  Place PE_DISC1.bin and PE_DISC2.bin (or .BIN) in:\n");
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
    const char *found_path1 = NULL;
    const char *found_path2 = NULL;

    for (int i = 0; s_disc1_search_paths[i] != NULL; i++) {
        SceUID fd = sceIoOpen(s_disc1_search_paths[i], PSP_O_RDONLY, 0);
        if (fd >= 0) {
            sceIoClose(fd);
            disc1_found = 1;
            found_path1 = s_disc1_search_paths[i];
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
            found_path2 = s_disc2_search_paths[i];
            break;
        } else {
            last_err2 = (int)fd;
        }
    }

    if (!disc1_found || !disc2_found) {
        char err_msg[768];
        int len;

        fprintf(stderr, "[PE-PSP] FATAL ERROR: File not found: PE_DISC1.bin / PE_DISC1.BIN\n");
        len = snprintf(err_msg, sizeof(err_msg),
            "\n[PE-PSP] FATAL ERROR: File not found: PE_DISC1.bin / PE_DISC1.BIN\n"
            "Disc 1 found: %s (last err: 0x%08X)\n"
            "Disc 2 found: %s (last err: 0x%08X)\n"
            "Attempted search locations:\n"
            "  1. disc/PE_DISC1.bin | disc/PE_DISC1.BIN | disc/PE_DISC2.bin | disc/PE_DISC2.BIN\n"
            "  2. ./PE_DISC1.bin | ./PE_DISC1.BIN | ./PE_DISC2.bin | ./PE_DISC2.BIN\n"
            "  3. ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC1.bin | .BIN | PE_DISC2.bin | .BIN\n"
            "  4. host0:/disc/PE_DISC1.bin | .BIN | PE_DISC2.bin | .BIN\n",
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
        pspDebugScreenPrintf("  FATAL ERROR: File not found: PE_DISC1.bin / PE_DISC1.BIN\n\n");
        pspDebugScreenPrintf("  Status:\n");
        pspDebugScreenPrintf("    - PE_DISC1.bin / .BIN: %s\n", disc1_found ? "FOUND" : "MISSING");
        pspDebugScreenPrintf("    - PE_DISC2.bin / .BIN: %s\n\n", disc2_found ? "FOUND" : "MISSING");
        pspDebugScreenPrintf("  Please place PE_DISC1.bin and PE_DISC2.bin (or .BIN) into:\n");
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

    {
        char ok_msg[256];
        int len = snprintf(ok_msg, sizeof(ok_msg),
            "[PE-PSP] Disc check passed: Disc1='%s', Disc2='%s'\n",
            found_path1 ? found_path1 : "OK",
            found_path2 ? found_path2 : "OK");
        sceIoWrite(2, ok_msg, len);
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
    const char *disc_name = (disc_num == 2) ? "PE_DISC2.bin / PE_DISC2.BIN" : "PE_DISC1.bin / PE_DISC1.BIN";
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

        /* Detect Mode 2 Form 1 (2352 bytes/sector) vs ISO 9660 (2048 bytes/sector) */
        if (size > 0 && (size % PSP_CDROM_SECTOR_RAW == 0)) {
            s_cd.sector_stride = PSP_CDROM_SECTOR_RAW; /* 2352 */
            s_cd.user_offset   = PSP_CDROM_USER_OFFSET; /* 24 */
        } else {
            s_cd.sector_stride = PSP_CDROM_SECTOR_USER; /* 2048 */
            s_cd.user_offset   = 0;
        }

        s_cd.total_sectors = (uint32_t)(size / s_cd.sector_stride);
        s_cd.current_lba = 0;
        s_cd.target_lba = 0;
        s_cd.seeking = 0;
        s_cd.active_disc = disc_num;
        s_cd.status = 0x02; /* CdlStatStandby */

        char msg[256];
        int len = snprintf(msg, sizeof(msg),
                 "[PE-PSP] [PSP_CDROM] Opened %s from %s (%u sectors, %lu MB, stride=%u, offset=+%u, fd=%d)\n",
                 disc_name,
                 (disc_num == 2 ? s_cd.disc2_path : s_cd.disc1_path),
                 (unsigned)s_cd.total_sectors,
                 (unsigned long)(size / (1024 * 1024)),
                 (unsigned)s_cd.sector_stride,
                 (unsigned)s_cd.user_offset,
                 (int)s_cd.fd);
        sceIoWrite(2, msg, len);
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
    if (!out_raw || s_cd.fd < 0 || lba >= s_cd.total_sectors) return 0;

    SDL_LockMutex(s_cd.lock);
    uint32_t stride = (s_cd.sector_stride > 0) ? s_cd.sector_stride : PSP_CDROM_SECTOR_RAW;

    /* 1. Fast path: check if sector is already present in 256 KB ring buffer */
    for (uint32_t i = 0; i < s_cd.ring_count; i++) {
        uint32_t slot = (s_cd.ring_read_idx + i) % PSP_CDROM_RING_SECTORS;
        if (s_cd.ring_lbas[slot] == lba) {
            memcpy(out_raw, s_cd.ring_buffer + (slot * PSP_CDROM_SECTOR_RAW), stride);
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
    sceIoLseek(s_cd.fd, (SceOff)lba * stride, PSP_SEEK_SET);
    int n = sceIoRead(s_cd.fd, out_raw, stride);
    SDL_CondSignal(s_cd.cond_space_avail);
    SDL_UnlockMutex(s_cd.lock);

    return n == (int)stride;
}

int PspCdrom_ReadUserSector(uint32_t lba, uint8_t *out_user)
{
    if (!out_user || s_cd.fd < 0 || lba >= s_cd.total_sectors) return 0;

    SDL_LockMutex(s_cd.lock);
    uint32_t stride = (s_cd.sector_stride > 0) ? s_cd.sector_stride : PSP_CDROM_SECTOR_RAW;
    SceOff offset = ((SceOff)lba * stride) + s_cd.user_offset;
    sceIoLseek(s_cd.fd, offset, PSP_SEEK_SET);
    int read_bytes = sceIoRead(s_cd.fd, out_user, PSP_CDROM_SECTOR_USER);
    SDL_UnlockMutex(s_cd.lock);

    return (read_bytes == (int)PSP_CDROM_SECTOR_USER);
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

int CdPosToInt(CdlLOC *msf)
{
    if (!msf) return 0;
    const unsigned char *b = (const unsigned char *)msf;
    int m = (b[0] >> 4) * 10 + (b[0] & 0x0F);
    int s = (b[1] >> 4) * 10 + (b[1] & 0x0F);
    int f = (b[2] >> 4) * 10 + (b[2] & 0x0F);
    return (m * 60 + s) * 75 + f - 150;
}

CdlLOC *CdIntToPos(int lba, CdlLOC *msf)
{
    if (!msf) return NULL;
    unsigned char *b = (unsigned char *)msf;
    int total = lba + 150;
    int f = total % 75;
    total /= 75;
    int s = total % 60;
    int m = total / 60;
    b[0] = (unsigned char)(((m / 10) << 4) | (m % 10));
    b[1] = (unsigned char)(((s / 10) << 4) | (s % 10));
    b[2] = (unsigned char)(((f / 10) << 4) | (f % 10));
    return msf;
}

static void fire_cd_sync_callback(void);
static void fire_cd_ready_callback(void);
static void fire_cd_read_callback(void);

int CdControl(unsigned char com, unsigned char *param, unsigned char *result)
{
    if (result) *result = (s_cd.status != 0) ? s_cd.status : 0x02; /* 0x02 = CdlStatStandby */

    switch (com) {
    case CdlNop:
        return 1;
    case CdlSetloc:
        if (param) {
            uint32_t lba = (uint32_t)CdPosToInt((CdlLOC *)param);
            s_cd.current_lba = lba;
            PspCdrom_SeekLba(lba);
        }
        return 1;
    case CdlSeekL:
    case CdlSeekP:
        if (param) {
            uint32_t lba = (uint32_t)CdPosToInt((CdlLOC *)param);
            s_cd.current_lba = lba;
            PspCdrom_SeekLba(lba);
        }
        s_cd.status = 0x02; /* CdlStatStandby */
        fire_cd_sync_callback();
        return 1;
    case CdlStandby:
    case CdlPause:
    case CdlStop:
        s_cd.status &= (uint8_t)~PSP_CD_STAT_READING;
        s_cd.status |= PSP_CD_STAT_IDLE;
        fire_cd_sync_callback();
        return 1;
    case CdlSetmode:
        return 1;
    default:
        return 1;
    }
}

int CdControlF(unsigned char com, unsigned char *param)
{
    return CdControl(com, param, NULL);
}

int CdControlB(unsigned char com, unsigned char *param, unsigned char *result)
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

static int s_cd_sync_cb_depth = 0;
static void fire_cd_sync_callback(void)
{
    unsigned char res[8];
    memset(res, 0, sizeof(res));
    res[0] = s_cd.status ? s_cd.status : 0x02; /* CdlStatStandby (0x02) */
    if (s_cd_sync_cb_depth > 8) return;
    s_cd_sync_cb_depth++;
    if (s_cd_sync_cb) {
        s_cd_sync_cb(0x02 /* CdlComplete */, res);
    }
    if (g_CdSyncCallback) {
        g_CdSyncCallback(0x02 /* CdlComplete */, res);
    }
    s_cd_sync_cb_depth--;
}

static int s_cd_ready_cb_depth = 0;
static void fire_cd_ready_callback(void)
{
    unsigned char res[8];
    memset(res, 0, sizeof(res));
    res[0] = s_cd.status ? s_cd.status : 0x02; /* CdlStatStandby (0x02) */
    if (s_cd_ready_cb_depth > 8) return;
    s_cd_ready_cb_depth++;
    if (s_cd_ready_cb) {
        s_cd_ready_cb(0x01 /* CdlDataReady */, res);
    }
    if (g_CdReadyCallback) {
        g_CdReadyCallback(0x01 /* CdlDataReady */, res);
    }
    s_cd_ready_cb_depth--;
}

static int s_cd_read_cb_depth = 0;
static void fire_cd_read_callback(void)
{
    unsigned char res[8];
    memset(res, 0, sizeof(res));
    res[0] = s_cd.status ? s_cd.status : 0x02; /* CdlStatStandby (0x02) */
    if (s_cd_read_cb_depth > 8) return;
    s_cd_read_cb_depth++;
    if (s_cd_read_cb) {
        s_cd_read_cb(0x02 /* CdlComplete */, res);
    }
    if (g_CdReadCallback) {
        g_CdReadCallback(0x02 /* CdlComplete */, res);
    }
    s_cd_read_cb_depth--;
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

/* ── PS1 Psy-Q libds & ISO9660 Directory Traversal ─────────────────────────── */

#define DECODE_BCD(x) (((x) >> 4) * 10 + ((x) & 0xF))
int DsPosToInt(CdlLOC *p)
{
    if (!p) return 0;
    return (DECODE_BCD(p->minute) * 60 + DECODE_BCD(p->second)) * 75 +
           DECODE_BCD(p->sector) - 150;
}

static inline int encode_bcd(int n)
{
    return ((n / 10) << 4) + (n % 10);
}

CdlLOC *DsIntToPos(int i, CdlLOC *p)
{
    if (!p) return NULL;
    i += 150;
    p->sector = (uint8_t)encode_bcd(i % 75);
    p->second = (uint8_t)encode_bcd((i / 75) % 60);
    p->minute = (uint8_t)encode_bcd((i / 75) / 60);
    p->track  = 1;
    return p;
}

DslFILE *DsSearchFile(DslFILE *file, char *name)
{
    static DslFILE s_static_file;
    DslFILE *out_file = file ? file : &s_static_file;
    if (!name || !name[0]) return NULL;

    uint8_t pvd[PSP_CDROM_SECTOR_USER];
    if (!PspCdrom_ReadUserSector(16, pvd)) {
        fprintf(stderr, "[ISO9660] Failed to read PVD at sector 16\n");
        return NULL;
    }

    if (memcmp(pvd + 1, "CD001", 5) != 0) {
        fprintf(stderr, "[ISO9660] Invalid PVD signature\n");
        return NULL;
    }

    /* Root directory record at offset 156 */
    uint32_t dir_lba  = (uint32_t)pvd[158] | ((uint32_t)pvd[159] << 8) |
                        ((uint32_t)pvd[160] << 16) | ((uint32_t)pvd[161] << 24);
    uint32_t dir_size = (uint32_t)pvd[166] | ((uint32_t)pvd[167] << 8) |
                        ((uint32_t)pvd[168] << 16) | ((uint32_t)pvd[169] << 24);

    char path_buf[256];
    strncpy(path_buf, name, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';

    char *tokens[16];
    int num_tokens = 0;
    char *p = path_buf;
    while (*p == '\\' || *p == '/') p++;
    while (*p && num_tokens < 16) {
        tokens[num_tokens++] = p;
        while (*p && *p != '\\' && *p != '/') p++;
        if (*p) {
            *p++ = '\0';
            while (*p == '\\' || *p == '/') p++;
        }
    }

    if (num_tokens == 0) return NULL;

    uint8_t dir_buf[PSP_CDROM_SECTOR_USER];

    for (int t = 0; t < num_tokens; t++) {
        const char *target = tokens[t];
        int is_last = (t == num_tokens - 1);
        int found_token = 0;

        uint32_t sectors_to_read = (dir_size + PSP_CDROM_SECTOR_USER - 1) / PSP_CDROM_SECTOR_USER;
        if (sectors_to_read > 4) sectors_to_read = 4;

        for (uint32_t s = 0; s < sectors_to_read && !found_token; s++) {
            if (!PspCdrom_ReadUserSector(dir_lba + s, dir_buf)) break;

            uint32_t offset = 0;
            while (offset < PSP_CDROM_SECTOR_USER) {
                uint8_t rec_len = dir_buf[offset];
                if (rec_len == 0) break;

                uint32_t rec_lba  = (uint32_t)dir_buf[offset + 2] |
                                    ((uint32_t)dir_buf[offset + 3] << 8) |
                                    ((uint32_t)dir_buf[offset + 4] << 16) |
                                    ((uint32_t)dir_buf[offset + 5] << 24);
                uint32_t rec_size = (uint32_t)dir_buf[offset + 10] |
                                    ((uint32_t)dir_buf[offset + 11] << 8) |
                                    ((uint32_t)dir_buf[offset + 12] << 16) |
                                    ((uint32_t)dir_buf[offset + 13] << 24);
                uint8_t flags     = dir_buf[offset + 25];
                uint8_t name_len  = dir_buf[offset + 32];
                const char *rec_name = (const char *)&dir_buf[offset + 33];

                char entry_name[64];
                int copy_len = (name_len < 63) ? name_len : 63;
                memcpy(entry_name, rec_name, copy_len);
                entry_name[copy_len] = '\0';

                int match = (strcasecmp(target, entry_name) == 0);
                if (!match) {
                    char t_strip[64], e_strip[64];
                    strncpy(t_strip, target, 63); t_strip[63] = '\0';
                    char *sc = strchr(t_strip, ';'); if (sc) *sc = '\0';
                    strncpy(e_strip, entry_name, 63); e_strip[63] = '\0';
                    sc = strchr(e_strip, ';'); if (sc) *sc = '\0';
                    if (strcasecmp(t_strip, e_strip) == 0) match = 1;
                }

                if (match) {
                    if (is_last) {
                        out_file->size = rec_size;
                        DsIntToPos((int)rec_lba, &out_file->pos);
                        strncpy(out_file->name, target, 15);
                        out_file->name[15] = '\0';
                        fprintf(stderr, "[ISO9660] DsSearchFile: found '%s' -> LBA %u, size %u bytes\n",
                                name, (unsigned)rec_lba, (unsigned)rec_size);
                        return out_file;
                    } else if (flags & 2) {
                        dir_lba = rec_lba;
                        dir_size = rec_size;
                        found_token = 1;
                        break;
                    }
                }

                offset += rec_len;
            }
        }

        if (!found_token && !is_last) {
            fprintf(stderr, "[ISO9660] DsSearchFile: directory component '%s' not found for '%s'\n", target, name);
            return NULL;
        }
    }

    fprintf(stderr, "[ISO9660] DsSearchFile: '%s' not found on disc\n", name);
    return NULL;
}

int PspCdrom_ReadDiscSectors(uint32_t lba, uint32_t offset, void *destination, uint32_t size)
{
    if (!destination || size == 0) return 0;
    uint8_t *dst = (uint8_t *)destination;
    uint32_t start_lba = lba + offset;
    for (uint32_t i = 0; i < size; i++) {
        if (!PspCdrom_ReadUserSector(start_lba + i, dst + (i * PSP_CDROM_SECTOR_USER))) {
            memset(dst + (i * PSP_CDROM_SECTOR_USER), 0, PSP_CDROM_SECTOR_USER);
        }
    }
    return 0;
}

int __attribute__((weak)) CdRom_ReadSectors(u32 lba, u32 offset, void *destination, u32 size)
{
    return PspCdrom_ReadDiscSectors(lba, offset, destination, size);
}

int __attribute__((weak)) CdRom_ReadSectorsFromLba(u32 lba, void *destination, u32 size)
{
    return PspCdrom_ReadDiscSectors(lba, 0, destination, size);
}

int __attribute__((weak)) CdRom_PollReady(void)
{
    return 0;
}

int DsReadSync(void *argument)
{
    (void)argument;
    return 0;
}

int DsReady(uint8_t *destination)
{
    (void)destination;
    return 0;
}

int DsRead(CdlLOC *position, int sectors, void *destination, int mode)
{
    (void)mode;
    if (!position || !destination) return 0;
    int lba = DsPosToInt(position);
    int res = PspCdrom_ReadDiscSectors((uint32_t)lba, 0, destination, (uint32_t)sectors);
    return (res == 0) ? 1 : 0;
}

int DsControl(unsigned char command, void *param, void *result)
{
    return CdControl(command, (unsigned char *)param, (unsigned char *)result);
}

int DsControlB(unsigned char command, void *param, void *result)
{
    return CdControlB(command, (unsigned char *)param, (unsigned char *)result);
}

int DsSystemStatus(void)
{
    return 1;
}

int DsShellOpen(void)
{
    return 0;
}

int DsQueueLen(void)
{
    return 0;
}

int DsGetDiskType(void)
{
    return 4; /* PS1 CD-ROM disc */
}

void DS_restart(void) {}
int DS_system_active(void) { return 1; }
int DsReset(void) { return 1; }
int ER_retry(void) { return 0; }
int DS_lastcom(void) { return 0; }
int DS_lastmode(void) { return 0; }
static CdlLOC s_last_pos = {0};
CdlLOC *DS_lastpos(void) { return &s_last_pos; }
CdlLOC *DsLastPos(CdlLOC *destination) {
    if (destination) *destination = s_last_pos;
    return &s_last_pos;
}

void DsInit(void) {}
void DsSetDebug(int level) { (void)level; }
int DsCommand(unsigned char com, const unsigned char *param, int mode) {
    (void)com; (void)param; (void)mode;
    return 1;
}
int DsReadBreak(void) { return 0; }
int DsStatus(void) { return 1; }
int DsSync(int mode, unsigned char *result) {
    (void)mode;
    if (result) *result = 0;
    return 0;
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

/* ── PS1 Psy-Q Streaming Ring Buffer (St*) ──────────────────────────────────── */

typedef struct StHEADER {
    uint16_t id;
    uint16_t type;
    uint16_t secCount;
    uint16_t nSectors;
    uint32_t frameCount;
    uint32_t frameSize;
    uint16_t width;
    uint16_t height;
    uint32_t dummy1;
    uint32_t dummy2;
    CdlLOC   loc;
} StHEADER;

static StHEADER *s_st_ring_addr = NULL;
static uint32_t  s_st_ring_size = 0;
static uint32_t  s_st_read_slot = 0;
static uint32_t  s_st_write_slot = 0;
static uint32_t  s_st_stream_lba = 0;
static int       s_st_stream_active = 0;
static void    (*s_st_end_callback)(void) = NULL;

extern void MdecDiag_ExpandDecoderTable(uint16_t *table);

static uint16_t s_vlc_decoder_table[34816];
static int      s_vlc_table_initialized = 0;

void StSetRing(u32 *ring_addr, u32 ring_size)
{
    s_st_ring_addr = (StHEADER *)ring_addr;
    s_st_ring_size = (ring_size > 0) ? ring_size : 64;
    s_st_read_slot = 0;
    s_st_write_slot = 0;
    StClearRing();
}

void StClearRing(void)
{
    s_st_read_slot = 0;
    s_st_write_slot = 0;
    if (s_st_ring_addr && s_st_ring_size > 0) {
        memset(s_st_ring_addr, 0, s_st_ring_size * sizeof(StHEADER));
    }
}

void StUnSetRing(void)
{
    s_st_stream_active = 0;
}

void StSetStream(u32 mode, u32 startFrame, u32 endFrame, void (*callback1)(void), void (*callback2)(void))
{
    (void)mode;
    (void)startFrame;
    (void)endFrame;
    (void)callback2;
    s_st_stream_active = 1;
    s_st_end_callback = callback1;
}

int StGetBackloc(CdlLOC *loc)
{
    if (loc) {
        uint32_t lba = s_st_stream_lba + 150u;
        loc->minute = (uint8_t)(lba / (60u * 75u));
        loc->second = (uint8_t)((lba / 75u) % 60u);
        loc->sector = (uint8_t)(lba % 75u);
        loc->track  = 1;
    }
    return 0;
}

static void psp_str_pump_sectors(void)
{
    if (!s_st_ring_addr || s_st_ring_size == 0) return;

    uint8_t sector_raw[PSP_CDROM_SECTOR_RAW];
    static uint8_t s_frame_payloads[32][2016];
    static uint8_t s_frame_headers[32][32];
    static uint32_t s_asm_frame = 0;
    static uint32_t s_asm_mask = 0;
    static uint32_t s_asm_chunks = 0;
    static int s_assembling = 0;

    int max_sectors = 40;
    while (max_sectors-- > 0) {
        if (!PspCdrom_ReadRawSector(s_st_stream_lba++, sector_raw)) {
            break;
        }

        uint8_t submode = sector_raw[18];

        /* Check for XA audio (submode bit 0x04) */
        if (submode & 0x04u) {
            PspAudio_PushCdAudio((const int16_t *)(sector_raw + 24), 2016 / 4);
            continue;
        }

        /* Check for STR video chunk (submode bit 0x08 and magic 0x0160) */
        const uint8_t *user = sector_raw + 24;
        uint16_t magic = (uint16_t)(user[0] | (user[1] << 8));
        if ((submode & 0x08u) && magic == 0x0160u) {
            uint16_t chunk_idx  = (uint16_t)(user[4] | (user[5] << 8));
            uint16_t num_chunks = (uint16_t)(user[6] | (user[7] << 8));
            uint32_t frame_no   = (uint32_t)(user[8] | (user[9] << 8) |
                                            (user[10] << 16) | (user[11] << 24));

            if (num_chunks == 0 || num_chunks > 32 || chunk_idx >= num_chunks) continue;

            if (s_assembling && (frame_no != s_asm_frame || num_chunks != s_asm_chunks)) {
                s_assembling = 0;
            }

            if (!s_assembling) {
                s_assembling = 1;
                s_asm_frame = frame_no;
                s_asm_chunks = num_chunks;
                s_asm_mask = 0;
            }

            memcpy(s_frame_headers[chunk_idx], user, 32);
            memcpy(s_frame_payloads[chunk_idx], user + 32, 2016);
            s_asm_mask |= (1u << chunk_idx);

            uint32_t complete_mask = (num_chunks >= 32) ? 0xFFFFFFFFu : ((1u << num_chunks) - 1u);
            if (s_asm_mask == complete_mask) {
                s_assembling = 0;
                uint32_t w = s_st_write_slot;
                uint8_t *data_base = (uint8_t *)s_st_ring_addr + s_st_ring_size * 32u;

                for (uint32_t k = 0; k < num_chunks; k++) {
                    uint32_t slot = (w + k) % s_st_ring_size;
                    StHEADER *hdr = &s_st_ring_addr[slot];
                    memcpy(hdr, s_frame_headers[k], 32);
                    memcpy(data_base + slot * 2016u, s_frame_payloads[k], 2016);
                    hdr->nSectors = (uint16_t)num_chunks;
                    hdr->id = (k == 0) ? 2 : 3;
                }
                s_st_ring_addr[w % s_st_ring_size].id = 2; /* publish frame! */
                s_st_write_slot = (w + num_chunks) % s_st_ring_size;
                break;
            }
        }
    }
}

u32 StGetNext(u32 **addr, u32 **header)
{
    if (!s_st_ring_addr || s_st_ring_size == 0) return 1;

    if (s_st_ring_addr[s_st_read_slot].id != 2) {
        psp_str_pump_sectors();
    }

    if (s_st_ring_addr[s_st_read_slot].id == 2) {
        s_st_ring_addr[s_st_read_slot].id = 4;
        uint8_t *data_base = (uint8_t *)s_st_ring_addr + s_st_ring_size * 32u;
        if (header) *header = (u32 *)&s_st_ring_addr[s_st_read_slot];
        if (addr)   *addr   = (u32 *)(data_base + s_st_read_slot * 2016u);
        return 0;
    }

    return 1;
}

u32 StFreeRing(u32 *ptr)
{
    if (!s_st_ring_addr || s_st_ring_size == 0 || !ptr) return 1;
    uint8_t *data_base = (uint8_t *)s_st_ring_addr + s_st_ring_size * 32u;
    uint32_t byte_diff = (uint8_t *)ptr - data_base;
    uint32_t slot = (byte_diff / 2016u) % s_st_ring_size;
    uint32_t count = s_st_ring_addr[slot].nSectors;
    if (count == 0 || count > s_st_ring_size) count = 1;

    for (uint32_t k = 0; k < count; k++) {
        s_st_ring_addr[(slot + k) % s_st_ring_size].id = 0;
    }
    s_st_read_slot = (slot + count) % s_st_ring_size;
    return 0;
}

/* ── PS1 Psy-Q MDEC Decompression API (DecDCT*) ────────────────────────────── */

static void (*s_decdct_out_callback)(void) = NULL;
static void (*s_decdct_in_callback)(void) = NULL;

static const uint16_t *s_mdec_input_words = NULL;
static size_t          s_mdec_input_word_count = 0;
static size_t          s_mdec_word_pos = 0;
static int             s_mdec_depth = 2; /* 2 = 24-bit RGB888, 3 = 16-bit RGB1555 */
static int             s_mdec_slice_col = 0;
static int             s_mdec_mb_cols = 20; /* 320 / 16 */
static int             s_mdec_mb_rows = 15; /* 240 / 16 */

void DecDCTReset(int mode)
{
    (void)mode;
    s_mdec_slice_col = 0;
    s_mdec_word_pos = 0;
    PspMdec_Reset();
}

void DecDCTinCallback(void *callback)
{
    s_decdct_in_callback = (void (*)(void))callback;
}

void DecDCToutCallback(void *callback)
{
    s_decdct_out_callback = (void (*)(void))callback;
}

s32 DecDCTinSync(s32 mode)
{
    (void)mode;
    return 0;
}

s32 DecDCToutSync(s32 mode)
{
    (void)mode;
    return 0;
}

void DecDCTvlc(void *bs, void *buf)
{
    if (!s_vlc_table_initialized) {
        MdecDiag_ExpandDecoderTable(s_vlc_decoder_table);
        s_vlc_table_initialized = 1;
    }
    DecDCTvlc2(bs, buf, s_vlc_decoder_table);
}

void DecDCTvlcSize(u32 size)
{
    DecDCTvlcSize2(size);
}

void DecDCTin(void *buf, s32 mode)
{
    if (!buf) return;
    uint32_t *cmd_ptr = (uint32_t *)buf;

    if (mode & 1) {
        *cmd_ptr &= ~0x08000000u;
    } else {
        *cmd_ptr |= 0x08000000u;
    }
    if (mode & 2) {
        *cmd_ptr |= 0x02000000u;
    } else {
        *cmd_ptr &= ~0x02000000u;
    }

    uint32_t cmd = *cmd_ptr;
    s_mdec_depth = (cmd >> 27u) & 3u;
    s_mdec_input_words = (const uint16_t *)buf + 2; /* skip 32-bit command header */
    s_mdec_input_word_count = (cmd & 0xFFFFu) * 2u;
    s_mdec_word_pos = 0;
    s_mdec_slice_col = 0;

    if (s_decdct_in_callback) {
        s_decdct_in_callback();
    }
}

void DecDCTout(void *buf, u32 size)
{
    (void)size;
    if (!buf) return;

    int is_16bit = (s_mdec_depth == 3);
    int mb_x = s_mdec_slice_col;

    if (mb_x < s_mdec_mb_cols && s_mdec_input_words) {
        int32_t mb_blocks[6][64];
        for (int mb_y = 0; mb_y < s_mdec_mb_rows; mb_y++) {
            /* Decode Cr (block 0) and Cb (block 1) */
            if (decode_block(s_mdec_input_words, s_mdec_input_word_count, &s_mdec_word_pos, mb_blocks[0], s_mdec.quant_chroma) &&
                decode_block(s_mdec_input_words, s_mdec_input_word_count, &s_mdec_word_pos, mb_blocks[1], s_mdec.quant_chroma)) {
                /* Decode Y1, Y2, Y3, Y4 */
                for (int b = 2; b < 6; b++) {
                    decode_block(s_mdec_input_words, s_mdec_input_word_count, &s_mdec_word_pos, mb_blocks[b], s_mdec.quant_luma);
                }
            }

            /* Convert 16x16 macroblock to vertical slice buffer */
            for (int py = 0; py < 16; py++) {
                int row = mb_y * 16 + py;
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

                    if (is_16bit) {
                        uint16_t *dst_row = (uint16_t *)buf + row * 16;
                        dst_row[px] = (uint16_t)((red >> 3) | ((green >> 3) << 5) | ((blue >> 3) << 10) | 0x8000u);
                    } else {
                        uint8_t *dst_row = (uint8_t *)buf + row * (16 * 3);
                        dst_row[px * 3 + 0] = red;
                        dst_row[px * 3 + 1] = green;
                        dst_row[px * 3 + 2] = blue;
                    }
                }
            }
        }
        s_mdec_slice_col++;
    }

    /* IMMEDIATELY invoke registered DecDCToutCallback */
    if (s_decdct_out_callback) {
        s_decdct_out_callback();
    }
}

/*
 * Decode one STR video frame into destination buffer or shadow VRAM
 */
int PspStr_DecodeFrame(const uint8_t *str_chunk_stream, size_t total_bytes,
                       void *dst_texture, int tex_width, int tex_height, int is_16bit)
{
    const uint16_t *words = (const uint16_t *)str_chunk_stream;
    size_t num_words = total_bytes / 2u;
    size_t word_pos = 2; /* skip 32-bit MDEC command word */

    if (tex_width <= 0)  tex_width = 320;
    if (tex_height <= 0) tex_height = 240;

    int mb_cols = tex_width / 16;
    int mb_rows = tex_height / 16;

    if (!dst_texture) {
        dst_texture = g_ps1_vram;
    }

    int32_t mb_blocks[6][64];

    for (int mb_y = 0; mb_y < mb_rows; mb_y++) {
        for (int mb_x = 0; mb_x < mb_cols; mb_x++) {
            /* 1. Decode Cr (block 0) and Cb (block 1) */
            if (!decode_block(words, num_words, &word_pos, mb_blocks[0], s_mdec.quant_chroma)) break;
            if (!decode_block(words, num_words, &word_pos, mb_blocks[1], s_mdec.quant_chroma)) break;

            /* 2. Decode 4 Luma blocks Y1, Y2, Y3, Y4 (blocks 2..5) */
            for (int b = 2; b < 6; b++) {
                decode_block(words, num_words, &word_pos, mb_blocks[b], s_mdec.quant_luma);
            }

            /* 3. Convert 16x16 macroblock from 4:2:0 YCbCr directly into destination buffer */
            for (int py = 0; py < 16; py++) {
                int dst_y = mb_y * 16 + py;
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

                    if (is_16bit) {
                        uint16_t *tex16 = (uint16_t *)dst_texture;
                        uint16_t col = (uint16_t)((red >> 3) | ((green >> 3) << 5) | ((blue >> 3) << 10) | 0x8000u);
                        size_t stride = (dst_texture == g_ps1_vram) ? PS1_VRAM_WIDTH : (size_t)tex_width;
                        tex16[dst_y * stride + dst_x] = col;
                    } else {
                        uint8_t *vram_bytes = (uint8_t *)dst_texture;
                        size_t row_stride = (dst_texture == g_ps1_vram) ? (PS1_VRAM_WIDTH * 2) : (size_t)(tex_width * 3);
                        vram_bytes[dst_y * row_stride + dst_x * 3 + 0] = red;
                        vram_bytes[dst_y * row_stride + dst_x * 3 + 1] = green;
                        vram_bytes[dst_y * row_stride + dst_x * 3 + 2] = blue;
                    }
                }
            }
        }
    }

    s_mdec.decoded_frames++;
    return 1;
}

/*
 * Stream STR video sectors from the active disc directly into VRAM / display at 15 fps
 */
int PspStr_StreamVideo(uint32_t start_lba, uint32_t sector_count,
                       void *dst_texture, int tex_width, int tex_height, int is_16bit)
{
    if (!s_vlc_table_initialized) {
        MdecDiag_ExpandDecoderTable(s_vlc_decoder_table);
        s_vlc_table_initialized = 1;
    }

    uint8_t sector_raw[PSP_CDROM_SECTOR_RAW];
    static uint8_t frame_buffer[65536];
    static uint16_t rle_buffer[65536];
    size_t  frame_bytes = 0;
    uint32_t current_lba = start_lba;
    uint32_t end_lba = start_lba + sector_count;
    uint32_t frames_rendered = 0;

    fprintf(stderr, "[PSP_STR] Starting video playback from LBA %u (%u sectors, %dx%d, %s)\n",
            (unsigned)start_lba, (unsigned)sector_count, tex_width, tex_height,
            is_16bit ? "16-bit" : "24-bit");

    /* Set up 24-bit or 16-bit DISPENV */
    DISPENV env;
    SetDefDispEnv(&env, 0, 0, tex_width, tex_height);
    env.isrgb24 = is_16bit ? 0 : 1;
    PutDispEnv(&env);

    while (current_lba < end_lba) {
        if (!PspCdrom_ReadRawSector(current_lba++, sector_raw)) {
            break;
        }

        uint8_t submode = sector_raw[18];

        /* Check for XA Audio sector (submode bit 0x04) */
        if (submode & 0x04u) {
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
                uint32_t frame_size  = (uint32_t)(payload[12] | (payload[13] << 8) |
                                                  (payload[14] << 16) | (payload[15] << 24));

                if (chunk_idx == 0) {
                    frame_bytes = 0;
                    memset(frame_buffer, 0, sizeof(frame_buffer));
                }

                uint32_t chunk_off = (uint32_t)chunk_idx * 2016u;
                if (chunk_off < frame_size) {
                    uint32_t take = frame_size - chunk_off;
                    if (take > 2016u) take = 2016u;
                    if (chunk_off + take <= sizeof(frame_buffer)) {
                        memcpy(frame_buffer + chunk_off, payload + 32, take);
                        if (chunk_off + take > frame_bytes) {
                            frame_bytes = chunk_off + take;
                        }
                    }
                }

                /* Complete frame assembled */
                if (chunk_idx + 1u == num_chunks && frame_bytes > 0) {
                    /* 1. Unpack Huffman VLC stream into RLE buffer */
                    memset(rle_buffer, 0, sizeof(rle_buffer));
                    DecDCTvlcSize2(0);
                    DecDCTvlc2((void *)frame_buffer, (void *)rle_buffer, (void *)s_vlc_decoder_table);

                    /* 2. Decode RLE macroblocks directly into shadow VRAM */
                    PspStr_DecodeFrame((const uint8_t *)rle_buffer, sizeof(rle_buffer),
                                       dst_texture ? dst_texture : g_ps1_vram,
                                       tex_width, tex_height, is_16bit);

                    /* 3. Render authentic 4:3 362x272 centered viewport and present */
                    PspGpu_DrawOTag(NULL, 0);
                    VSync(0);
                    frames_rendered++;

                    if (frames_rendered == 1) {
                        fprintf(stderr, "[PSP_STR] Rendered first authentic STR frame in 4:3 (362x272, %s)\n",
                                is_16bit ? "16-bit" : "24-bit");
                    }

                    frame_bytes = 0;
                }
            }
        }
    }

    /* Restore authentic 16-bit DISPENV after video streaming */
    DISPENV env_end;
    SetDefDispEnv(&env_end, 0, 0, tex_width, tex_height);
    env_end.isrgb24 = 0;
    PutDispEnv(&env_end);

    fprintf(stderr, "[PSP_STR] Video stream finished: %u frames rendered\n", (unsigned)frames_rendered);
    return 1;
}
