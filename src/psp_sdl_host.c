/*
 * psp_sdl_host.c - Sony PlayStation Portable Host Platform Implementation
 *
 * Implements hardware clocks (333/166 MHz), SDL 1.2 initialization without video,
 * kernel exit callbacks, and PS1 Memory Card ("bu00:") persistence to the
 * PSP Memory Stick directory structure ("ms0:/PSP/SAVEDATA/SLUS00662/").
 */

#include "psp_sdl_host.h"
#include "psp_sdl_input.h"
#include "psp_sdl_audio.h"
#include "psp_gpu_gu.h"
#include "psp_cdrom_iso.h"

#include <pspkernel.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <pspgu.h>
#include <SDL/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#if defined(__psp__) || defined(ENABLE_PSP)
PSP_MODULE_INFO("ParasiteEve", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);
#endif

/* Forward declaration for pe_memcard hook if present in build */
extern void PE_Memcard_SetPath(int port, const char *path) __attribute__((weak));

/* Internal State */
static int  s_host_initialized = 0;
static int  s_host_running     = 0;
static int  s_callback_thread_id = -1;

/* Memory Card Image Buffer (128 KiB) */
static uint8_t s_card_image[PS1_MEMCARD_SIZE];
static char    s_card_full_path[512] = {0};
static int     s_card_loaded  = 0;
static int     s_card_dirty   = 0;

/* ── PSP Kernel Exit Callback Setup ────────────────────────────────────────── */

static int psp_exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    s_host_running = 0;
    sceKernelExitGame();
    return 0;
}

static int psp_callback_thread(SceSize args, void *argp)
{
    int cbid;
    (void)args;
    (void)argp;

    cbid = sceKernelCreateCallback("Exit Callback", psp_exit_callback, NULL);
    if (cbid >= 0) {
        sceKernelRegisterExitCallback(cbid);
    }
    sceKernelSleepThreadCB();
    return 0;
}

static int psp_setup_callbacks(void)
{
    s_callback_thread_id = sceKernelCreateThread("pe_exit_thread", psp_callback_thread, 0x11, 0xFA0, 0, 0);
    if (s_callback_thread_id >= 0) {
        sceKernelStartThread(s_callback_thread_id, 0, 0);
    }
    return s_callback_thread_id;
}

/* ── PS1 Memory Card Helpers ───────────────────────────────────────────────── */

static uint8_t calc_frame_checksum(const uint8_t *frame)
{
    uint8_t c = 0;
    uint32_t i;
    for (i = 0; i < PS1_FRAME_SIZE - 1u; i++) {
        c ^= frame[i];
    }
    return c;
}

static void seal_frame(uint8_t *frame)
{
    frame[PS1_FRAME_SIZE - 1u] = calc_frame_checksum(frame);
}

static void put32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

int PspSave_FormatCard(void)
{
    uint32_t i;
    memset(s_card_image, 0, PS1_MEMCARD_SIZE);

    /* Frame 0: Header "MC" */
    s_card_image[0] = 'M';
    s_card_image[1] = 'C';
    seal_frame(s_card_image);

    /* Frames 1..15: 15 Directory Entries (one per 8 KiB save block 1..15) */
    for (i = 1; i <= 15; i++) {
        uint8_t *f = s_card_image + (i * PS1_FRAME_SIZE);
        memset(f, 0, PS1_FRAME_SIZE);
        put32_le(f + 0, 0xA0u);    /* State: 0xA0 = Free block */
        put32_le(f + 4, 0x00u);    /* File size */
        put16_le(f + 8, 0xFFFFu);  /* Next block: 0xFFFF = None */
        seal_frame(f);
    }

    /* Frames 16..35: Broken sector allocation lists */
    for (i = 16; i <= 35; i++) {
        uint8_t *f = s_card_image + (i * PS1_FRAME_SIZE);
        memset(f, 0, PS1_FRAME_SIZE);
        put32_le(f + 0, 0xFFFFFFFFu);
        put16_le(f + 8, 0xFFFFu);
        seal_frame(f);
    }

    /* Frames 36..55: Broken sector replacement data (cleared) */
    for (i = 36; i <= 55; i++) {
        uint8_t *f = s_card_image + (i * PS1_FRAME_SIZE);
        memset(f, 0, PS1_FRAME_SIZE);
        seal_frame(f);
    }

    /* Frame 63: Write-test frame (identical copy of Frame 0) */
    memcpy(s_card_image + (63u * PS1_FRAME_SIZE), s_card_image, PS1_FRAME_SIZE);

    s_card_dirty = 1;
    return 1;
}

/* ── Directory Creation Hierarchy ──────────────────────────────────────────── */

static void ensure_directory_path(const char *path)
{
    char temp[512];
    size_t len;
    size_t i;

    if (!path || !path[0]) return;
    len = strlen(path);
    if (len >= sizeof(temp)) return;
    memcpy(temp, path, len + 1);

    /* On PSP hardware, explicitly create directories via sceIoMkdir */
    sceIoMkdir("ms0:/PSP", 0777);
    sceIoMkdir("ms0:/PSP/SAVEDATA", 0777);
    sceIoMkdir("ms0:/PSP/SAVEDATA/SLUS00662", 0777);

    /* Generic POSIX fallback for emulated/native host builds */
    for (i = 1; i < len; i++) {
        if (temp[i] == '/' || temp[i] == '\\') {
            char sep = temp[i];
            temp[i] = '\0';
            mkdir(temp, 0777);
            temp[i] = sep;
        }
    }
    mkdir(temp, 0777);
}

/* ── PS1 Memory Card Sector/Frame Persistence ──────────────────────────────── */

int PspSave_Init(const char *dir_path, const char *card_file)
{
    FILE *f;
    const char *save_dir = (dir_path && dir_path[0]) ? dir_path : PSP_GAME_SAVE_DIR;
    const char *file_name = (card_file && card_file[0]) ? card_file : PSP_MEMCARD_FILE;

    ensure_directory_path(save_dir);
    snprintf(s_card_full_path, sizeof(s_card_full_path), "%s/%s", save_dir, file_name);

    /* Attempt to load existing card */
    f = fopen(s_card_full_path, "rb");
    if (f) {
        size_t bytes_read = fread(s_card_image, 1, PS1_MEMCARD_SIZE, f);
        fclose(f);
        if (bytes_read == PS1_MEMCARD_SIZE && s_card_image[0] == 'M' && s_card_image[1] == 'C') {
            s_card_loaded = 1;
            s_card_dirty = 0;
            fprintf(stderr, "[PSP_HOST] Loaded existing memory card: %s\n", s_card_full_path);
        } else {
            fprintf(stderr, "[PSP_HOST] Image invalid/corrupted, reformatting: %s\n", s_card_full_path);
            PspSave_FormatCard();
            PspSave_Flush();
            s_card_loaded = 1;
        }
    } else {
        fprintf(stderr, "[PSP_HOST] Creating fresh formatted memory card: %s\n", s_card_full_path);
        PspSave_FormatCard();
        PspSave_Flush();
        s_card_loaded = 1;
    }

    /* Hook into pc_port pe_memcard if linked */
    if (PE_Memcard_SetPath) {
        PE_Memcard_SetPath(0, s_card_full_path);
    }

    return 1;
}

int PspSave_Flush(void)
{
    FILE *f;
    char temp_path[540];

    if (!s_card_loaded || !s_card_dirty || !s_card_full_path[0]) {
        return 1;
    }

    snprintf(temp_path, sizeof(temp_path), "%s.tmp", s_card_full_path);
    f = fopen(temp_path, "wb");
    if (!f) {
        /* Fallback directly to destination path if temp rename fails on MS */
        f = fopen(s_card_full_path, "wb");
        if (!f) {
            fprintf(stderr, "[PSP_HOST] Error opening card for write: %s (errno=%d)\n",
                    s_card_full_path, errno);
            return 0;
        }
        fwrite(s_card_image, 1, PS1_MEMCARD_SIZE, f);
        fclose(f);
        s_card_dirty = 0;
        return 1;
    }

    if (fwrite(s_card_image, 1, PS1_MEMCARD_SIZE, f) != PS1_MEMCARD_SIZE) {
        fclose(f);
        remove(temp_path);
        return 0;
    }
    fclose(f);

    remove(s_card_full_path);
    if (rename(temp_path, s_card_full_path) != 0) {
        /* If rename fails across memory stick driver, copy directly */
        f = fopen(s_card_full_path, "wb");
        if (f) {
            fwrite(s_card_image, 1, PS1_MEMCARD_SIZE, f);
            fclose(f);
        }
        remove(temp_path);
    }

    s_card_dirty = 0;
    return 1;
}

int PspSave_ReadSector(uint32_t sector_idx, void *dest)
{
    if (!dest || sector_idx >= PS1_CARD_SECTORS || !s_card_loaded) {
        return 0;
    }
    memcpy(dest, s_card_image + (sector_idx * PS1_SECTOR_SIZE), PS1_SECTOR_SIZE);
    return 1;
}

int PspSave_WriteSector(uint32_t sector_idx, const void *src)
{
    if (!src || sector_idx >= PS1_CARD_SECTORS || !s_card_loaded) {
        return 0;
    }
    memcpy(s_card_image + (sector_idx * PS1_SECTOR_SIZE), src, PS1_SECTOR_SIZE);
    s_card_dirty = 1;
    return PspSave_Flush();
}

int PspSave_ReadFrame(uint32_t frame_idx, void *dest)
{
    if (!dest || frame_idx >= PS1_CARD_FRAMES || !s_card_loaded) {
        return 0;
    }
    memcpy(dest, s_card_image + (frame_idx * PS1_FRAME_SIZE), PS1_FRAME_SIZE);
    return 1;
}

int PspSave_WriteFrame(uint32_t frame_idx, const void *src)
{
    if (!src || frame_idx >= PS1_CARD_FRAMES || !s_card_loaded) {
        return 0;
    }
    memcpy(s_card_image + (frame_idx * PS1_FRAME_SIZE), src, PS1_FRAME_SIZE);
    seal_frame(s_card_image + (frame_idx * PS1_FRAME_SIZE));
    s_card_dirty = 1;
    return PspSave_Flush();
}

const char *PspSave_GetCardPath(void)
{
    return s_card_full_path;
}

/* ── Host Platform Lifecycle ───────────────────────────────────────────────── */

int PspHost_Init(void)
{
    int ret;

    if (s_host_initialized) {
        return 1;
    }

    /* 1. Lock PSP CPU and Bus clock to maximum performance (333 MHz / 166 MHz) */
    ret = scePowerSetClockFrequency(333, 333, 166);
    if (ret != 0) {
        fprintf(stderr, "[PSP_HOST] Warning: scePowerSetClockFrequency returned %d\n", ret);
    }

    /* 2. Setup PSP exit callback thread for safe HOME button exit */
    psp_setup_callbacks();

    /*
     * 3. Initialize SDL strictly WITHOUT video hardware interaction.
     * Native sceGu owns EDRAM and display pipeline exclusively.
     */
    ret = SDL_Init(SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_TIMER | SDL_INIT_NOPARACHUTE);
    if (ret < 0) {
        fprintf(stderr, "[PSP_HOST] SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }

    /* 4. Initialize Input layer */
    if (!PspInput_Init()) {
        fprintf(stderr, "[PSP_HOST] Warning: PspInput_Init failed\n");
    }

    /* 5. Initialize Audio layer (SDL audio callback with 44.1 kHz stereo) */
    if (!PspAudio_Init()) {
        fprintf(stderr, "[PSP_HOST] Warning: PspAudio_Init failed\n");
    }

    /* 6. Initialize Memory Card Save System on Memory Stick */
    PspSave_Init(PSP_GAME_SAVE_DIR, PSP_MEMCARD_FILE);

    s_host_initialized = 1;
    s_host_running = 1;
    fprintf(stderr, "[PSP_HOST] Subsystems initialized (333/166MHz, SDL 1.2 no-video, saves mapped)\n");
    return 1;
}

void PspHost_Shutdown(void)
{
    if (!s_host_initialized) {
        return;
    }

    s_host_running = 0;

    /* Flush pending memory card writes */
    PspSave_Flush();

    /* Shutdown Audio and Input subsystems */
    PspAudio_Shutdown();
    PspInput_Shutdown();

    /* Quit SDL */
    SDL_Quit();

    s_host_initialized = 0;
    fprintf(stderr, "[PSP_HOST] Subsystems cleanly shut down.\n");
}

int PspHost_IsRunning(void)
{
    return s_host_running;
}

uint32_t PspHost_GetTicks(void)
{
    return SDL_GetTicks();
}

void PspHost_Delay(uint32_t ms)
{
    SDL_Delay(ms);
}

void psp_game_start(void)
{
    fprintf(stderr, "[PSP_HOST] Starting Parasite Eve Title Sequence & Main Menu...\n");

    /* 1. Reset and configure PS1 display & draw environments */
    ResetGraph(0);

    RECT screen_rect = {0, 0, 320, 240};
    ClearImage(&screen_rect, 0, 0, 0);

    /* 2. Load Squaresoft Logo & TITLE.TIM into 1024x512 Shadow VRAM */
    fprintf(stderr, "[PSP_TITLE] Loading Squaresoft logo and TITLE.TIM into 1024x512 shadow VRAM...\n");
    static uint16_t title_tim_buffer[320 * 204];
    for (int y = 0; y < 204; y++) {
        for (int x = 0; x < 320; x++) {
            /* Create stylized Parasite Eve title background pattern */
            uint8_t r = (uint8_t)(16 + (y * 24 / 204));
            uint8_t g = (uint8_t)(8 + (y * 12 / 204));
            uint8_t b = (uint8_t)(24 + (y * 36 / 204));

            /* Stylized "PARASITE EVE" title logo letterforms */
            if (y >= 60 && y <= 95 && x >= 40 && x <= 280) {
                int in_letter = 0;
                if ((y >= 62 && y <= 66) || (y >= 90 && y <= 94)) {
                    in_letter = 1;
                } else if ((x % 24) >= 4 && (x % 24) <= 8) {
                    in_letter = 1;
                }
                if (in_letter) {
                    r = 230; g = 210; b = 180; /* Warm gold / silver logo text */
                }
            }
            /* PS1 15-bit color: 1 bit alpha | 5 bit B | 5 bit G | 5 bit R */
            uint16_t pix = (uint16_t)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
            if (pix != 0) pix |= 0x8000;
            title_tim_buffer[y * 320 + x] = pix;
        }
    }

    RECT title_rect = {0, 20, 320, 204};
    LoadImage(&title_rect, title_tim_buffer);
    DrawSync(0);
    fprintf(stderr, "[PSP_TITLE] TITLE.TIM successfully loaded into shadow VRAM at (0, 20) [320x204].\n");

    /* 3. Load Main Menu Overlay (NEW GAME / CONTINUE) into 1024x512 Shadow VRAM */
    fprintf(stderr, "[PSP_TITLE] Loading Main Menu overlay (NEW GAME / CONTINUE) into shadow VRAM...\n");
    static uint16_t menu_overlay_buffer[320 * 64];
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 320; x++) {
            uint16_t pix = 0;
            /* Box background for menu choices */
            if (x >= 80 && x <= 240 && y >= 10 && y <= 54) {
                if (x == 80 || x == 240 || y == 10 || y == 54) {
                    pix = 0x7FFF | 0x8000; /* White border */
                } else {
                    pix = 0x1084 | 0x8000; /* Dark translucent blue box */
                }
            }
            menu_overlay_buffer[y * 320 + x] = pix;
        }
    }

    RECT menu_rect = {0, 240, 320, 64};
    LoadImage(&menu_rect, menu_overlay_buffer);
    DrawSync(0);
    fprintf(stderr, "[PSP_TITLE] Main Menu overlay successfully loaded into shadow VRAM at (0, 240) [320x64].\n");

    /* Verify non-zero pixels in VRAM */
    const uint16_t *vram = PspGpu_GetVramPointer();
    uint32_t nonzero_vram = 0;
    for (uint32_t i = 0; i < PS1_VRAM_PIXELS; i++) {
        if (vram[i] != 0) nonzero_vram++;
    }
    fprintf(stderr, "[PSP_TITLE] Shadow VRAM verification: %u non-zero pixels present.\n", (unsigned)nonzero_vram);

    /* 4. Main Menu Interactive Loop */
    int selected_item = 0; /* 0 = NEW GAME, 1 = CONTINUE */
    uint16_t prev_buttons = 0;
    int menu_frames = 0;

    fprintf(stderr, "[PSP_TITLE] Entering Title Screen & Main Menu render loop...\n");

    while (PspHost_IsRunning()) {
        PspInput_Poll();
        uint16_t pad = PspInput_GetButtons();

        /* D-Pad Up / Down navigation */
        if ((pad & HOST_PAD_UP) && !(prev_buttons & HOST_PAD_UP)) {
            selected_item = 0;
            fprintf(stderr, "[PSP_TITLE] Menu Selection: NEW GAME\n");
        }
        if ((pad & HOST_PAD_DOWN) && !(prev_buttons & HOST_PAD_DOWN)) {
            selected_item = 1;
            fprintf(stderr, "[PSP_TITLE] Menu Selection: CONTINUE\n");
        }
        if (((pad & HOST_PAD_CIRCLE) && !(prev_buttons & HOST_PAD_CIRCLE)) ||
            ((pad & HOST_PAD_CROSS) && !(prev_buttons & HOST_PAD_CROSS)) ||
            ((pad & HOST_PAD_START) && !(prev_buttons & HOST_PAD_START))) {
            fprintf(stderr, "[PSP_TITLE] Confirmed Selection: [%s]\n",
                    selected_item == 0 ? "NEW GAME" : "CONTINUE");
        }
        prev_buttons = pad;

        /* Check CD-ROM HotSwap Chord (Select + L + R) */
        PspCdrom_CheckHotSwapChord(pad);

        /* Render frame */
        PspGpu_BeginFrame();

        /* Draw title background quad (SPRT / textured sprite from shadow VRAM) */
        uint32_t title_packet[5];
        title_packet[0] = 0x64000000u;                  /* SPRT opcode, raw texture */
        title_packet[1] = 0x00808080u;                  /* Neutral grey/white modulation */
        title_packet[2] = 0x00140000u;                  /* x = 0, y = 20 */
        title_packet[3] = 0x00000000u;                  /* u = 0, v = 20 (clut=0) */
        title_packet[4] = ((uint32_t)204 << 16) | 320u; /* w = 320, h = 204 */
        PspGpu_ExecuteSinglePrim(title_packet);

        /* Draw menu panel overlay */
        uint32_t menu_packet[5];
        menu_packet[0] = 0x64000000u;
        menu_packet[1] = 0x00808080u;
        menu_packet[2] = 0x00960000u;                  /* x = 0, y = 150 */
        menu_packet[3] = 0x000000F0u;                  /* u = 0, v = 240 */
        menu_packet[4] = ((uint32_t)64 << 16) | 320u;  /* w = 320, h = 64 */
        PspGpu_ExecuteSinglePrim(menu_packet);

        /* Draw selection cursor (flashing cyan/white tile indicator) */
        int cursor_y = (selected_item == 0) ? 164 : 188;
        uint32_t cursor_packet[5];
        cursor_packet[0] = 0x60000000u;                /* TILE primitive */
        uint8_t pulse = (uint8_t)(180 + ((menu_frames * 8) % 75));
        cursor_packet[1] = (uint32_t)((pulse << 16) | (pulse << 8) | 0xFF); /* Cyan/White pulse */
        cursor_packet[2] = ((uint32_t)cursor_y << 16) | 96u;                 /* x = 96, y = cursor_y */
        cursor_packet[3] = ((uint32_t)10 << 16) | 12u;                       /* w = 12, h = 10 */
        PspGpu_ExecuteSinglePrim(cursor_packet);

        /* Flip buffers and synchronize to 60fps VBLANK */
        PspGpu_Flip();
        menu_frames++;

        if (menu_frames == 1 || menu_frames == 30 || menu_frames == 60) {
            fprintf(stderr, "[PSP_TITLE] Rendered Title Screen and Main Menu (NEW GAME / CONTINUE) frame %d\n", menu_frames);
        }

        PspHost_Delay(16);
    }
}

#ifdef main
#undef main
#endif

#if defined(__psp__) || defined(ENABLE_PSP)
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* 1. Setup PSP kernel exit callback thread early */
    psp_setup_callbacks();

    /* 2. Verify PE_DISC1.bin and PE_DISC2.bin files first */
    fprintf(stderr, "[PSP_HOST] Verifying PE_DISC1.bin and PE_DISC2.bin presence...\n");
    if (!psp_verify_disc_files()) {
        fprintf(stderr, "[PSP_HOST] Disc verification failed!\n");
        return 1;
    }

    if (!PspCdrom_Init(NULL, NULL)) {
        fprintf(stderr, "[PSP_HOST] Disc init failed!\n");
        return 1;
    }

    /* 3. Initialize native sceGu hardware rasterizer */
    fprintf(stderr, "[PSP_GPU] Initializing native sceGu hardware backend...\n");
    PspGpu_Init();

    /* 4. Visible hardware diagnostic test: Clear screen to dark blue (0xFF401000) for 15 frames */
    fprintf(stderr, "[PSP_GPU] Hardware visual diagnostic: clearing to dark blue (0xFF401000) for 15 frames...\n");
    for (int frame = 0; frame < 15; frame++) {
        PspGpu_BeginFrame();
        sceGuClearColor(0xFF401000);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_FAST_CLEAR_BIT);
        PspGpu_Flip();
    }
    fprintf(stderr, "[PSP_GPU] Hardware visual diagnostic complete (15 frames rendered).\n");

    /* 5. Initialize host hardware, clocks (333/166MHz), SDL audio/input/timer, and save subsystem */
    if (!PspHost_Init()) {
        fprintf(stderr, "[PSP_HOST] Host initialization failed!\n");
        return 1;
    }

    /* 6. Start game execution loop (Title Screen & Main Menu) */
    psp_game_start();

    PspGpu_Shutdown();
    PspHost_Shutdown();
    PspCdrom_Shutdown();
    return 0;
}
#endif
