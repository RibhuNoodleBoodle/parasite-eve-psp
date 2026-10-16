/* ==============================================================================
 * psp_gpu_gu.h - Native Sony PlayStation Portable (PSP) libgu/libgum Backend
 * for Parasite Eve (PS1) Decompilation Port.
 *
 * Implements hardware-accelerated PlayStation 1 GPU packet and primitive
 * decoding directly into the PSP Graphics Engine (GE) display list.
 *
 * Target: Sony PSP (MIPS Allegrex + GE hardware)
 * Headers: <pspgu.h>, <pspgum.h>, <pspdisplay.h>
 * ==============================================================================
 */

#ifndef PSP_GPU_GU_H
#define PSP_GPU_GU_H

#include "common.h"
#include <stdint.h>
#include <stddef.h>
#include <psptypes.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspdisplay.h>
#include "pe1/psyq_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------
 * Display & Framebuffer Geometry Constants
 * ------------------------------------------------------------------------------
 */
#define PSP_SCREEN_WIDTH        480
#define PSP_SCREEN_HEIGHT       272
#define PSP_BUFFER_STRIDE       512

#define PS1_VRAM_WIDTH          1024
#define PS1_VRAM_HEIGHT         512
#define PS1_VRAM_PIXELS         (PS1_VRAM_WIDTH * PS1_VRAM_HEIGHT)
#define PS1_VRAM_BYTES          (PS1_VRAM_PIXELS * 2)

extern uint16_t g_ps1_vram[PS1_VRAM_PIXELS];
void psp_render_vram_dispenv(void);

#define PS1_NATIVE_WIDTH        320
#define PS1_NATIVE_HEIGHT       240

/* Display list allocation: 262,144 bytes (256 KB) aligned to 64-byte boundary */
#define PSP_GU_DISPLAY_LIST_SIZE 262144

/* ------------------------------------------------------------------------------
 * PS1 GPU Primitive Codes
 * ------------------------------------------------------------------------------
 */
/* Miscellaneous */
#define PS1_CMD_NOP             0x00
#define PS1_CMD_CLEAR_CACHE     0x01
#define PS1_CMD_FILL_RECT       0x02
#define PS1_CMD_MOVE_IMAGE      0x80
#define PS1_CMD_LOAD_IMAGE      0xA0
#define PS1_CMD_STORE_IMAGE     0xC0

/* Environment settings in packet stream */
#define PS1_CMD_DRAW_MODE       0xE1
#define PS1_CMD_TEX_WINDOW      0xE2
#define PS1_CMD_DRAW_AREA_TL    0xE3
#define PS1_CMD_DRAW_AREA_BR    0xE4
#define PS1_CMD_DRAW_OFFSET     0xE5
#define PS1_CMD_MASK_SETTING    0xE6

/* ------------------------------------------------------------------------------
 * Primitive Structures (Psy-Q Compatible)
 * ------------------------------------------------------------------------------
 */
#ifndef RECT_DEFINED
#define RECT_DEFINED
typedef struct RECT {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} RECT;
#endif

#ifndef DR_ENV_DEFINED
#define DR_ENV_DEFINED
typedef struct DR_ENV {
    uint32_t tag;
    uint32_t code[15];
} DR_ENV;
#endif

#ifndef DRAWENV_DEFINED
#define DRAWENV_DEFINED
typedef struct DRAWENV {
    RECT clip;             /* Clip area */
    int16_t ofs[2];        /* Drawing offset */
    RECT tw;               /* Texture window */
    uint16_t tpage;        /* Texture page */
    uint8_t dtd;           /* Dither flag */
    uint8_t dfe;           /* Draw to display enable */
    uint8_t isbg;          /* Clear background flag */
    uint8_t r0, g0, b0;    /* Background clear color */
    DR_ENV dr_env;         /* Reserved GPU packet environment */
} DRAWENV;
#endif

#ifndef DISPENV_DEFINED
#define DISPENV_DEFINED
typedef struct DISPENV {
    RECT disp;             /* Display area in VRAM */
    RECT screen;           /* Screen offset / geometry */
    uint8_t isinter;       /* Interlace flag */
    uint8_t isrgb24;       /* 24-bit direct color flag */
    uint8_t pad0;
    uint8_t pad1;
} DISPENV;
#endif

/* ------------------------------------------------------------------------------
 * PSP Screen Scaling & Aspect Ratio Modes
 * ------------------------------------------------------------------------------
 */
typedef enum PspScalingMode {
    PSP_SCALE_ORIGINAL_CENTER = 0, /* 1:1 Pixel perfect centered (320x240 inside 480x272) */
    PSP_SCALE_4_3_PILLARBOX   = 1, /* Aspect-correct 4:3 (362x272 with black pillarboxes) */
    PSP_SCALE_16_9_FULL       = 2  /* Widescreen stretch (full 480x272 display) */
} PspScalingMode;

typedef enum PspFilterMode {
    PSP_FILTER_NEAREST = 0,        /* Sharp point-sampling */
    PSP_FILTER_BILINEAR = 1        /* Smooth hardware bilinear filtering */
} PspFilterMode;

/* ------------------------------------------------------------------------------
 * PSP Hardware Vertex Formats
 * ------------------------------------------------------------------------------
 */
/* Untextured 2D vertex: Color ABGR8888 + X, Y, Z + Pad (12 bytes, 4-byte aligned) */
typedef struct PspVertexColor2D {
    uint32_t color;
    int16_t x, y, z;
    int16_t pad;
} PspVertexColor2D;

/* Textured 2D vertex: U, V + Color ABGR8888 + X, Y, Z + Pad (16 bytes, 4-byte aligned) */
typedef struct PspVertexTex2D {
    uint16_t u, v;
    uint32_t color;
    int16_t x, y, z;
    int16_t pad;
} PspVertexTex2D;

/* ------------------------------------------------------------------------------
 * PSP GPU State & Diagnostics
 * ------------------------------------------------------------------------------
 */
typedef struct PspGpuState {
    int initialized;
    int in_frame;
    int frame_drawn;
    uint32_t frame_counter;

    /* Display buffer pointers in PSP EDRAM */
    void *draw_buffer;
    void *disp_buffer;
    void *depth_buffer;

    /* Current PS1 draw environment state */
    int16_t offset_x;
    int16_t offset_y;
    int16_t clip_x1;
    int16_t clip_y1;
    int16_t clip_x2;
    int16_t clip_y2;

    uint16_t current_tpage;
    uint16_t current_clut;
    int semi_trans_mode;      /* ABR 0..3 */
    int semi_trans_enabled;

    /* Texture Window */
    uint8_t twin_mask_x;
    uint8_t twin_mask_y;
    uint8_t twin_offset_x;
    uint8_t twin_offset_y;

    /* Presentation settings */
    PspScalingMode scaling_mode;
    PspFilterMode filter_mode;
    int display_mask;          /* 0 = display active, 1 = blanked */

    /* Current PS1 display environment state */
    int has_dispenv;
    DISPENV current_dispenv;

    /* Profiling & statistics */
    uint32_t primitives_drawn;
    uint32_t textures_uploaded;
} PspGpuState;

void psp_render_vram_dispenv(void);

extern PspGpuState g_psp_gpu;

/* ------------------------------------------------------------------------------
 * Core API Function Prototypes
 * ------------------------------------------------------------------------------
 */

/* Subsystem Lifecycle */
void PspGpu_Init(void);
void PspGpu_Shutdown(void);
void PspGpu_Reset(void);

/* Presentation Configuration */
void PspGpu_SetScalingMode(PspScalingMode mode);
void PspGpu_SetFilterMode(PspFilterMode filter);
void PspGpu_SetDispMask(int mask);

/* Frame Boundary Management */
void PspGpu_BeginFrame(void);
void PspGpu_EndFrame(void);
void PspGpu_Flip(void);
void PspGpu_Sync(void);

/* VRAM Transfers */
int  PspGpu_LoadImage(const RECT *rect, const void *pixels);
int  PspGpu_StoreImage(const RECT *rect, void *pixels);
int  PspGpu_MoveImage(const RECT *src, int dst_x, int dst_y);
void PspGpu_ClearImage(const RECT *rect, uint8_t r, uint8_t g, uint8_t b);

/* Ordering Table Execution */
void PspGpu_DrawOTag(const uint32_t *ot, int count);
void PspGpu_DrawOTagEnv(const uint32_t *ot, int count, const DRAWENV *env);
void PspGpu_ExecutePacket(const uint32_t *packet);
void PspGpu_ExecuteSinglePrim(const uint32_t *packet);

/* Psy-Q API Compatibility Shims */
DRAWENV *PspGpu_PutDrawEnv(const DRAWENV *env);
DISPENV *PspGpu_PutDispEnv(const DISPENV *env);
DRAWENV *PspGpu_SetDefDrawEnv(DRAWENV *env, int x, int y, int w, int h);
DISPENV *PspGpu_SetDefDispEnv(DISPENV *env, int x, int y, int w, int h);

/* Direct VRAM Access */
uint16_t *PspGpu_GetVramPointer(void);
void PspGpu_InvalidateVramRange(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* PS1 Psy-Q Standard API Declarations */
int  DrawSync(int mode);
int  VSync(int mode);
void ResetGraph(int mode);
void SetDispMask(int mask);
int  LoadImage(RECT *rect, void *pixels);
int  StoreImage(RECT *rect, void *pixels);
void MoveImage(RECT *src, int dst_x, int dst_y);
void ClearImage(RECT *rect, int r, int g, int b);
DRAWENV *PutDrawEnv(DRAWENV *env);
DISPENV *PutDispEnv(DISPENV *env);
int  DrawOTag(u_int *ot);
int  DrawPrim(void *p);

#ifdef __cplusplus
}
#endif

#endif /* PSP_GPU_GU_H */
