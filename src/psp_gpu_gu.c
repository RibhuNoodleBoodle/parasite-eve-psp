/* ==============================================================================
 * psp_gpu_gu.c - Native Sony PlayStation Portable (PSP) libgu/libgum Backend
 * for Parasite Eve (PS1) Decompilation Port.
 *
 * Implements hardware-accelerated PlayStation 1 GPU packet and primitive
 * decoding directly into the PSP Graphics Engine (GE) display list.
 *
 * Architecture:
 * - Direct execution via <pspgu.h>, <pspgum.h>, <pspdisplay.h>
 * - Complete switch cases for PS1 primitive opcodes 0x20 through 0x7F
 * - Software shadow of PS1 1024x512 VRAM with dirty cache writeback invalidation
 * - PS1 Semi-transparency ABR 0..3 mapped to sceGuBlendFunc
 * - Dynamic aspect scaling (1:1 Center, 4:3 Pillarbox, 16:9 Full)
 * ==============================================================================
 */

#include "psp_gpu_gu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include <pspdebug.h>

/* ------------------------------------------------------------------------------
 * Static Buffers & Memory Layout
 * ------------------------------------------------------------------------------
 */

/* 256 KB Static display list aligned to 64 bytes */
static unsigned int __attribute__((aligned(64))) g_display_list[PSP_GU_DISPLAY_LIST_SIZE / sizeof(unsigned int)];

/* 1024x512 16-bit PS1 VRAM shadow buffer (1 MB) in main RAM */
static uint16_t __attribute__((aligned(64))) g_ps1_vram[PS1_VRAM_PIXELS];

/* Temporary vertex storage for batch drawing */
#define MAX_TEMP_VERTS 256
static PspVertexTex2D   __attribute__((aligned(16))) g_temp_tex_verts[MAX_TEMP_VERTS];
static PspVertexColor2D __attribute__((aligned(16))) g_temp_col_verts[MAX_TEMP_VERTS];

/* Active GPU State */
PspGpuState g_psp_gpu;

/* EDRAM buffer offsets for 16-bit GU_PSM_5551 framebuffer */
#define EDRAM_DRAW_OFFSET   ((void*)0x00000000)
#define EDRAM_DISP_OFFSET   ((void*)0x00044000)
#define EDRAM_DEPTH_OFFSET  ((void*)0x00088000)

/* ------------------------------------------------------------------------------
 * Helper Inline Functions
 * ------------------------------------------------------------------------------
 */

/* Convert PS1 8-bit RGB to PSP ABGR8888 */
static inline uint32_t PsxColorToPsp(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

/* Transform PS1 native 320x240 coordinates to PSP 480x272 screen space */
static inline void TransformCoords(int16_t x, int16_t y, int16_t *out_x, int16_t *out_y) {
    int32_t world_x = (int32_t)x + (int32_t)g_psp_gpu.offset_x;
    int32_t world_y = (int32_t)y + (int32_t)g_psp_gpu.offset_y;

    switch (g_psp_gpu.scaling_mode) {
    case PSP_SCALE_ORIGINAL_CENTER:
        /* 1:1 Pixel perfect centered: (480-320)/2 = 80, (272-240)/2 = 16 */
        *out_x = (int16_t)(world_x + 80);
        *out_y = (int16_t)(world_y + 16);
        break;

    case PSP_SCALE_4_3_PILLARBOX:
        /* 4:3 Aspect ratio scaled: height = 272, width = 362, offset_x = (480-362)/2 = 59 */
        *out_x = (int16_t)(((world_x * 362) / 320) + 59);
        *out_y = (int16_t)((world_y * 272) / 240);
        break;

    case PSP_SCALE_16_9_FULL:
    default:
        /* Full widescreen stretch: 320 -> 480, 240 -> 272 */
        *out_x = (int16_t)((world_x * 480) / 320);
        *out_y = (int16_t)((world_y * 272) / 240);
        break;
    }
}

/* Apply PS1 Semi-Transparency Mode (ABR 0..3) */
static void ApplySemiTransparency(int enabled, int abr) {
    if (!enabled) {
        if (g_psp_gpu.semi_trans_enabled) {
            sceGuDisable(GU_BLEND);
            g_psp_gpu.semi_trans_enabled = 0;
        }
        return;
    }

    if (!g_psp_gpu.semi_trans_enabled || g_psp_gpu.semi_trans_mode != abr) {
        sceGuEnable(GU_BLEND);
        switch (abr & 3) {
        case 0:
            /* ABR 0: 0.5 * Back + 0.5 * Front */
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x00808080, 0x00808080);
            break;
        case 1:
            /* ABR 1: 1.0 * Back + 1.0 * Front (Additive) */
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x00FFFFFF, 0x00FFFFFF);
            break;
        case 2:
            /* ABR 2: 1.0 * Back - 1.0 * Front (Subtractive) */
            sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0x00FFFFFF, 0x00FFFFFF);
            break;
        case 3:
            /* ABR 3: 1.0 * Back + 0.25 * Front */
            sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x00404040, 0x00FFFFFF);
            break;
        }
        g_psp_gpu.semi_trans_enabled = 1;
        g_psp_gpu.semi_trans_mode = abr & 3;
    }
}

/* Setup Texture State from PS1 TPAGE and CLUT */
static void ApplyTexture(uint16_t tpage, uint16_t clut, int raw_texture) {
    uint32_t depth   = (tpage >> 7) & 0x03;
    uint32_t clut_x  = (clut & 0x3F) * 16;
    uint32_t clut_y  = (clut >> 6) & 0x1FF;

    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0, 0x01);

    /* Texture Filtering */
    if (g_psp_gpu.filter_mode == PSP_FILTER_BILINEAR) {
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    } else {
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    }

    /* Modulation vs Raw Texture */
    if (raw_texture) {
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    } else {
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    }

    switch (depth) {
    case 0: {
        /* 4-bit Indexed CLUT (GU_PSM_T4) */
        uint16_t *clut_ptr = &g_ps1_vram[clut_y * PS1_VRAM_WIDTH + clut_x];
        for (int i = 0; i < 16; i++) {
            if (clut_ptr[i] != 0 && !(clut_ptr[i] & 0x8000)) {
                clut_ptr[i] |= 0x8000;
            }
        }
        sceKernelDcacheWritebackRange(clut_ptr, 16 * sizeof(uint16_t));
        sceGuClutMode(GU_PSM_5551, 0, 0xFF, 0);
        sceGuClutLoad(2, clut_ptr);
        sceGuTexMode(GU_PSM_T4, 0, 0, 0);
        sceGuTexImage(0, 1024, 512, 1024, g_ps1_vram);
        break;
    }

    case 1: {
        /* 8-bit Indexed CLUT (GU_PSM_T8) */
        uint16_t *clut_ptr = &g_ps1_vram[clut_y * PS1_VRAM_WIDTH + clut_x];
        for (int i = 0; i < 256; i++) {
            if (clut_ptr[i] != 0 && !(clut_ptr[i] & 0x8000)) {
                clut_ptr[i] |= 0x8000;
            }
        }
        sceKernelDcacheWritebackRange(clut_ptr, 256 * sizeof(uint16_t));
        sceGuClutMode(GU_PSM_5551, 0, 0xFF, 0);
        sceGuClutLoad(32, clut_ptr);
        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
        sceGuTexImage(0, 1024, 512, 1024, g_ps1_vram);
        break;
    }

    case 2:
    default:
        /* 15-bit Direct Color (GU_PSM_5551) */
        sceGuTexMode(GU_PSM_5551, 0, 0, 0);
        sceGuTexImage(0, 1024, 512, 1024, g_ps1_vram);
        break;
    }

    g_psp_gpu.current_tpage = tpage;
    g_psp_gpu.current_clut  = clut;
}

/* ------------------------------------------------------------------------------
 * Subsystem Lifecycle Implementation
 * ------------------------------------------------------------------------------
 */

void PspGpu_Init(void) {
    if (g_psp_gpu.initialized) return;

    memset(&g_psp_gpu, 0, sizeof(g_psp_gpu));
    memset(g_ps1_vram, 0, sizeof(g_ps1_vram));

    g_psp_gpu.draw_buffer  = EDRAM_DRAW_OFFSET;
    g_psp_gpu.disp_buffer  = EDRAM_DISP_OFFSET;
    g_psp_gpu.depth_buffer = EDRAM_DEPTH_OFFSET;

    g_psp_gpu.scaling_mode = PSP_SCALE_4_3_PILLARBOX;
    g_psp_gpu.filter_mode  = PSP_FILTER_BILINEAR;
    g_psp_gpu.clip_x2      = PS1_NATIVE_WIDTH;
    g_psp_gpu.clip_y2      = PS1_NATIVE_HEIGHT;

    /* Initialize PSP Graphics Engine (libgu) */
    sceGuInit();

    sceKernelDcacheWritebackInvalidateAll();
    sceGuStart(GU_DIRECT, g_display_list);

    /* Setup 16-bit double buffered 512-stride framebuffers */
    sceGuDrawBuffer(GU_PSM_5551, g_psp_gpu.draw_buffer, PSP_BUFFER_STRIDE);
    sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT, g_psp_gpu.disp_buffer, PSP_BUFFER_STRIDE);
    sceGuDepthBuffer(g_psp_gpu.depth_buffer, PSP_BUFFER_STRIDE);

    /* Coordinate system offset and viewport */
    sceGuOffset(2048 - (PSP_SCREEN_WIDTH / 2), 2048 - (PSP_SCREEN_HEIGHT / 2));
    sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
    sceGuDepthRange(65535, 0);

    /* Scissor test covering full 480x272 display */
    sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);

    /* Render state defaults */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_DITHER);

    sceKernelDcacheWritebackInvalidateAll();
    sceGuFinish();
    sceGuSync(0, 0);

    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    g_psp_gpu.initialized = 1;
}

void PspGpu_Shutdown(void) {
    if (!g_psp_gpu.initialized) return;

    sceGuDisplay(GU_FALSE);
    sceGuTerm();
    g_psp_gpu.initialized = 0;
}

void PspGpu_Reset(void) {
    g_psp_gpu.offset_x = 0;
    g_psp_gpu.offset_y = 0;
    g_psp_gpu.clip_x1  = 0;
    g_psp_gpu.clip_y1  = 0;
    g_psp_gpu.clip_x2  = PS1_NATIVE_WIDTH;
    g_psp_gpu.clip_y2  = PS1_NATIVE_HEIGHT;
    g_psp_gpu.semi_trans_enabled = 0;
}

void PspGpu_SetScalingMode(PspScalingMode mode) {
    g_psp_gpu.scaling_mode = mode;
}

void PspGpu_SetFilterMode(PspFilterMode filter) {
    g_psp_gpu.filter_mode = filter;
}

void PspGpu_SetDispMask(int mask) {
    g_psp_gpu.display_mask = mask;
    if (mask) {
        sceGuDisplay(GU_TRUE);
    } else {
        sceGuDisplay(GU_FALSE);
    }
}

uint16_t *PspGpu_GetVramPointer(void) {
    return g_ps1_vram;
}

void PspGpu_InvalidateVramRange(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)x; (void)y; (void)w; (void)h;
    /* Flush entire VRAM buffer from CPU D-Cache to physical RAM for GPU reading */
    sceKernelDcacheWritebackInvalidateRange(g_ps1_vram, sizeof(g_ps1_vram));
}

void PspGpu_Sync(void) {
    sceGuSync(0, 0);
}

/* ------------------------------------------------------------------------------
 * Frame Boundary Management
 * ------------------------------------------------------------------------------
 */

void PspGpu_BeginFrame(void) {
    if (g_psp_gpu.in_frame) return;

    /* Flush D-Cache to physical RAM before starting display list */
    sceKernelDcacheWritebackInvalidateAll();

    sceGuStart(GU_DIRECT, g_display_list);

    /* Scissor to active display bounds */
    sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);

    /* Clear framebuffer to black */
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);

    g_psp_gpu.in_frame = 1;
}

void PspGpu_EndFrame(void) {
    if (!g_psp_gpu.in_frame) return;

    sceKernelDcacheWritebackInvalidateAll();
    sceGuFinish();
    sceGuSync(0, 0);

    g_psp_gpu.in_frame = 0;
}

void PspGpu_Flip(void) {
    PspGpu_EndFrame();

    sceDisplayWaitVblankStart();
    /* Swap display and draw buffer pointers via sceGuSwapBuffers */
    g_psp_gpu.disp_buffer = g_psp_gpu.draw_buffer;
    g_psp_gpu.draw_buffer = sceGuSwapBuffers();
    g_psp_gpu.frame_counter++;
}

/* ------------------------------------------------------------------------------
 * PS1 VRAM Image Transfers
 * ------------------------------------------------------------------------------
 */

int PspGpu_LoadImage(const RECT *rect, const void *pixels) {
    if (!rect || !pixels) return 0;
    if (rect->w <= 0 || rect->h <= 0) return 0;

    const uint16_t *src = (const uint16_t *)pixels;
    int x = rect->x & 0x3FF;
    int y = rect->y & 0x1FF;
    int w = rect->w;
    int h = rect->h;

    for (int row = 0; row < h; row++) {
        int dst_y = (y + row) & 0x1FF;
        for (int col = 0; col < w; col++) {
            int dst_x = (x + col) & 0x3FF;
            uint16_t pix = src[row * w + col];
            /* PS1 15-bit color format: 0x0000 is color-key transparent.
             * Non-zero pixels need bit 15 set to 1 for PSP GU_PSM_5551
             * alpha (opaque) so they are preserved under alpha testing. */
            if (pix != 0) {
                pix |= 0x8000;
            }
            g_ps1_vram[dst_y * PS1_VRAM_WIDTH + dst_x] = pix;
        }
    }

    /* Writeback D-Cache so GPU texture sampler sees the new pixel data */
    sceKernelDcacheWritebackInvalidateRange(g_ps1_vram, sizeof(g_ps1_vram));
    g_psp_gpu.textures_uploaded++;
    return 1;
}

int PspGpu_StoreImage(const RECT *rect, void *pixels) {
    if (!rect || !pixels) return 0;
    if (rect->w <= 0 || rect->h <= 0) return 0;

    uint16_t *dst = (uint16_t *)pixels;
    int x = rect->x & 0x3FF;
    int y = rect->y & 0x1FF;
    int w = rect->w;
    int h = rect->h;

    for (int row = 0; row < h; row++) {
        int src_y = (y + row) & 0x1FF;
        for (int col = 0; col < w; col++) {
            int src_x = (x + col) & 0x3FF;
            dst[row * w + col] = g_ps1_vram[src_y * PS1_VRAM_WIDTH + src_x];
        }
    }
    return 1;
}

int PspGpu_MoveImage(const RECT *src, int dst_x, int dst_y) {
    if (!src || src->w <= 0 || src->h <= 0) return 0;

    int sx = src->x & 0x3FF;
    int sy = src->y & 0x1FF;
    int dx = dst_x & 0x3FF;
    int dy = dst_y & 0x1FF;
    int w  = src->w;
    int h  = src->h;

    for (int row = 0; row < h; row++) {
        int cur_sy = (sy + row) & 0x1FF;
        int cur_dy = (dy + row) & 0x1FF;
        for (int col = 0; col < w; col++) {
            int cur_sx = (sx + col) & 0x3FF;
            int cur_dx = (dx + col) & 0x3FF;
            g_ps1_vram[cur_dy * PS1_VRAM_WIDTH + cur_dx] = g_ps1_vram[cur_sy * PS1_VRAM_WIDTH + cur_sx];
        }
    }

    sceKernelDcacheWritebackInvalidateRange(g_ps1_vram, sizeof(g_ps1_vram));
    return 1;
}

void PspGpu_ClearImage(const RECT *rect, uint8_t r, uint8_t g, uint8_t b) {
    if (!rect || rect->w <= 0 || rect->h <= 0) return;

    /* 15-bit color: 1 bit alpha, 5 bits B, 5 bits G, 5 bits R */
    uint16_t pixel = (uint16_t)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
    if (pixel != 0 || (r != 0 || g != 0 || b != 0)) {
        pixel |= 0x8000;
    }
    int x = rect->x & 0x3FF;
    int y = rect->y & 0x1FF;
    int w = rect->w;
    int h = rect->h;

    for (int row = 0; row < h; row++) {
        int cur_y = (y + row) & 0x1FF;
        for (int col = 0; col < w; col++) {
            int cur_x = (x + col) & 0x3FF;
            g_ps1_vram[cur_y * PS1_VRAM_WIDTH + cur_x] = pixel;
        }
    }

    sceKernelDcacheWritebackInvalidateRange(g_ps1_vram, sizeof(g_ps1_vram));
}

/* ------------------------------------------------------------------------------
 * Psy-Q Environment Compatibility Shims
 * ------------------------------------------------------------------------------
 */

DRAWENV *PspGpu_PutDrawEnv(const DRAWENV *env) {
    if (!env) return NULL;

    g_psp_gpu.offset_x = env->ofs[0];
    g_psp_gpu.offset_y = env->ofs[1];
    g_psp_gpu.clip_x1  = env->clip.x;
    g_psp_gpu.clip_y1  = env->clip.y;
    g_psp_gpu.clip_x2  = env->clip.x + env->clip.w;
    g_psp_gpu.clip_y2  = env->clip.y + env->clip.h;

    g_psp_gpu.current_tpage = env->tpage;

    /* Background clear requested */
    if (env->isbg && g_psp_gpu.in_frame) {
        RECT r = { env->clip.x, env->clip.y, env->clip.w, env->clip.h };
        PspGpu_ClearImage(&r, env->r0, env->g0, env->b0);
    }
    return (DRAWENV *)env;
}

DISPENV *PspGpu_PutDispEnv(const DISPENV *env) {
    return (DISPENV *)env;
}

DRAWENV *PspGpu_SetDefDrawEnv(DRAWENV *env, int x, int y, int w, int h) {
    if (!env) return NULL;
    memset(env, 0, sizeof(DRAWENV));
    env->clip.x = (int16_t)x;
    env->clip.y = (int16_t)y;
    env->clip.w = (int16_t)w;
    env->clip.h = (int16_t)h;
    env->ofs[0] = (int16_t)x;
    env->ofs[1] = (int16_t)y;
    return env;
}

DISPENV *PspGpu_SetDefDispEnv(DISPENV *env, int x, int y, int w, int h) {
    if (!env) return NULL;
    memset(env, 0, sizeof(DISPENV));
    env->disp.x = (int16_t)x;
    env->disp.y = (int16_t)y;
    env->disp.w = (int16_t)w;
    env->disp.h = (int16_t)h;
    return env;
}

/* ------------------------------------------------------------------------------
 * Primitive Decoding & Hardware Execution: Complete Opcode Matrix (0x20 - 0x7F)
 * ------------------------------------------------------------------------------
 */

void PspGpu_ExecuteSinglePrim(const uint32_t *packet) {
    if (!packet) return;

    /* In Psy-Q packets: word 0 is [length(8) | tag(24)], word 1 is [code(8) | R,G,B(24)] */
    uint32_t word0 = packet[0];
    uint32_t word1 = packet[1];
    uint8_t opcode = (uint8_t)(word1 >> 24);

    /* If opcode is 0, check if word 0 is a direct GP0 command */
    if (opcode == 0) {
        opcode = (uint8_t)(word0 >> 24);
    }

    /* Common primitive flags */
    int is_semi_trans = (opcode & 0x02) != 0;
    int is_raw_tex    = (opcode & 0x01) != 0;
    int abr_mode      = (g_psp_gpu.current_tpage >> 5) & 0x03;

    switch (opcode) {

    /* --- Special Commands: 0x00..0x02, 0xE1..0xE6 --- */
    case PS1_CMD_NOP:
    case PS1_CMD_CLEAR_CACHE:
        break;

    case PS1_CMD_FILL_RECT: {
        /* Quick Fill: [cmd|RGB], [Y|X], [H|W] */
        uint8_t r = (uint8_t)(word0);
        uint8_t g = (uint8_t)(word0 >> 8);
        uint8_t b = (uint8_t)(word0 >> 16);
        int16_t x = (int16_t)(packet[1] & 0xFFFF);
        int16_t y = (int16_t)(packet[1] >> 16);
        int16_t w = (int16_t)(packet[2] & 0xFFFF);
        int16_t h = (int16_t)(packet[2] >> 16);
        RECT rc = { x, y, w, h };
        PspGpu_ClearImage(&rc, r, g, b);
        break;
    }

    case PS1_CMD_DRAW_MODE:
        /* GP0(E1h): Draw Mode / TPAGE setting */
        g_psp_gpu.current_tpage = (uint16_t)(word0 & 0xFFFF);
        break;

    case PS1_CMD_TEX_WINDOW:
        /* GP0(E2h): Texture Window Setting */
        g_psp_gpu.twin_mask_x   = (uint8_t)(word0 & 0x1F);
        g_psp_gpu.twin_mask_y   = (uint8_t)((word0 >> 5) & 0x1F);
        g_psp_gpu.twin_offset_x = (uint8_t)((word0 >> 10) & 0x1F);
        g_psp_gpu.twin_offset_y = (uint8_t)((word0 >> 15) & 0x1F);
        break;

    case PS1_CMD_DRAW_AREA_TL:
        /* GP0(E3h): Drawing Area Top-Left */
        g_psp_gpu.clip_x1 = (int16_t)(word0 & 0x3FF);
        g_psp_gpu.clip_y1 = (int16_t)((word0 >> 10) & 0x1FF);
        break;

    case PS1_CMD_DRAW_AREA_BR:
        /* GP0(E4h): Drawing Area Bottom-Right */
        g_psp_gpu.clip_x2 = (int16_t)(word0 & 0x3FF);
        g_psp_gpu.clip_y2 = (int16_t)((word0 >> 10) & 0x1FF);
        break;

    case PS1_CMD_DRAW_OFFSET:
        /* GP0(E5h): Drawing Offset */
        g_psp_gpu.offset_x = (int16_t)(word0 & 0x7FF);
        if (g_psp_gpu.offset_x & 0x400) g_psp_gpu.offset_x |= 0xF800; /* Sign-extend 11-bit */
        g_psp_gpu.offset_y = (int16_t)((word0 >> 11) & 0x7FF);
        if (g_psp_gpu.offset_y & 0x400) g_psp_gpu.offset_y |= 0xF800;
        break;

    case PS1_CMD_MASK_SETTING:
        /* GP0(E6h): Mask bit settings */
        break;

    /* --- POLY_F3: Flat Triangle (0x20..0x23) --- */
    case 0x20: case 0x21: case 0x22: case 0x23: {
        uint8_t r = (uint8_t)(word1);
        uint8_t g = (uint8_t)(word1 >> 8);
        uint8_t b = (uint8_t)(word1 >> 16);
        uint32_t col = PsxColorToPsp(r, g, b, 0xFF);

        int16_t x0, y0, x1, y1, x2, y2;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[3] & 0xFFFF), (int16_t)(packet[3] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[4] & 0xFFFF), (int16_t)(packet[4] >> 16), &x2, &y2);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        PspVertexColor2D *v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0;
        v[2].color = col; v[2].x = x2; v[2].y = y2; v[2].z = 0;

        sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 3, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_FT3: Textured Flat Triangle (0x24..0x27) --- */
    case 0x24: case 0x25: case 0x26: case 0x27: {
        uint8_t r = (uint8_t)(word1);
        uint8_t g = (uint8_t)(word1 >> 8);
        uint8_t b = (uint8_t)(word1 >> 16);
        uint32_t col = PsxColorToPsp(r, g, b, 0xFF);

        uint16_t clut  = (uint16_t)(packet[3] >> 16);
        uint16_t tpage = (uint16_t)(packet[5] >> 16);

        uint32_t tpage_x = (tpage & 0x0F) * 64;
        uint32_t tpage_y = ((tpage >> 4) & 0x01) * 256;

        uint16_t u0 = (uint16_t)((packet[3] & 0xFF) + tpage_x);
        uint16_t v0 = (uint16_t)(((packet[3] >> 8) & 0xFF) + tpage_y);

        uint16_t u1 = (uint16_t)((packet[5] & 0xFF) + tpage_x);
        uint16_t v1 = (uint16_t)(((packet[5] >> 8) & 0xFF) + tpage_y);

        uint16_t u2 = (uint16_t)((packet[7] & 0xFF) + tpage_x);
        uint16_t v2 = (uint16_t)(((packet[7] >> 8) & 0xFF) + tpage_y);

        int16_t x0, y0, x1, y1, x2, y2;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[4] & 0xFFFF), (int16_t)(packet[4] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[6] & 0xFFFF), (int16_t)(packet[6] >> 16), &x2, &y2);

        ApplySemiTransparency(is_semi_trans, (tpage >> 5) & 0x03);
        ApplyTexture(tpage, clut, is_raw_tex);

        PspVertexTex2D *v = g_temp_tex_verts;
        v[0].u = u0; v[0].v = v0; v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].u = u1; v[1].v = v1; v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0;
        v[2].u = u2; v[2].v = v2; v[2].color = col; v[2].x = x2; v[2].y = y2; v[2].z = 0;

        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 3, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_F4: Flat Quad (0x28..0x2B) --- */
    case 0x28: case 0x29: case 0x2A: case 0x2B: {
        uint8_t r = (uint8_t)(word1);
        uint8_t g = (uint8_t)(word1 >> 8);
        uint8_t b = (uint8_t)(word1 >> 16);
        uint32_t col = PsxColorToPsp(r, g, b, 0xFF);

        int16_t x0, y0, x1, y1, x2, y2, x3, y3;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[3] & 0xFFFF), (int16_t)(packet[3] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[4] & 0xFFFF), (int16_t)(packet[4] >> 16), &x2, &y2);
        TransformCoords((int16_t)(packet[5] & 0xFFFF), (int16_t)(packet[5] >> 16), &x3, &y3);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        PspVertexColor2D *v = g_temp_col_verts;
        /* Quad as 4-vertex triangle strip (0, 1, 2, 3) */
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0;
        v[2].color = col; v[2].x = x2; v[2].y = y2; v[2].z = 0;
        v[3].color = col; v[3].x = x3; v[3].y = y3; v[3].z = 0;

        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_FT4: Textured Flat Quad (0x2C..0x2F) --- */
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
        uint8_t r = (uint8_t)(word1);
        uint8_t g = (uint8_t)(word1 >> 8);
        uint8_t b = (uint8_t)(word1 >> 16);
        uint32_t col = PsxColorToPsp(r, g, b, 0xFF);

        uint16_t clut  = (uint16_t)(packet[3] >> 16);
        uint16_t tpage = (uint16_t)(packet[5] >> 16);

        uint32_t tpage_x = (tpage & 0x0F) * 64;
        uint32_t tpage_y = ((tpage >> 4) & 0x01) * 256;

        uint16_t u0 = (uint16_t)((packet[3] & 0xFF) + tpage_x);
        uint16_t v0 = (uint16_t)(((packet[3] >> 8) & 0xFF) + tpage_y);

        uint16_t u1 = (uint16_t)((packet[5] & 0xFF) + tpage_x);
        uint16_t v1 = (uint16_t)(((packet[5] >> 8) & 0xFF) + tpage_y);

        uint16_t u2 = (uint16_t)((packet[7] & 0xFF) + tpage_x);
        uint16_t v2 = (uint16_t)(((packet[7] >> 8) & 0xFF) + tpage_y);

        uint16_t u3 = (uint16_t)((packet[9] & 0xFF) + tpage_x);
        uint16_t v3 = (uint16_t)(((packet[9] >> 8) & 0xFF) + tpage_y);

        int16_t x0, y0, x1, y1, x2, y2, x3, y3;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[4] & 0xFFFF), (int16_t)(packet[4] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[6] & 0xFFFF), (int16_t)(packet[6] >> 16), &x2, &y2);
        TransformCoords((int16_t)(packet[8] & 0xFFFF), (int16_t)(packet[8] >> 16), &x3, &y3);

        ApplySemiTransparency(is_semi_trans, (tpage >> 5) & 0x03);
        ApplyTexture(tpage, clut, is_raw_tex);

        PspVertexTex2D *v = g_temp_tex_verts;
        v[0].u = u0; v[0].v = v0; v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].u = u1; v[1].v = v1; v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0;
        v[2].u = u2; v[2].v = v2; v[2].color = col; v[2].x = x2; v[2].y = y2; v[2].z = 0;
        v[3].u = u3; v[3].v = v3; v[3].color = col; v[3].x = x3; v[3].y = y3; v[3].z = 0;

        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_G3: Gouraud Shaded Triangle (0x30..0x33) --- */
    case 0x30: case 0x31: case 0x32: case 0x33: {
        uint32_t c0 = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        uint32_t c1 = PsxColorToPsp((uint8_t)packet[3], (uint8_t)(packet[3] >> 8), (uint8_t)(packet[3] >> 16), 0xFF);
        uint32_t c2 = PsxColorToPsp((uint8_t)packet[5], (uint8_t)(packet[5] >> 8), (uint8_t)(packet[5] >> 16), 0xFF);

        int16_t x0, y0, x1, y1, x2, y2;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[4] & 0xFFFF), (int16_t)(packet[4] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[6] & 0xFFFF), (int16_t)(packet[6] >> 16), &x2, &y2);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        PspVertexColor2D *v = g_temp_col_verts;
        v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0;
        v[2].color = c2; v[2].x = x2; v[2].y = y2; v[2].z = 0;

        sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 3, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_GT3: Textured Gouraud Triangle (0x34..0x37) --- */
    case 0x34: case 0x35: case 0x36: case 0x37: {
        uint32_t c0 = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        uint32_t c1 = PsxColorToPsp((uint8_t)packet[4], (uint8_t)(packet[4] >> 8), (uint8_t)(packet[4] >> 16), 0xFF);
        uint32_t c2 = PsxColorToPsp((uint8_t)packet[7], (uint8_t)(packet[7] >> 8), (uint8_t)(packet[7] >> 16), 0xFF);

        uint16_t clut  = (uint16_t)(packet[3] >> 16);
        uint16_t tpage = (uint16_t)(packet[6] >> 16);

        uint32_t tpage_x = (tpage & 0x0F) * 64;
        uint32_t tpage_y = ((tpage >> 4) & 0x01) * 256;

        uint16_t u0 = (uint16_t)((packet[3] & 0xFF) + tpage_x);
        uint16_t v0 = (uint16_t)(((packet[3] >> 8) & 0xFF) + tpage_y);

        uint16_t u1 = (uint16_t)((packet[6] & 0xFF) + tpage_x);
        uint16_t v1 = (uint16_t)(((packet[6] >> 8) & 0xFF) + tpage_y);

        uint16_t u2 = (uint16_t)((packet[8] & 0xFF) + tpage_x);
        uint16_t v2 = (uint16_t)(((packet[8] >> 8) & 0xFF) + tpage_y);

        int16_t x0, y0, x1, y1, x2, y2;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[5] & 0xFFFF), (int16_t)(packet[5] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[8] & 0xFFFF), (int16_t)(packet[8] >> 16), &x2, &y2);

        ApplySemiTransparency(is_semi_trans, (tpage >> 5) & 0x03);
        ApplyTexture(tpage, clut, is_raw_tex);

        PspVertexTex2D *v = g_temp_tex_verts;
        v[0].u = u0; v[0].v = v0; v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].u = u1; v[1].v = v1; v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0;
        v[2].u = u2; v[2].v = v2; v[2].color = c2; v[2].x = x2; v[2].y = y2; v[2].z = 0;

        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 3, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_G4: Gouraud Shaded Quad (0x38..0x3B) --- */
    case 0x38: case 0x39: case 0x3A: case 0x3B: {
        uint32_t c0 = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        uint32_t c1 = PsxColorToPsp((uint8_t)packet[3], (uint8_t)(packet[3] >> 8), (uint8_t)(packet[3] >> 16), 0xFF);
        uint32_t c2 = PsxColorToPsp((uint8_t)packet[5], (uint8_t)(packet[5] >> 8), (uint8_t)(packet[5] >> 16), 0xFF);
        uint32_t c3 = PsxColorToPsp((uint8_t)packet[7], (uint8_t)(packet[7] >> 8), (uint8_t)(packet[7] >> 16), 0xFF);

        int16_t x0, y0, x1, y1, x2, y2, x3, y3;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[4] & 0xFFFF), (int16_t)(packet[4] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[6] & 0xFFFF), (int16_t)(packet[6] >> 16), &x2, &y2);
        TransformCoords((int16_t)(packet[8] & 0xFFFF), (int16_t)(packet[8] >> 16), &x3, &y3);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        PspVertexColor2D *v = g_temp_col_verts;
        v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0;
        v[2].color = c2; v[2].x = x2; v[2].y = y2; v[2].z = 0;
        v[3].color = c3; v[3].x = x3; v[3].y = y3; v[3].z = 0;

        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_GT4: Textured Gouraud Quad (0x3C..0x3F) --- */
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: {
        uint32_t c0 = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        uint32_t c1 = PsxColorToPsp((uint8_t)packet[4], (uint8_t)(packet[4] >> 8), (uint8_t)(packet[4] >> 16), 0xFF);
        uint32_t c2 = PsxColorToPsp((uint8_t)packet[7], (uint8_t)(packet[7] >> 8), (uint8_t)(packet[7] >> 16), 0xFF);
        uint32_t c3 = PsxColorToPsp((uint8_t)packet[10], (uint8_t)(packet[10] >> 8), (uint8_t)(packet[10] >> 16), 0xFF);

        uint16_t clut  = (uint16_t)(packet[3] >> 16);
        uint16_t tpage = (uint16_t)(packet[6] >> 16);

        uint32_t tpage_x = (tpage & 0x0F) * 64;
        uint32_t tpage_y = ((tpage >> 4) & 0x01) * 256;

        uint16_t u0 = (uint16_t)((packet[3] & 0xFF) + tpage_x);
        uint16_t v0 = (uint16_t)(((packet[3] >> 8) & 0xFF) + tpage_y);

        uint16_t u1 = (uint16_t)((packet[6] & 0xFF) + tpage_x);
        uint16_t v1 = (uint16_t)(((packet[6] >> 8) & 0xFF) + tpage_y);

        uint16_t u2 = (uint16_t)((packet[9] & 0xFF) + tpage_x);
        uint16_t v2 = (uint16_t)(((packet[9] >> 8) & 0xFF) + tpage_y);

        uint16_t u3 = (uint16_t)((packet[11] & 0xFF) + tpage_x);
        uint16_t v3 = (uint16_t)(((packet[11] >> 8) & 0xFF) + tpage_y);

        int16_t x0, y0, x1, y1, x2, y2, x3, y3;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[5] & 0xFFFF), (int16_t)(packet[5] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[8] & 0xFFFF), (int16_t)(packet[8] >> 16), &x2, &y2);
        TransformCoords((int16_t)(packet[11] & 0xFFFF), (int16_t)(packet[11] >> 16), &x3, &y3);

        ApplySemiTransparency(is_semi_trans, (tpage >> 5) & 0x03);
        ApplyTexture(tpage, clut, is_raw_tex);

        PspVertexTex2D *v = g_temp_tex_verts;
        v[0].u = u0; v[0].v = v0; v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].u = u1; v[1].v = v1; v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0;
        v[2].u = u2; v[2].v = v2; v[2].color = c2; v[2].x = x2; v[2].y = y2; v[2].z = 0;
        v[3].u = u3; v[3].v = v3; v[3].color = c3; v[3].x = x3; v[3].y = y3; v[3].z = 0;

        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- LINE_F2: Flat Line (0x40..0x43) --- */
    case 0x40: case 0x41: case 0x42: case 0x43: {
        uint32_t col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        int16_t x0, y0, x1, y1;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[3] & 0xFFFF), (int16_t)(packet[3] >> 16), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        PspVertexColor2D *v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0;

        sceGuDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- LINE_F3 / Polyline Flat (0x48..0x4B) --- */
    case 0x48: case 0x49: case 0x4A: case 0x4B: {
        uint32_t col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        /* Variable length packet terminated by 0x55555555 */
        uint32_t len = (packet[0] >> 24) & 0xFF;
        if (len < 3) len = 3;

        int vert_count = 0;
        PspVertexColor2D *v = g_temp_col_verts;

        for (uint32_t i = 2; i <= len && vert_count < MAX_TEMP_VERTS; i++) {
            if (packet[i] == 0x55555555 || packet[i] == 0x50005000) break;
            int16_t x, y;
            TransformCoords((int16_t)(packet[i] & 0xFFFF), (int16_t)(packet[i] >> 16), &x, &y);
            v[vert_count].color = col;
            v[vert_count].x = x;
            v[vert_count].y = y;
            v[vert_count].z = 0;
            vert_count++;
        }

        if (vert_count >= 2) {
            sceGuDrawArray(GU_LINE_STRIP, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, vert_count, NULL, v);
            g_psp_gpu.primitives_drawn++;
        }
        break;
    }

    /* --- LINE_G2: Gouraud Line (0x50..0x53) --- */
    case 0x50: case 0x51: case 0x52: case 0x53: {
        uint32_t c0 = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        uint32_t c1 = PsxColorToPsp((uint8_t)packet[3], (uint8_t)(packet[3] >> 8), (uint8_t)(packet[3] >> 16), 0xFF);

        int16_t x0, y0, x1, y1;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[4] & 0xFFFF), (int16_t)(packet[4] >> 16), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        PspVertexColor2D *v = g_temp_col_verts;
        v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0;

        sceGuDrawArray(GU_LINES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- LINE_G3 / Polyline Gouraud (0x58..0x5B) --- */
    case 0x58: case 0x59: case 0x5A: case 0x5B: {
        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        uint32_t len = (packet[0] >> 24) & 0xFF;
        if (len < 4) len = 4;

        int vert_count = 0;
        PspVertexColor2D *v = g_temp_col_verts;

        uint32_t cur_col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        int16_t x0, y0;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        v[vert_count].color = cur_col; v[vert_count].x = x0; v[vert_count].y = y0; v[vert_count].z = 0;
        vert_count++;

        for (uint32_t i = 3; i + 1 <= len && vert_count < MAX_TEMP_VERTS; i += 2) {
            if (packet[i] == 0x55555555 || packet[i] == 0x50005000) break;
            cur_col = PsxColorToPsp((uint8_t)packet[i], (uint8_t)(packet[i] >> 8), (uint8_t)(packet[i] >> 16), 0xFF);
            int16_t x, y;
            TransformCoords((int16_t)(packet[i+1] & 0xFFFF), (int16_t)(packet[i+1] >> 16), &x, &y);
            v[vert_count].color = cur_col; v[vert_count].x = x; v[vert_count].y = y; v[vert_count].z = 0;
            vert_count++;
        }

        if (vert_count >= 2) {
            sceGuDrawArray(GU_LINE_STRIP, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, vert_count, NULL, v);
            g_psp_gpu.primitives_drawn++;
        }
        break;
    }

    /* --- TILE: Variable-Size Rectangle (0x60..0x63) --- */
    case 0x60: case 0x61: case 0x62: case 0x63: {
        uint32_t col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        int16_t x = (int16_t)(packet[2] & 0xFFFF);
        int16_t y = (int16_t)(packet[2] >> 16);
        int16_t w = (int16_t)(packet[3] & 0xFFFF);
        int16_t h = (int16_t)(packet[3] >> 16);

        int16_t x0, y0, x1, y1;
        TransformCoords(x, y, &x0, &y0);
        TransformCoords((int16_t)(x + w), (int16_t)(y + h), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        PspVertexColor2D *v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0;

        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- SPRT: Variable-Size Textured Rectangle (0x64..0x67) --- */
    case 0x64: case 0x65: case 0x66: case 0x67: {
        uint32_t col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        int16_t x = (int16_t)(packet[2] & 0xFFFF);
        int16_t y = (int16_t)(packet[2] >> 16);

        uint16_t u = (uint16_t)(packet[3] & 0xFF);
        uint16_t v = (uint16_t)((packet[3] >> 8) & 0xFF);
        uint16_t clut = (uint16_t)(packet[3] >> 16);

        int16_t w = (int16_t)(packet[4] & 0xFFFF);
        int16_t h = (int16_t)(packet[4] >> 16);

        uint32_t tpage_x = (g_psp_gpu.current_tpage & 0x0F) * 64;
        uint32_t tpage_y = ((g_psp_gpu.current_tpage >> 4) & 0x01) * 256;

        int16_t x0, y0, x1, y1;
        TransformCoords(x, y, &x0, &y0);
        TransformCoords((int16_t)(x + w), (int16_t)(y + h), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        ApplyTexture(g_psp_gpu.current_tpage, clut, is_raw_tex);

        PspVertexTex2D *vert = g_temp_tex_verts;
        vert[0].u = (uint16_t)(u + tpage_x);     vert[0].v = (uint16_t)(v + tpage_y);
        vert[0].color = col; vert[0].x = x0; vert[0].y = y0; vert[0].z = 0;

        vert[1].u = (uint16_t)(u + w + tpage_x); vert[1].v = (uint16_t)(v + h + tpage_y);
        vert[1].color = col; vert[1].x = x1; vert[1].y = y1; vert[1].z = 0;

        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, vert);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- TILE_1: 1x1 Dot (0x68..0x6B) --- */
    case 0x68: case 0x69: case 0x6A: case 0x6B: {
        uint32_t col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        int16_t x = (int16_t)(packet[2] & 0xFFFF);
        int16_t y = (int16_t)(packet[2] >> 16);

        int16_t x0, y0, x1, y1;
        TransformCoords(x, y, &x0, &y0);
        TransformCoords((int16_t)(x + 1), (int16_t)(y + 1), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        PspVertexColor2D *v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0;

        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- TILE_8: 8x8 Tile (0x70..0x73) --- */
    case 0x70: case 0x71: case 0x72: case 0x73: {
        uint32_t col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        int16_t x = (int16_t)(packet[2] & 0xFFFF);
        int16_t y = (int16_t)(packet[2] >> 16);

        int16_t x0, y0, x1, y1;
        TransformCoords(x, y, &x0, &y0);
        TransformCoords((int16_t)(x + 8), (int16_t)(y + 8), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        PspVertexColor2D *v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0;

        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- SPRT_8: 8x8 Textured Sprite (0x74..0x77) --- */
    case 0x74: case 0x75: case 0x76: case 0x77: {
        uint32_t col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        int16_t x = (int16_t)(packet[2] & 0xFFFF);
        int16_t y = (int16_t)(packet[2] >> 16);

        uint16_t u = (uint16_t)(packet[3] & 0xFF);
        uint16_t v = (uint16_t)((packet[3] >> 8) & 0xFF);
        uint16_t clut = (uint16_t)(packet[3] >> 16);

        uint32_t tpage_x = (g_psp_gpu.current_tpage & 0x0F) * 64;
        uint32_t tpage_y = ((g_psp_gpu.current_tpage >> 4) & 0x01) * 256;

        int16_t x0, y0, x1, y1;
        TransformCoords(x, y, &x0, &y0);
        TransformCoords((int16_t)(x + 8), (int16_t)(y + 8), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        ApplyTexture(g_psp_gpu.current_tpage, clut, is_raw_tex);

        PspVertexTex2D *vert = g_temp_tex_verts;
        vert[0].u = (uint16_t)(u + tpage_x);     vert[0].v = (uint16_t)(v + tpage_y);
        vert[0].color = col; vert[0].x = x0; vert[0].y = y0; vert[0].z = 0;

        vert[1].u = (uint16_t)(u + 8 + tpage_x); vert[1].v = (uint16_t)(v + 8 + tpage_y);
        vert[1].color = col; vert[1].x = x1; vert[1].y = y1; vert[1].z = 0;

        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, vert);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- TILE_16: 16x16 Tile (0x78..0x7B) --- */
    case 0x78: case 0x79: case 0x7A: case 0x7B: {
        uint32_t col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        int16_t x = (int16_t)(packet[2] & 0xFFFF);
        int16_t y = (int16_t)(packet[2] >> 16);

        int16_t x0, y0, x1, y1;
        TransformCoords(x, y, &x0, &y0);
        TransformCoords((int16_t)(x + 16), (int16_t)(y + 16), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);

        PspVertexColor2D *v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0;

        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- SPRT_16: 16x16 Textured Sprite (0x7C..0x7F) --- */
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        uint32_t col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        int16_t x = (int16_t)(packet[2] & 0xFFFF);
        int16_t y = (int16_t)(packet[2] >> 16);

        uint16_t u = (uint16_t)(packet[3] & 0xFF);
        uint16_t v = (uint16_t)((packet[3] >> 8) & 0xFF);
        uint16_t clut = (uint16_t)(packet[3] >> 16);

        uint32_t tpage_x = (g_psp_gpu.current_tpage & 0x0F) * 64;
        uint32_t tpage_y = ((g_psp_gpu.current_tpage >> 4) & 0x01) * 256;

        int16_t x0, y0, x1, y1;
        TransformCoords(x, y, &x0, &y0);
        TransformCoords((int16_t)(x + 16), (int16_t)(y + 16), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        ApplyTexture(g_psp_gpu.current_tpage, clut, is_raw_tex);

        PspVertexTex2D *vert = g_temp_tex_verts;
        vert[0].u = (uint16_t)(u + tpage_x);      vert[0].v = (uint16_t)(v + tpage_y);
        vert[0].color = col; vert[0].x = x0; vert[0].y = y0; vert[0].z = 0;

        vert[1].u = (uint16_t)(u + 16 + tpage_x); vert[1].v = (uint16_t)(v + 16 + tpage_y);
        vert[1].color = col; vert[1].x = x1; vert[1].y = y1; vert[1].z = 0;

        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, vert);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    default:
        /* Unhandled / Unknown GPU opcode */
        break;
    }
}

/* ------------------------------------------------------------------------------
 * Ordering Table Execution
 * ------------------------------------------------------------------------------
 */

void PspGpu_ExecutePacket(const uint32_t *packet) {
    /* Walk linked list of packets */
    const uint32_t *cur = packet;
    while (cur != NULL) {
        PspGpu_ExecuteSinglePrim(cur);

        uint32_t tag = cur[0];
        uint32_t next_addr = tag & 0x00FFFFFF;

        /* Terminator on PS1 GPU DMA is 0x00FFFFFF */
        if (next_addr == 0x00FFFFFF || next_addr == 0) {
            break;
        }

        /* Next packet pointer in RAM */
        cur = (const uint32_t *)((uintptr_t)next_addr);
    }
}

void PspGpu_DrawOTag(const uint32_t *ot, int count) {
    if (!ot || count <= 0) return;

    PspGpu_BeginFrame();

    /* Back-to-front rendering: traverse Ordering Table entries from count-1 down to 0 */
    for (int i = count - 1; i >= 0; i--) {
        uint32_t head = ot[i];
        if (head == 0x00FFFFFF || head == 0) continue;

        const uint32_t *packet = (const uint32_t *)((uintptr_t)(head & 0x00FFFFFF));
        PspGpu_ExecutePacket(packet);
    }

    PspGpu_EndFrame();
}

void PspGpu_DrawOTagEnv(const uint32_t *ot, int count, const DRAWENV *env) {
    if (env) {
        PspGpu_PutDrawEnv(env);
    }
    PspGpu_DrawOTag(ot, count);
}

/* ------------------------------------------------------------------------------
 * PS1 Psy-Q Standard Function Exports
 * ------------------------------------------------------------------------------
 */

int DrawSync(int mode) {
    (void)mode;
    if (g_psp_gpu.in_frame) {
        sceKernelDcacheWritebackInvalidateAll();
        sceGuFinish();
        sceGuSync(0, 0);
        g_psp_gpu.in_frame = 0;
    }
    return 0;
}

int VSync(int mode) {
    (void)mode;
    if (g_psp_gpu.in_frame) {
        sceKernelDcacheWritebackInvalidateAll();
        sceGuFinish();
        sceGuSync(0, 0);
        g_psp_gpu.in_frame = 0;
    }
    sceDisplayWaitVblankStart();
    g_psp_gpu.disp_buffer = g_psp_gpu.draw_buffer;
    g_psp_gpu.draw_buffer = sceGuSwapBuffers();
    g_psp_gpu.frame_counter++;
    return 0;
}

int ResetGraph(int mode) {
    (void)mode;
    PspGpu_Reset();
    return 0;
}

int SetDispMask(int mask) {
    PspGpu_SetDispMask(mask);
    return 0;
}

int LoadImage(const RECT *rect, const void *pixels) {
    return PspGpu_LoadImage(rect, pixels);
}

int StoreImage(const RECT *rect, void *pixels) {
    return PspGpu_StoreImage(rect, pixels);
}

int MoveImage(const RECT *src, int dst_x, int dst_y) {
    return PspGpu_MoveImage(src, dst_x, dst_y);
}

void ClearImage(const RECT *rect, uint8_t r, uint8_t g, uint8_t b) {
    PspGpu_ClearImage(rect, r, g, b);
}

DRAWENV *PutDrawEnv(const DRAWENV *env) {
    return PspGpu_PutDrawEnv(env);
}

DISPENV *PutDispEnv(const DISPENV *env) {
    return PspGpu_PutDispEnv(env);
}

void DrawOTag(const uint32_t *ot) {
    PspGpu_DrawOTag(ot, 4096);
}

void DrawPrim(const void *p) {
    if (p) {
        PspGpu_ExecuteSinglePrim((const uint32_t *)p);
    }
}
