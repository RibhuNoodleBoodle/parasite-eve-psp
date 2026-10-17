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
#include <pspiofilemgr.h>

/* ------------------------------------------------------------------------------
 * Static Buffers & Memory Layout
 * ------------------------------------------------------------------------------
 */

/* 256 KB Static display list aligned to 64 bytes */
static unsigned int __attribute__((aligned(64))) g_display_list[PSP_GU_DISPLAY_LIST_SIZE / sizeof(unsigned int)];

/* 1024x512 16-bit PS1 VRAM shadow buffer (1 MB) in main RAM */
uint16_t __attribute__((aligned(64))) g_ps1_vram[PS1_VRAM_PIXELS];

/* 512x512 16-bit decoded power-of-two texture page (512 KB) */
static uint16_t __attribute__((aligned(64))) s_decoded_tpage[512 * 512];
/* Dedicated 512x512 16-bit decoded DISPENV framebuffer texture (512 KB) */
static uint16_t __attribute__((aligned(64))) s_dispenv_tex[512 * 512];
/* Dedicated 512x512 32-bit decoded 24-bit RGB DISPENV framebuffer texture (1 MB) */
static uint32_t __attribute__((aligned(64))) s_dispenv_tex32[512 * 512];
static uint16_t s_cached_tpage = 0xFFFF;
static uint16_t s_cached_clut  = 0xFFFF;
static uint32_t s_cached_textures_uploaded = 0xFFFFFFFF;


/* Temporary vertex storage fallback for batch drawing */
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

/* Double PS1 modulation color (0x80 = 1.0x neutral) clamped to 255 (0xFF = 1.0x PSP GU) */
static inline uint8_t PsxScaleColor(uint8_t c) {
    uint32_t v = (uint32_t)c * 2;
    return (v > 255) ? 255 : (uint8_t)v;
}

static inline uint32_t PsxModulateColorToPsp(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) |
           ((uint32_t)PsxScaleColor(b) << 16) |
           ((uint32_t)PsxScaleColor(g) << 8) |
           (uint32_t)PsxScaleColor(r);
}

/* Transform PS1 native coordinates to PSP 480x272 screen space */
static inline void TransformCoords(int16_t x, int16_t y, int16_t *out_x, int16_t *out_y) {
    int32_t world_x = (int32_t)x + (int32_t)g_psp_gpu.offset_x - (int32_t)g_psp_gpu.clip_x1;
    int32_t world_y = (int32_t)y + (int32_t)g_psp_gpu.offset_y - (int32_t)g_psp_gpu.clip_y1;

    int32_t ps1_disp_w = (g_psp_gpu.has_dispenv && g_psp_gpu.current_dispenv.disp.w > 0) ?
                         (int32_t)g_psp_gpu.current_dispenv.disp.w : 320;
    int32_t ps1_disp_h = (g_psp_gpu.has_dispenv && g_psp_gpu.current_dispenv.disp.h > 0) ?
                         (int32_t)g_psp_gpu.current_dispenv.disp.h : 240;

    switch (g_psp_gpu.scaling_mode) {
    case PSP_SCALE_ORIGINAL_CENTER:
        /* 1:1 Pixel perfect centered: (480-320)/2 = 80, (272-240)/2 = 16 */
        *out_x = (int16_t)(world_x + 80);
        *out_y = (int16_t)(world_y + 16);
        break;

    case PSP_SCALE_16_9_FULL:
        /* Full widescreen stretch: ps1_disp_w -> 480, ps1_disp_h -> 272 */
        *out_x = (int16_t)((world_x * 480) / ps1_disp_w);
        *out_y = (int16_t)((world_y * 272) / ps1_disp_h);
        break;

    case PSP_SCALE_4_3_PILLARBOX:
    default:
        /* 4:3 Aspect ratio centered: VIEWPORT_X=59, VIEWPORT_Y=0, VIEWPORT_W=362, VIEWPORT_H=272 */
        *out_x = (int16_t)(59 + ((world_x * 362) / ps1_disp_w));
        *out_y = (int16_t)((world_y * 272) / ps1_disp_h);
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
    uint32_t tpage_x = (tpage & 0x0F) * 64;
    uint32_t tpage_y = ((tpage >> 4) & 0x01) * 256;

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

    /* Check if cached decoded texture buffer needs regeneration */
    if (s_cached_tpage != tpage || s_cached_clut != clut || s_cached_textures_uploaded != g_psp_gpu.textures_uploaded) {
        s_cached_tpage = tpage;
        s_cached_clut  = clut;
        s_cached_textures_uploaded = g_psp_gpu.textures_uploaded;

        switch (depth) {
        case 0: {
            /* 4-bit Indexed CLUT (16 colors) -> Decode to 512x512 16-bit GU_PSM_5551 */
            const uint16_t *clut_ptr = &g_ps1_vram[clut_y * PS1_VRAM_WIDTH + clut_x];
            uint16_t palette[16];
            for (int i = 0; i < 16; i++) {
                uint16_t c = clut_ptr[i];
                if (c != 0) c |= 0x8000;
                palette[i] = c;
            }

            for (int y = 0; y < 256; y++) {
                int vy = (tpage_y + y) & 0x1FF;
                const uint16_t *vram_row = &g_ps1_vram[vy * PS1_VRAM_WIDTH];
                uint16_t *dst_row = &s_decoded_tpage[y * 512];
                for (int x = 0; x < 256; x += 4) {
                    int vx = (tpage_x + (x / 4)) & 0x3FF;
                    uint16_t hw = vram_row[vx];
                    dst_row[x + 0] = palette[hw & 0x0F];
                    dst_row[x + 1] = palette[(hw >> 4) & 0x0F];
                    dst_row[x + 2] = palette[(hw >> 8) & 0x0F];
                    dst_row[x + 3] = palette[(hw >> 12) & 0x0F];
                }
            }
            break;
        }

        case 1: {
            /* 8-bit Indexed CLUT (256 colors) -> Decode to 512x512 16-bit GU_PSM_5551 */
            const uint16_t *clut_ptr = &g_ps1_vram[clut_y * PS1_VRAM_WIDTH + clut_x];
            uint16_t palette[256];
            for (int i = 0; i < 256; i++) {
                uint16_t c = clut_ptr[i];
                if (c != 0) c |= 0x8000;
                palette[i] = c;
            }

            for (int y = 0; y < 256; y++) {
                int vy = (tpage_y + y) & 0x1FF;
                const uint16_t *vram_row = &g_ps1_vram[vy * PS1_VRAM_WIDTH];
                uint16_t *dst_row = &s_decoded_tpage[y * 512];
                for (int x = 0; x < 256; x += 2) {
                    int vx = (tpage_x + (x / 2)) & 0x3FF;
                    uint16_t hw = vram_row[vx];
                    dst_row[x + 0] = palette[hw & 0xFF];
                    dst_row[x + 1] = palette[(hw >> 8) & 0xFF];
                }
            }
            break;
        }

        case 2:
        default: {
            /* 15-bit Direct Color (GU_PSM_5551) */
            for (int y = 0; y < 512; y++) {
                int vy = (tpage_y + y) & 0x1FF;
                const uint16_t *vram_row = &g_ps1_vram[vy * PS1_VRAM_WIDTH];
                uint16_t *dst_row = &s_decoded_tpage[y * 512];
                for (int x = 0; x < 512; x++) {
                    int vx = (tpage_x + x) & 0x3FF;
                    uint16_t c = vram_row[vx];
                    if (c != 0) c |= 0x8000;
                    dst_row[x] = c;
                }
            }
            break;
        }
        }

        sceKernelDcacheWritebackInvalidateRange(s_decoded_tpage, sizeof(s_decoded_tpage));
    }

    sceGuTexMode(GU_PSM_5551, 0, 0, 0);
    sceGuTexImage(0, 512, 512, 512, s_decoded_tpage);
    sceGuTexFlush();

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
    g_psp_gpu.has_dispenv  = 1;
    g_psp_gpu.current_dispenv.disp.w = 320;
    g_psp_gpu.current_dispenv.disp.h = 240;
    g_psp_gpu.current_dispenv.isrgb24 = 0;

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
    s_cached_tpage = 0xFFFF;
    s_cached_clut  = 0xFFFF;
    s_cached_textures_uploaded = 0xFFFFFFFF;
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

    /* Scissor covering full 480x272 panel to clear to black */
    sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);

    /* Clear framebuffer to black */
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);

    /* Set scissor strictly to 4:3 viewport: (59, 0, 362, 272) */
    if (g_psp_gpu.scaling_mode == PSP_SCALE_4_3_PILLARBOX) {
        sceGuScissor(59, 0, 362, 272);
    } else {
        sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
    }

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
            /* Store exact raw 16-bit halfwords in PS1 shadow VRAM so 4-bit/8-bit
             * CLUT indices and 24-bit RGB byte triplets are never corrupted. */
            g_ps1_vram[dst_y * PS1_VRAM_WIDTH + dst_x] = src[row * w + col];
        }
    }

    /* Writeback D-Cache so GPU texture sampler sees the new pixel data */
    sceKernelDcacheWritebackInvalidateRange(g_ps1_vram, sizeof(g_ps1_vram));
    g_psp_gpu.textures_uploaded++;
    g_psp_gpu.vram_dirty = 1;
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
    g_psp_gpu.textures_uploaded++;
    g_psp_gpu.vram_dirty = 1;
    return 1;
}

void PspGpu_ClearImage(const RECT *rect, uint8_t r, uint8_t g, uint8_t b) {
    if (!rect || rect->w <= 0 || rect->h <= 0) return;

    /* 15-bit color: 5 bits B, 5 bits G, 5 bits R (do not set bit 15 so ClearImage(0,0,1) stays 0x0000) */
    uint16_t pixel = (uint16_t)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
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
    g_psp_gpu.textures_uploaded++;
    g_psp_gpu.vram_dirty = 1;
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
    if (env) {
        g_psp_gpu.current_dispenv = *env;
        g_psp_gpu.has_dispenv = 1;
        g_psp_gpu.dispenv_dirty = 1;
    }
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

    uint32_t cmd_word = ((uint8_t)(word1 >> 24) == opcode) ? word1 : word0;

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
        uint8_t r = (uint8_t)(cmd_word);
        uint8_t g = (uint8_t)(cmd_word >> 8);
        uint8_t b = (uint8_t)(cmd_word >> 16);
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
        g_psp_gpu.current_tpage = (uint16_t)(cmd_word & 0xFFFF);
        break;

    case PS1_CMD_TEX_WINDOW:
        /* GP0(E2h): Texture Window Setting */
        g_psp_gpu.twin_mask_x   = (uint8_t)(cmd_word & 0x1F);
        g_psp_gpu.twin_mask_y   = (uint8_t)((cmd_word >> 5) & 0x1F);
        g_psp_gpu.twin_offset_x = (uint8_t)((cmd_word >> 10) & 0x1F);
        g_psp_gpu.twin_offset_y = (uint8_t)((cmd_word >> 15) & 0x1F);
        break;

    case PS1_CMD_DRAW_AREA_TL:
        /* GP0(E3h): Drawing Area Top-Left */
        g_psp_gpu.clip_x1 = (int16_t)(cmd_word & 0x3FF);
        g_psp_gpu.clip_y1 = (int16_t)((cmd_word >> 10) & 0x1FF);
        break;

    case PS1_CMD_DRAW_AREA_BR:
        /* GP0(E4h): Drawing Area Bottom-Right */
        g_psp_gpu.clip_x2 = (int16_t)(cmd_word & 0x3FF);
        g_psp_gpu.clip_y2 = (int16_t)((cmd_word >> 10) & 0x1FF);
        break;

    case PS1_CMD_DRAW_OFFSET:
        /* GP0(E5h): Drawing Offset */
        g_psp_gpu.offset_x = (int16_t)(cmd_word & 0x7FF);
        if (g_psp_gpu.offset_x & 0x400) g_psp_gpu.offset_x |= 0xF800; /* Sign-extend 11-bit */
        g_psp_gpu.offset_y = (int16_t)((cmd_word >> 11) & 0x7FF);
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

        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(3 * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;
        v[2].color = col; v[2].x = x2; v[2].y = y2; v[2].z = 0; v[2].pad = 0;

        sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 3, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_FT3: Textured Flat Triangle (0x24..0x27) --- */
    case 0x24: case 0x25: case 0x26: case 0x27: {
        uint8_t r = (uint8_t)(word1);
        uint8_t g = (uint8_t)(word1 >> 8);
        uint8_t b = (uint8_t)(word1 >> 16);
        uint32_t col = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp(r, g, b, 0xFF);

        uint16_t clut  = (uint16_t)(packet[3] >> 16);
        uint16_t tpage = (uint16_t)(packet[5] >> 16);

        uint16_t u0 = (uint16_t)(packet[3] & 0xFF);
        uint16_t v0 = (uint16_t)((packet[3] >> 8) & 0xFF);

        uint16_t u1 = (uint16_t)(packet[5] & 0xFF);
        uint16_t v1 = (uint16_t)((packet[5] >> 8) & 0xFF);

        uint16_t u2 = (uint16_t)(packet[7] & 0xFF);
        uint16_t v2 = (uint16_t)((packet[7] >> 8) & 0xFF);

        int16_t x0, y0, x1, y1, x2, y2;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[4] & 0xFFFF), (int16_t)(packet[4] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[6] & 0xFFFF), (int16_t)(packet[6] >> 16), &x2, &y2);

        ApplySemiTransparency(is_semi_trans, (tpage >> 5) & 0x03);
        ApplyTexture(tpage, clut, is_raw_tex);

        PspVertexTex2D *v = (PspVertexTex2D *)sceGuGetMemory(3 * sizeof(PspVertexTex2D));
        if (!v) v = g_temp_tex_verts;
        v[0].u = u0; v[0].v = v0; v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].u = u1; v[1].v = v1; v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;
        v[2].u = u2; v[2].v = v2; v[2].color = col; v[2].x = x2; v[2].y = y2; v[2].z = 0; v[2].pad = 0;

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

        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(4 * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;
        /* Quad as 4-vertex triangle strip (0, 1, 2, 3) */
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;
        v[2].color = col; v[2].x = x2; v[2].y = y2; v[2].z = 0; v[2].pad = 0;
        v[3].color = col; v[3].x = x3; v[3].y = y3; v[3].z = 0; v[3].pad = 0;

        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_FT4: Textured Flat Quad (0x2C..0x2F) --- */
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
        uint8_t r = (uint8_t)(word1);
        uint8_t g = (uint8_t)(word1 >> 8);
        uint8_t b = (uint8_t)(word1 >> 16);
        uint32_t col = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp(r, g, b, 0xFF);

        uint16_t clut  = (uint16_t)(packet[3] >> 16);
        uint16_t tpage = (uint16_t)(packet[5] >> 16);

        uint16_t u0 = (uint16_t)(packet[3] & 0xFF);
        uint16_t v0 = (uint16_t)((packet[3] >> 8) & 0xFF);

        uint16_t u1 = (uint16_t)(packet[5] & 0xFF);
        uint16_t v1 = (uint16_t)((packet[5] >> 8) & 0xFF);

        uint16_t u2 = (uint16_t)(packet[7] & 0xFF);
        uint16_t v2 = (uint16_t)((packet[7] >> 8) & 0xFF);

        uint16_t u3 = (uint16_t)(packet[9] & 0xFF);
        uint16_t v3 = (uint16_t)((packet[9] >> 8) & 0xFF);

        int16_t x0, y0, x1, y1, x2, y2, x3, y3;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[4] & 0xFFFF), (int16_t)(packet[4] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[6] & 0xFFFF), (int16_t)(packet[6] >> 16), &x2, &y2);
        TransformCoords((int16_t)(packet[8] & 0xFFFF), (int16_t)(packet[8] >> 16), &x3, &y3);

        ApplySemiTransparency(is_semi_trans, (tpage >> 5) & 0x03);
        ApplyTexture(tpage, clut, is_raw_tex);

        PspVertexTex2D *v = (PspVertexTex2D *)sceGuGetMemory(4 * sizeof(PspVertexTex2D));
        if (!v) v = g_temp_tex_verts;
        v[0].u = u0; v[0].v = v0; v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].u = u1; v[1].v = v1; v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;
        v[2].u = u2; v[2].v = v2; v[2].color = col; v[2].x = x2; v[2].y = y2; v[2].z = 0; v[2].pad = 0;
        v[3].u = u3; v[3].v = v3; v[3].color = col; v[3].x = x3; v[3].y = y3; v[3].z = 0; v[3].pad = 0;

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

        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(3 * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;
        v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;
        v[2].color = c2; v[2].x = x2; v[2].y = y2; v[2].z = 0; v[2].pad = 0;

        sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 3, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_GT3: Textured Gouraud Triangle (0x34..0x37) --- */
    case 0x34: case 0x35: case 0x36: case 0x37: {
        uint32_t c0 = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        uint32_t c1 = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp((uint8_t)packet[4], (uint8_t)(packet[4] >> 8), (uint8_t)(packet[4] >> 16), 0xFF);
        uint32_t c2 = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp((uint8_t)packet[7], (uint8_t)(packet[7] >> 8), (uint8_t)(packet[7] >> 16), 0xFF);

        uint16_t clut  = (uint16_t)(packet[3] >> 16);
        uint16_t tpage = (uint16_t)(packet[6] >> 16);

        uint16_t u0 = (uint16_t)(packet[3] & 0xFF);
        uint16_t v0 = (uint16_t)((packet[3] >> 8) & 0xFF);

        uint16_t u1 = (uint16_t)(packet[6] & 0xFF);
        uint16_t v1 = (uint16_t)((packet[6] >> 8) & 0xFF);

        uint16_t u2 = (uint16_t)(packet[8] & 0xFF);
        uint16_t v2 = (uint16_t)((packet[8] >> 8) & 0xFF);

        int16_t x0, y0, x1, y1, x2, y2;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[5] & 0xFFFF), (int16_t)(packet[5] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[8] & 0xFFFF), (int16_t)(packet[8] >> 16), &x2, &y2);

        ApplySemiTransparency(is_semi_trans, (tpage >> 5) & 0x03);
        ApplyTexture(tpage, clut, is_raw_tex);

        PspVertexTex2D *v = (PspVertexTex2D *)sceGuGetMemory(3 * sizeof(PspVertexTex2D));
        if (!v) v = g_temp_tex_verts;
        v[0].u = u0; v[0].v = v0; v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].u = u1; v[1].v = v1; v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;
        v[2].u = u2; v[2].v = v2; v[2].color = c2; v[2].x = x2; v[2].y = y2; v[2].z = 0; v[2].pad = 0;

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

        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(4 * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;
        v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;
        v[2].color = c2; v[2].x = x2; v[2].y = y2; v[2].z = 0; v[2].pad = 0;
        v[3].color = c3; v[3].x = x3; v[3].y = y3; v[3].z = 0; v[3].pad = 0;

        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- POLY_GT4: Textured Gouraud Quad (0x3C..0x3F) --- */
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: {
        uint32_t c0 = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        uint32_t c1 = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp((uint8_t)packet[4], (uint8_t)(packet[4] >> 8), (uint8_t)(packet[4] >> 16), 0xFF);
        uint32_t c2 = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp((uint8_t)packet[7], (uint8_t)(packet[7] >> 8), (uint8_t)(packet[7] >> 16), 0xFF);
        uint32_t c3 = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp((uint8_t)packet[10], (uint8_t)(packet[10] >> 8), (uint8_t)(packet[10] >> 16), 0xFF);

        uint16_t clut  = (uint16_t)(packet[3] >> 16);
        uint16_t tpage = (uint16_t)(packet[6] >> 16);

        uint16_t u0 = (uint16_t)(packet[3] & 0xFF);
        uint16_t v0 = (uint16_t)((packet[3] >> 8) & 0xFF);

        uint16_t u1 = (uint16_t)(packet[6] & 0xFF);
        uint16_t v1 = (uint16_t)((packet[6] >> 8) & 0xFF);

        uint16_t u2 = (uint16_t)(packet[9] & 0xFF);
        uint16_t v2 = (uint16_t)((packet[9] >> 8) & 0xFF);

        uint16_t u3 = (uint16_t)(packet[11] & 0xFF);
        uint16_t v3 = (uint16_t)((packet[11] >> 8) & 0xFF);

        int16_t x0, y0, x1, y1, x2, y2, x3, y3;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        TransformCoords((int16_t)(packet[5] & 0xFFFF), (int16_t)(packet[5] >> 16), &x1, &y1);
        TransformCoords((int16_t)(packet[8] & 0xFFFF), (int16_t)(packet[8] >> 16), &x2, &y2);
        TransformCoords((int16_t)(packet[11] & 0xFFFF), (int16_t)(packet[11] >> 16), &x3, &y3);

        ApplySemiTransparency(is_semi_trans, (tpage >> 5) & 0x03);
        ApplyTexture(tpage, clut, is_raw_tex);

        PspVertexTex2D *v = (PspVertexTex2D *)sceGuGetMemory(4 * sizeof(PspVertexTex2D));
        if (!v) v = g_temp_tex_verts;
        v[0].u = u0; v[0].v = v0; v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].u = u1; v[1].v = v1; v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;
        v[2].u = u2; v[2].v = v2; v[2].color = c2; v[2].x = x2; v[2].y = y2; v[2].z = 0; v[2].pad = 0;
        v[3].u = u3; v[3].v = v3; v[3].color = c3; v[3].x = x3; v[3].y = y3; v[3].z = 0; v[3].pad = 0;

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

        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(2 * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;

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
        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(MAX_TEMP_VERTS * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;

        for (uint32_t i = 2; i <= len && vert_count < MAX_TEMP_VERTS; i++) {
            if (packet[i] == 0x55555555 || packet[i] == 0x50005000) break;
            int16_t x, y;
            TransformCoords((int16_t)(packet[i] & 0xFFFF), (int16_t)(packet[i] >> 16), &x, &y);
            v[vert_count].color = col;
            v[vert_count].x = x;
            v[vert_count].y = y;
            v[vert_count].z = 0;
            v[vert_count].pad = 0;
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

        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(2 * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;
        v[0].color = c0; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].color = c1; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;

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
        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(MAX_TEMP_VERTS * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;

        uint32_t cur_col = PsxColorToPsp((uint8_t)word1, (uint8_t)(word1 >> 8), (uint8_t)(word1 >> 16), 0xFF);
        int16_t x0, y0;
        TransformCoords((int16_t)(packet[2] & 0xFFFF), (int16_t)(packet[2] >> 16), &x0, &y0);
        v[vert_count].color = cur_col; v[vert_count].x = x0; v[vert_count].y = y0; v[vert_count].z = 0; v[vert_count].pad = 0;
        vert_count++;

        for (uint32_t i = 3; i + 1 <= len && vert_count < MAX_TEMP_VERTS; i += 2) {
            if (packet[i] == 0x55555555 || packet[i] == 0x50005000) break;
            cur_col = PsxColorToPsp((uint8_t)packet[i], (uint8_t)(packet[i] >> 8), (uint8_t)(packet[i] >> 16), 0xFF);
            int16_t x, y;
            TransformCoords((int16_t)(packet[i+1] & 0xFFFF), (int16_t)(packet[i+1] >> 16), &x, &y);
            v[vert_count].color = cur_col; v[vert_count].x = x; v[vert_count].y = y; v[vert_count].z = 0; v[vert_count].pad = 0;
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

        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(2 * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;

        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- SPRT: Variable-Size Textured Rectangle (0x64..0x67) --- */
    case 0x64: case 0x65: case 0x66: case 0x67: {
        uint8_t r = (uint8_t)(word1);
        uint8_t g = (uint8_t)(word1 >> 8);
        uint8_t b = (uint8_t)(word1 >> 16);
        uint32_t col = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp(r, g, b, 0xFF);
        int16_t x = (int16_t)(packet[2] & 0xFFFF);
        int16_t y = (int16_t)(packet[2] >> 16);

        uint16_t u = (uint16_t)(packet[3] & 0xFF);
        uint16_t v = (uint16_t)((packet[3] >> 8) & 0xFF);
        uint16_t clut = (uint16_t)(packet[3] >> 16);

        int16_t w = (int16_t)(packet[4] & 0xFFFF);
        int16_t h = (int16_t)(packet[4] >> 16);

        int16_t x0, y0, x1, y1;
        TransformCoords(x, y, &x0, &y0);
        TransformCoords((int16_t)(x + w), (int16_t)(y + h), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        ApplyTexture(g_psp_gpu.current_tpage, clut, is_raw_tex);

        /* Rasterize into shadow VRAM so authentic PS1 VRAM scanout sees all primitive pixels */
        uint32_t depth   = (g_psp_gpu.current_tpage >> 7) & 0x03;
        uint32_t clut_x  = (clut & 0x3F) * 16;
        uint32_t clut_y  = (clut >> 6) & 0x1FF;
        uint32_t tpage_x = (g_psp_gpu.current_tpage & 0x0F) * 64;
        uint32_t tpage_y = ((g_psp_gpu.current_tpage >> 4) & 0x01) * 256;

        int dst_base_x = (x + g_psp_gpu.offset_x) & 0x3FF;
        int dst_base_y = (y + g_psp_gpu.offset_y) & 0x1FF;

        uint16_t palette[256];
        int num_colors = (depth == 0) ? 16 : 256;
        const uint16_t *clut_ptr = &g_ps1_vram[clut_y * PS1_VRAM_WIDTH + clut_x];
        for (int i = 0; i < num_colors; i++) {
            palette[i] = clut_ptr[i];
        }

        uint8_t scale_r = PsxScaleColor(r);
        uint8_t scale_g = PsxScaleColor(g);
        uint8_t scale_b = PsxScaleColor(b);

        for (int dy = 0; dy < h; dy++) {
            int vy = (tpage_y + v + dy) & 0x1FF;
            int py = (dst_base_y + dy) & 0x1FF;
            const uint16_t *vram_src = &g_ps1_vram[vy * PS1_VRAM_WIDTH];
            uint16_t *vram_dst = &g_ps1_vram[py * PS1_VRAM_WIDTH];

            for (int dx = 0; dx < w; dx++) {
                int px = (dst_base_x + dx) & 0x3FF;
                uint16_t pix = 0;

                if (depth == 0) {
                    /* 4-bit CLUT */
                    int src_x = (u + dx);
                    int vx = (tpage_x + (src_x / 4)) & 0x3FF;
                    uint16_t hw = vram_src[vx];
                    int shift = (src_x % 4) * 4;
                    int idx = (hw >> shift) & 0x0F;
                    if (idx == 0) continue; /* transparent */
                    pix = palette[idx];
                } else if (depth == 1) {
                    /* 8-bit CLUT */
                    int src_x = (u + dx);
                    int vx = (tpage_x + (src_x / 2)) & 0x3FF;
                    uint16_t hw = vram_src[vx];
                    int shift = (src_x % 2) * 8;
                    int idx = (hw >> shift) & 0xFF;
                    if (idx == 0) continue;
                    pix = palette[idx];
                } else {
                    /* 15-bit Direct */
                    int vx = (tpage_x + u + dx) & 0x3FF;
                    pix = vram_src[vx];
                    if ((pix & 0x7FFF) == 0) continue;
                }

                if (!is_raw_tex) {
                    uint8_t pr = (uint8_t)(pix & 0x1F);
                    uint8_t pg = (uint8_t)((pix >> 5) & 0x1F);
                    uint8_t pb = (uint8_t)((pix >> 10) & 0x1F);
                    pr = (uint8_t)((pr * scale_r) / 255);
                    pg = (uint8_t)((pg * scale_g) / 255);
                    pb = (uint8_t)((pb * scale_b) / 255);
                    pix = (uint16_t)(((pb) << 10) | ((pg) << 5) | pr | 0x8000u);
                } else {
                    pix |= 0x8000u;
                }
                vram_dst[px] = pix;
            }
        }
        g_psp_gpu.vram_dirty = 1;

        PspVertexTex2D *vert = (PspVertexTex2D *)sceGuGetMemory(2 * sizeof(PspVertexTex2D));
        if (!vert) vert = g_temp_tex_verts;
        vert[0].u = u;     vert[0].v = v;
        vert[0].color = col; vert[0].x = x0; vert[0].y = y0; vert[0].z = 0; vert[0].pad = 0;

        vert[1].u = (uint16_t)(u + w); vert[1].v = (uint16_t)(v + h);
        vert[1].color = col; vert[1].x = x1; vert[1].y = y1; vert[1].z = 0; vert[1].pad = 0;

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

        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(2 * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;

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

        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(2 * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;

        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- SPRT_8: 8x8 Textured Sprite (0x74..0x77) --- */
    case 0x74: case 0x75: case 0x76: case 0x77: {
        uint8_t r = (uint8_t)(word1);
        uint8_t g = (uint8_t)(word1 >> 8);
        uint8_t b = (uint8_t)(word1 >> 16);
        uint32_t col = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp(r, g, b, 0xFF);
        int16_t x = (int16_t)(packet[2] & 0xFFFF);
        int16_t y = (int16_t)(packet[2] >> 16);

        uint16_t u = (uint16_t)(packet[3] & 0xFF);
        uint16_t v = (uint16_t)((packet[3] >> 8) & 0xFF);
        uint16_t clut = (uint16_t)(packet[3] >> 16);

        int16_t x0, y0, x1, y1;
        TransformCoords(x, y, &x0, &y0);
        TransformCoords((int16_t)(x + 8), (int16_t)(y + 8), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        ApplyTexture(g_psp_gpu.current_tpage, clut, is_raw_tex);

        PspVertexTex2D *vert = (PspVertexTex2D *)sceGuGetMemory(2 * sizeof(PspVertexTex2D));
        if (!vert) vert = g_temp_tex_verts;
        vert[0].u = u;     vert[0].v = v;
        vert[0].color = col; vert[0].x = x0; vert[0].y = y0; vert[0].z = 0; vert[0].pad = 0;

        vert[1].u = (uint16_t)(u + 8); vert[1].v = (uint16_t)(v + 8);
        vert[1].color = col; vert[1].x = x1; vert[1].y = y1; vert[1].z = 0; vert[1].pad = 0;

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

        PspVertexColor2D *v = (PspVertexColor2D *)sceGuGetMemory(2 * sizeof(PspVertexColor2D));
        if (!v) v = g_temp_col_verts;
        v[0].color = col; v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].pad = 0;
        v[1].color = col; v[1].x = x1; v[1].y = y1; v[1].z = 0; v[1].pad = 0;

        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
        g_psp_gpu.primitives_drawn++;
        break;
    }

    /* --- SPRT_16: 16x16 Textured Sprite (0x7C..0x7F) --- */
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        uint8_t r = (uint8_t)(word1);
        uint8_t g = (uint8_t)(word1 >> 8);
        uint8_t b = (uint8_t)(word1 >> 16);
        uint32_t col = is_raw_tex ? 0xFFFFFFFF : PsxModulateColorToPsp(r, g, b, 0xFF);
        int16_t x = (int16_t)(packet[2] & 0xFFFF);
        int16_t y = (int16_t)(packet[2] >> 16);

        uint16_t u = (uint16_t)(packet[3] & 0xFF);
        uint16_t v = (uint16_t)((packet[3] >> 8) & 0xFF);
        uint16_t clut = (uint16_t)(packet[3] >> 16);

        int16_t x0, y0, x1, y1;
        TransformCoords(x, y, &x0, &y0);
        TransformCoords((int16_t)(x + 16), (int16_t)(y + 16), &x1, &y1);

        ApplySemiTransparency(is_semi_trans, abr_mode);
        ApplyTexture(g_psp_gpu.current_tpage, clut, is_raw_tex);

        PspVertexTex2D *vert = (PspVertexTex2D *)sceGuGetMemory(2 * sizeof(PspVertexTex2D));
        if (!vert) vert = g_temp_tex_verts;
        vert[0].u = u;      vert[0].v = v;
        vert[0].color = col; vert[0].x = x0; vert[0].y = y0; vert[0].z = 0; vert[0].pad = 0;

        vert[1].u = (uint16_t)(u + 16); vert[1].v = (uint16_t)(v + 16);
        vert[1].color = col; vert[1].x = x1; vert[1].y = y1; vert[1].z = 0; vert[1].pad = 0;

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
    uintptr_t high_bits = (uintptr_t)packet & 0xFF000000u;
    while (cur != NULL) {
        PspGpu_ExecuteSinglePrim(cur);

        uint32_t tag = cur[0];
        uint32_t next_addr = tag & 0x00FFFFFF;

        /* Terminator on PS1 GPU DMA is 0x00FFFFFF */
        if (next_addr == 0x00FFFFFF || next_addr == 0) {
            break;
        }

        /* Next packet pointer in RAM (preserving PSP user-space upper byte) */
        cur = (const uint32_t *)(high_bits | (uintptr_t)next_addr);
    }
}

/* ------------------------------------------------------------------------------
 * DISPENV Shadow VRAM Base Quad Presentation
 * Supports 16-bit direct color (isrgb24=0) and 24-bit RGB (isrgb24=1)
 * ------------------------------------------------------------------------------
 */
void psp_render_vram_dispenv(void) {
    int disp_x = g_psp_gpu.current_dispenv.disp.x;
    int disp_y = g_psp_gpu.current_dispenv.disp.y;
    int disp_w = g_psp_gpu.current_dispenv.disp.w;
    int disp_h = g_psp_gpu.current_dispenv.disp.h;
    int isrgb24 = g_psp_gpu.current_dispenv.isrgb24;

    if (disp_w <= 0) disp_w = 320;
    if (disp_h <= 0) disp_h = 240;
    if (disp_w > 512) disp_w = 512;
    if (disp_h > 512) disp_h = 512;

    /* Decode active VRAM framebuffer into dedicated 512x512 DISPENV texture buffer */
    if (!isrgb24) {
        /* 16-bit 15bpp direct color mode */
        for (int r = 0; r < disp_h; r++) {
            int src_y = (disp_y + r) & 0x1FF;
            const uint16_t *src_row = &g_ps1_vram[src_y * PS1_VRAM_WIDTH];
            uint16_t *dst_row = &s_dispenv_tex[r * 512];
            for (int c = 0; c < disp_w; c++) {
                int src_x = (disp_x + c) & 0x3FF;
                uint16_t pix = src_row[src_x];
                if (pix != 0) pix |= 0x8000;
                dst_row[c] = pix;
            }
        }
        /* Writeback decoded DISPENV texture to RAM for GE */
        sceKernelDcacheWritebackInvalidateRange(s_dispenv_tex, sizeof(s_dispenv_tex));
        sceGuTexMode(GU_PSM_5551, 0, 0, 0);
        sceGuTexImage(0, 512, 512, 512, s_dispenv_tex);
    } else {
        /* 24-bit RGB mode (3 bytes per pixel packed in 16-bit VRAM words) */
        for (int r = 0; r < disp_h; r++) {
            int src_y = (disp_y + r) & 0x1FF;
            const uint8_t *src_bytes = (const uint8_t *)&g_ps1_vram[src_y * PS1_VRAM_WIDTH];
            uint32_t *dst_row = &s_dispenv_tex32[r * 512];
            for (int c = 0; c < disp_w; c++) {
                int byte_idx = (disp_x * 2) + (c * 3);
                uint8_t red   = src_bytes[byte_idx + 0];
                uint8_t green = src_bytes[byte_idx + 1];
                uint8_t blue  = src_bytes[byte_idx + 2];
                dst_row[c] = 0xFF000000u | ((uint32_t)blue << 16) | ((uint32_t)green << 8) | (uint32_t)red;
            }
        }
        /* Writeback decoded DISPENV texture to RAM for GE */
        sceKernelDcacheWritebackInvalidateRange(s_dispenv_tex32, sizeof(s_dispenv_tex32));
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, 512, 512, 512, s_dispenv_tex32);
    }
    sceGuTexFlush();
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    g_psp_gpu.semi_trans_enabled = 0;

    /* Render base quad stretched to 4:3 viewport: (59, 0) to (59 + 362, 272) */
    PspVertexTex2D *v = (PspVertexTex2D *)sceGuGetMemory(2 * sizeof(PspVertexTex2D));
    if (v) {
        v[0].u = 0;
        v[0].v = 0;
        v[0].color = 0xFFFFFFFF;
        v[0].x = 59;
        v[0].y = 0;
        v[0].z = 0;

        v[1].u = (uint16_t)disp_w;
        v[1].v = (uint16_t)disp_h;
        v[1].color = 0xFFFFFFFF;
        v[1].x = 59 + 362;
        v[1].y = 272;
        v[1].z = 0;

        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
    }
}

void PspGpu_DrawOTag(const uint32_t *ot, int count) {
    PspGpu_BeginFrame();

    /* Render authentic DISPENV shadow VRAM base quad */
    psp_render_vram_dispenv();

    if (ot && count > 0) {
        uintptr_t high_bits = (uintptr_t)ot & 0xFF000000u;

        /* Back-to-front rendering: traverse Ordering Table entries from count-1 down to 0 */
        for (int i = count - 1; i >= 0; i--) {
            uint32_t head = ot[i];
            uint32_t addr24 = head & 0x00FFFFFFu;
            if (addr24 == 0x00FFFFFFu || addr24 == 0) continue;

            const uint32_t *packet = (const uint32_t *)(high_bits | (uintptr_t)addr24);
            PspGpu_ExecutePacket(packet);
        }
    }

    PspGpu_EndFrame();
    g_psp_gpu.frame_drawn = 1;
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

static void (*s_vsync_callback)(void) = NULL;
static void (*s_drawsync_callback)(void) = NULL;

void (*VSyncCallback(void (*func)(void)))(void) {
    void (*prev)(void) = s_vsync_callback;
    s_vsync_callback = func;
    return prev;
}

void (*DrawSyncCallback(void (*func)(void)))(void) {
    void (*prev)(void) = s_drawsync_callback;
    s_drawsync_callback = func;
    return prev;
}

int SetGraphDebug(int level) {
    (void)level;
    return 0;
}

int DrawSync(int mode) {
    (void)mode;
    if (g_psp_gpu.in_frame) {
        sceKernelDcacheWritebackInvalidateAll();
        sceGuFinish();
        sceGuSync(0, 0);
        g_psp_gpu.in_frame = 0;
        g_psp_gpu.frame_drawn = 1;
    }
    if (s_drawsync_callback) {
        s_drawsync_callback();
    }
    return 0;
}

void psp_write_ppm_screenshot(const char *path, const uint16_t *fb) {
    static uint8_t s_ppm_row[PSP_SCREEN_WIDTH * 3];
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd < 0) return;

    const char *hdr = "P6\n480 272\n255\n";
    sceIoWrite(fd, hdr, 15);

    int disp_w = (g_psp_gpu.current_dispenv.disp.w > 0) ? g_psp_gpu.current_dispenv.disp.w : 320;
    int disp_h = (g_psp_gpu.current_dispenv.disp.h > 0) ? g_psp_gpu.current_dispenv.disp.h : 240;

    for (int y = 0; y < PSP_SCREEN_HEIGHT; y++) {
        int src_y = (y * disp_h) / PSP_SCREEN_HEIGHT;
        const uint16_t *row = &fb[y * PSP_BUFFER_STRIDE];
        for (int x = 0; x < PSP_SCREEN_WIDTH; x++) {
            if (g_psp_gpu.current_dispenv.isrgb24) {
                if (x >= 59 && x < 59 + 362) {
                    int src_x = ((x - 59) * disp_w) / 362;
                    uint32_t c32 = s_dispenv_tex32[src_y * 512 + src_x];
                    s_ppm_row[x * 3 + 0] = (uint8_t)(c32 & 0xFF);
                    s_ppm_row[x * 3 + 1] = (uint8_t)((c32 >> 8) & 0xFF);
                    s_ppm_row[x * 3 + 2] = (uint8_t)((c32 >> 16) & 0xFF);
                } else {
                    s_ppm_row[x * 3 + 0] = 0;
                    s_ppm_row[x * 3 + 1] = 0;
                    s_ppm_row[x * 3 + 2] = 0;
                }
            } else {
                uint16_t c = row[x];
                if (x >= 59 && x < 59 + 362) {
                    int src_x = ((x - 59) * disp_w) / 362;
                    uint16_t v_pix = s_dispenv_tex[src_y * 512 + src_x];
                    if ((v_pix & 0x7FFFu) != 0) c = v_pix;
                } else {
                    c = 0;
                }
                uint8_t r = (uint8_t)((c & 0x1F) << 3);
                uint8_t g = (uint8_t)(((c >> 5) & 0x1F) << 3);
                uint8_t b = (uint8_t)(((c >> 10) & 0x1F) << 3);
                s_ppm_row[x * 3 + 0] = r | (r >> 5);
                s_ppm_row[x * 3 + 1] = g | (g >> 5);
                s_ppm_row[x * 3 + 2] = b | (b >> 5);
            }
        }
        sceIoWrite(fd, s_ppm_row, sizeof(s_ppm_row));
    }
    sceIoClose(fd);
}

int VSync(int mode) {
    (void)mode;
    if (g_psp_gpu.in_frame) {
        sceKernelDcacheWritebackInvalidateAll();
        sceGuFinish();
        sceGuSync(0, 0);
        g_psp_gpu.in_frame = 0;
        g_psp_gpu.frame_drawn = 1;
    } else if (!g_psp_gpu.frame_drawn) {
        PspGpu_BeginFrame();
        psp_render_vram_dispenv();
        PspGpu_EndFrame();
    }
    g_psp_gpu.frame_drawn = 0;

    sceDisplayWaitVblankStart();
    g_psp_gpu.disp_buffer = g_psp_gpu.draw_buffer;
    g_psp_gpu.draw_buffer = sceGuSwapBuffers();
    g_psp_gpu.frame_counter++;

    extern int g_SceneDispatchToken;
    int is_carnegie = ((uint32_t)g_SceneDispatchToken == 0xA8000148u);
    static int s_dump_checked = 0;
    static int s_enable_frame_dump = 0;
    static int s_dumped_notice = 0;
    static int s_dumped_logo = 0;
    static int s_dumped_intro = 0;
    static int s_dumped_title = 0;
    static int s_dumped_newgame = 0;
    static int s_dumped_carnegie = 0;

    if (!s_dump_checked) {
        s_dump_checked = 1;
        SceUID fd = sceIoOpen("frame_dump_enable.flag", PSP_O_RDONLY, 0777);
        if (fd < 0) {
            fd = sceIoOpen("ms0:/PSP/GAME/ParasiteEve/frame_dump_enable.flag", PSP_O_RDONLY, 0777);
        }
        if (fd >= 0) {
            s_enable_frame_dump = 1;
            sceIoClose(fd);
        }
    }

    int is_log_frame = (g_psp_gpu.frame_counter % 60 == 0);
    int should_dump_notice = (s_enable_frame_dump && g_psp_gpu.frame_counter >= 15 && g_psp_gpu.frame_counter <= 25 && !s_dumped_notice);
    int should_dump_logo = (s_enable_frame_dump && g_psp_gpu.frame_counter >= 70 && g_psp_gpu.frame_counter <= 95 && !s_dumped_logo);
    int should_dump_intro = (s_enable_frame_dump && g_psp_gpu.frame_counter >= 155 && g_psp_gpu.frame_counter <= 185 && !s_dumped_intro);
    int should_dump_title = (s_enable_frame_dump && g_psp_gpu.frame_counter >= 265 && g_psp_gpu.frame_counter <= 295 && !s_dumped_title);
    int should_dump_newgame = (s_enable_frame_dump && g_psp_gpu.frame_counter >= 415 && g_psp_gpu.frame_counter <= 440 && !s_dumped_newgame);
    int should_dump_carnegie = (s_enable_frame_dump && is_carnegie && !s_dumped_carnegie);

    if (is_log_frame || should_dump_notice || should_dump_logo || should_dump_intro || should_dump_title || should_dump_newgame || should_dump_carnegie) {
        uint32_t vram_nonzero = 0;
        for (int p = 0; p < PS1_VRAM_PIXELS; p++) {
            if (g_ps1_vram[p] != 0) vram_nonzero++;
        }

        uint16_t *edram_disp = (uint16_t *)((uintptr_t)0x44000000u + (uintptr_t)g_psp_gpu.disp_buffer);
        uint32_t disp_nonblack = 0;

        int disp_w = (g_psp_gpu.current_dispenv.disp.w > 0) ? g_psp_gpu.current_dispenv.disp.w : 320;
        int disp_h = (g_psp_gpu.current_dispenv.disp.h > 0) ? g_psp_gpu.current_dispenv.disp.h : 240;
        for (int y = 0; y < PSP_SCREEN_HEIGHT; y++) {
            int src_y = (y * disp_h) / PSP_SCREEN_HEIGHT;
            for (int x = 0; x < PSP_SCREEN_WIDTH; x++) {
                uint16_t out_pix = edram_disp[y * PSP_BUFFER_STRIDE + x];
                if (x >= 59 && x < 59 + 362) {
                    int src_x = ((x - 59) * disp_w) / 362;
                    if (g_psp_gpu.current_dispenv.isrgb24) {
                        uint32_t c32 = s_dispenv_tex32[src_y * 512 + src_x];
                        uint8_t r = (uint8_t)(c32 & 0xFF);
                        uint8_t g = (uint8_t)((c32 >> 8) & 0xFF);
                        uint8_t b = (uint8_t)((c32 >> 16) & 0xFF);
                        out_pix = (uint16_t)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3) | 0x8000u);
                    } else {
                        uint16_t v_pix = s_dispenv_tex[src_y * 512 + src_x];
                        if ((v_pix & 0x7FFFu) != 0) out_pix = v_pix;
                    }
                } else {
                    out_pix = 0x8000u;
                }
                edram_disp[y * PSP_BUFFER_STRIDE + x] = out_pix;
                if ((out_pix & 0x7FFFu) != 0) {
                    disp_nonblack++;
                }
            }
        }

        if (is_log_frame) {
            char log_buf[192];
            int log_len = snprintf(log_buf, sizeof(log_buf),
                "[VRAM-CHECK] Frame %u: Non-zero texels: %u, Display non-black pixels: %u, OT primitives drawn: %u\n",
                (unsigned int)g_psp_gpu.frame_counter,
                (unsigned int)vram_nonzero,
                (unsigned int)disp_nonblack,
                (unsigned int)g_psp_gpu.primitives_drawn);
            if (log_len > 0) {
                sceIoWrite(2, log_buf, (SceSize)log_len);
            }
        }

        if (should_dump_notice) {
            s_dumped_notice = 1;
            psp_write_ppm_screenshot("ms0:/frame_dump_notice.ppm", edram_disp);
            psp_write_ppm_screenshot("frame_dump_notice.ppm", edram_disp);
        }
        if (should_dump_logo) {
            s_dumped_logo = 1;
            psp_write_ppm_screenshot("ms0:/frame_dump_logo.ppm", edram_disp);
            psp_write_ppm_screenshot("frame_dump_logo.ppm", edram_disp);
        }
        if (should_dump_intro) {
            s_dumped_intro = 1;
            psp_write_ppm_screenshot("ms0:/frame_dump_intro_fmv.ppm", edram_disp);
            psp_write_ppm_screenshot("frame_dump_intro_fmv.ppm", edram_disp);
            psp_write_ppm_screenshot("ms0:/frame_dump.ppm", edram_disp);
            psp_write_ppm_screenshot("frame_dump.ppm", edram_disp);
        }
        if (should_dump_title) {
            s_dumped_title = 1;
            psp_write_ppm_screenshot("ms0:/frame_dump_title.ppm", edram_disp);
            psp_write_ppm_screenshot("frame_dump_title.ppm", edram_disp);
        }
        if (should_dump_newgame) {
            s_dumped_newgame = 1;
            psp_write_ppm_screenshot("ms0:/frame_dump_newgame.ppm", edram_disp);
            psp_write_ppm_screenshot("frame_dump_newgame.ppm", edram_disp);
        }
        if (should_dump_carnegie && disp_nonblack > 0) {
            s_dumped_carnegie = 1;
            psp_write_ppm_screenshot("ms0:/frame_dump_carnegie.ppm", edram_disp);
            psp_write_ppm_screenshot("frame_dump_carnegie.ppm", edram_disp);
            psp_write_ppm_screenshot("ms0:/frame_dump.ppm", edram_disp);
            psp_write_ppm_screenshot("frame_dump.ppm", edram_disp);
        }
    }

    if (s_vsync_callback) {
        s_vsync_callback();
    }
    return 0;
}

uint32_t PspGpu_GetFrameCounter(void) {
    return g_psp_gpu.frame_counter;
}

void ResetGraph(int mode) {
    (void)mode;
    PspGpu_Reset();
}

void SetDispMask(int mask) {
    PspGpu_SetDispMask(mask);
}

int LoadImage(RECT *rect, void *pixels) {
    return PspGpu_LoadImage(rect, pixels);
}

int StoreImage(RECT *rect, void *pixels) {
    return PspGpu_StoreImage(rect, pixels);
}

void MoveImage(RECT *src, int dst_x, int dst_y) {
    PspGpu_MoveImage(src, dst_x, dst_y);
}

void ClearImage(RECT *rect, int r, int g, int b) {
    PspGpu_ClearImage(rect, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

DRAWENV *PutDrawEnv(DRAWENV *env) {
    return PspGpu_PutDrawEnv(env);
}

DISPENV *PutDispEnv(DISPENV *env) {
    return PspGpu_PutDispEnv(env);
}

int DrawOTag(u_int *ot) {
    PspGpu_DrawOTag((const uint32_t *)ot, 4096);
    return 0;
}

int DrawPrim(void *p) {
    if (p) {
        if (!g_psp_gpu.in_frame) {
            PspGpu_BeginFrame();
            psp_render_vram_dispenv();
        }
        PspGpu_ExecuteSinglePrim((const uint32_t *)p);
        g_psp_gpu.frame_drawn = 1;
    }
    return 0;
}

DRAWENV *SetDefDrawEnv(DRAWENV *env, int x, int y, int w, int h) {
    return PspGpu_SetDefDrawEnv(env, x, y, w, h);
}

DISPENV *SetDefDispEnv(DISPENV *env, int x, int y, int w, int h) {
    return PspGpu_SetDefDispEnv(env, x, y, w, h);
}

void SetDrawTPage(void *p, int dfe, int dtd, int tpage) {
    uint32_t *pkt = (uint32_t *)p;
    if (pkt) {
        pkt[0] = 0x01000000 | ((uint32_t)(dfe ? 1 : 0) << 9) | ((uint32_t)(dtd ? 1 : 0) << 8);
        pkt[1] = 0xE1000000 | (tpage & 0xFFFF);
    }
}

uint32_t *ClearOTagR(uint32_t *ot, int count) {
    if (!ot || count <= 0) return ot;
    for (int i = count - 1; i > 0; i--) {
        ot[i] = (uint32_t)(uintptr_t)&ot[i - 1];
    }
    ot[0] = 0x00FFFFFF;
    return ot;
}

