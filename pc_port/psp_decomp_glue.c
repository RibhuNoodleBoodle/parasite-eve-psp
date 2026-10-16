/*
 * psp_decomp_glue.c - Bridge for Parasite Eve PS1 Decompiled Game Loop & Overlays
 *
 * Provides storage, tables, buffers, and stubs for running the authentic
 * PS1 decompiled game logic (src/main/boot/main.c, src/overlays/menu_memcard/list_screen.c,
 * page_flip.c, fade.c, image_nodes.c) on the Sony PlayStation Portable.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "common.h"
#include "pe1/boot.h"
#include "pe1/boot_disc_check.h"
#include "pe1/cdrom.h"
#include "pe1/cdrom_buffers.h"
#include "pe1/display_env.h"
#include "pe1/draw_buffers.h"
#include "pe1/draw_state.h"
#include "pe1/game_state.h"
#include "pe1/pad_state.h"
#include "pe1/pe_image.h"
#include "pe1/psyq_gpu.h"
#include "pe1/asset_tim.h"
#include "pe1/textbox.h"
#include "pe1/render_matrix_slot.h"
#include "pe1/scene_assets.h"
#include "pe1/game_audio_state.h"

#include "psp_gpu_gu.h"
#include "psp_cdrom_iso.h"
#include "psp_sdl_input.h"

/* Akao audio state storage */
AkaoVoiceBankEntry g_AkaoVoiceBankEntries[32];
AkaoRuntimeState g_AkaoRuntimeState;
void *g_AkaoBgmHandle[16];

/* ── Disc & Asset Constants ─────────────────────────────────────────────────── */

char g_PeDisc1MarkerPath[] = "\\FMV1\\PEDISC01.IDF;1";
char g_PeDisc2MarkerPath[] = "\\FMV2\\PEDISC02.IDF;1";
char g_PeImagePath[]       = "\\PE.IMG;1";

unsigned int g_PeImageBaseLba = 1013;
signed char  g_PeImageMountFlags = 1;

u16 g_PeImageSectorTable[128] = {
    0, 71, 100, 150, 200, 250, 300, 350, 400, 450, 500, 550, 600, 650, 700, 750,
    800, 850, 900, 950, 965, 987, 1000, 1050, 1100, 1150, 1200, 1250, 1300, 1350
};
u16 g_PeImageSectorBoundsNext[128];

u16 g_StrFileDirLba[2] = {0, 2};
static u8 s_str_file_dir_storage[4096];
u8 *g_StrFileDirBuffer = s_str_file_dir_storage;
u8 *g_StrFileDirectory = s_str_file_dir_storage;

u16 g_BootDisplayDataLbaTbl = 0;
u16 g_MainAssetLbaTbl[2] = {0, 1};
u16 g_LargeTexLbaTbl[2]  = {0, 1};
u16 g_SmallTexLbaTbl[2]  = {0, 1};
u16 g_FontUiLbaTbl[2]    = {0, 1};
u16 g_SceneInitLbaTbl0[2]= {0, 1};

/* ── Work Buffers & Tables ─────────────────────────────────────────────────── */

u8  g_BootWorkArea[0x20000];
u8  g_SceneLoadScratchArea[0x10000];
u8 *g_SceneLoadScratchBuffer[2] = {g_SceneLoadScratchArea, g_SceneLoadScratchArea};

u8  g_CdLoadArea[0x80000];
u8 *g_CdLoadAreaBase = g_CdLoadArea;
u8 *g_MdecOverlayLoadBase = g_CdLoadArea;
u8 *g_LoadedSceneAssetBlock[2] = {g_CdLoadArea, g_CdLoadArea};

u8 *g_SceneBankAssetSource = NULL;
u8 *g_VoiceBankBase = NULL;
u8 *g_EntityBankBuffer = NULL;
u8 *g_EntityTextureBlob = NULL;
u8 *g_ObjectTextureBlobMidWriteOnly = NULL;
u8 *g_ScriptSpriteBuffer = NULL;
u8 *g_ScenePmSlotBanks = NULL;
u8 *g_SceneObjectWork = NULL;
u8 *D_8010BD00 = NULL;
u8 *D_80120D08 = NULL;

int g_ActiveDrawSlot = 0;
volatile unsigned char g_SceneInitSubphase = 0;

u32 g_OtBufferTable[2][4096];
u32 g_RenderOtBufferBaseAlt[2][4096];

u8  g_RenderScratchBuffer[0x8000];
u8 *g_RenderScratchBufferBase[2] = {g_RenderScratchBuffer, g_RenderScratchBuffer};

u8  g_RenderFrontBuffer[0x8000];
u8 *g_RenderFrontBufferBase[2] = {g_RenderFrontBuffer, g_RenderFrontBuffer};

u8  g_RenderBackBuffer[0x8000];
u8 *g_RenderBackBufferBase[2] = {g_RenderBackBuffer, g_RenderBackBuffer};

u8  g_RenderAnimBuffer[0x8000];
u8 *g_RenderAnimBufferBase[2] = {g_RenderAnimBuffer, g_RenderAnimBuffer};

DISPENV g_RenderDispEnvArray[2];
DRAWENV g_RenderDrawEnvArray[2];

PadPacket g_PadPackets[2];
s32 g_FieldRenderFlagTable[32];
s32 g_ScriptVariableBank[128];

volatile int D_800BCF88 = 0;
u32 D_8009D254 = 0;

RenderBufferPrefix D_800B0E38;
GpuDrawEnabledFlag g_DrawEnabled = { .value = 1 };

short g_SeqElapsed = 0;
u32 g_FrameRngCounter = 0;
u8 g_DiscChangeFlags = 0;

struct {
    int baseLba;
} g_DiscLoadState = {0};

u16 g_CdDiskType = 0;
char g_CdReadErrorFormat[] = "[CDROM] Read error at LBA %u size %u\n";

signed char g_DrawBaseY = 0;
RenderMatrixSlot g_ViewMatrixSlot;
u8 g_PadActuatorAlign[32];
u32 g_PadComboSequence[32];
u8 g_PadSetActBuffer[32];
u32 g_FieldPadHeldFrameCounts[32];
u8 g_Base32CharTable[32] __attribute__((nonstring)) = "0123456789ABCDEFGHIJKLMNOPQRSTUV";

/* ── Memory Card Screen & Title Overlay Structures ─────────────────────────── */

typedef struct MemcardImage { u8 header[16]; s16 width, height; } MemcardImage;
typedef struct MemcardImageNode {
    struct MemcardImageNode *next;
    s16 x, y, width, height;
    void *draw;
    void *update;
    void *field14;
    MemcardImage *volatile image;
    s32 intensity, targetY, stepY, parameter, kind, freePending;
} MemcardImageNode;
typedef struct MemcardImagePreset { s32 x, y, parameter; } MemcardImagePreset;

typedef struct MemcardScreenBuffer {
    DRAWENV draw;
    DISPENV disp;
    RECT overlay;
    RECT dirty;
    u8 pad80[0x8000];
    u8 overlayImage[0x14000];
} MemcardScreenBuffer;

static MemcardScreenBuffer s_memcard_screens[2];
MemcardScreenBuffer *g_MemcardScreenBuffers[2] = {&s_memcard_screens[0], &s_memcard_screens[1]};
MemcardScreenBuffer *g_MemcardCurrentScreen = &s_memcard_screens[0];

/* In PS1 RAM (overlay_abi.txt), g_MemcardFirstDrawScreen == g_MemcardScreenBuffers[0]
 * and g_MemcardSecondScreen / g_MemcardSecondScreenView == g_MemcardScreenBuffers[1]. */
__asm__(
    ".globl g_MemcardFirstDrawScreen\n"
    ".set g_MemcardFirstDrawScreen, g_MemcardScreenBuffers\n"
    ".globl g_MemcardSecondScreen\n"
    ".set g_MemcardSecondScreen, g_MemcardScreenBuffers + 4\n"
    ".globl g_MemcardSecondScreenView\n"
    ".set g_MemcardSecondScreenView, g_MemcardScreenBuffers + 4\n"
);

int g_MemcardScreenPage = 0;
int g_MemcardScreenTimer = 0;
s32 g_MemcardPrevPad = 0;
int g_LogoFadePlayed = 0;

DISPENV g_MemcardListSavedDispEnv[2];
DRAWENV g_MemcardListSavedDrawEnv[2];

MemcardImageNode g_MemcardImageNodePool[8];
MemcardImageNode *g_MemcardFreeImageNodes = NULL;
MemcardImageNode *g_MemcardActiveNodeHead = NULL;
MemcardImageNode *g_MemcardActiveNodeTail = NULL;
MemcardImageNode *g_MemcardPendingNodeHead = NULL;
MemcardImageNode *g_MemcardPendingNodeTail = NULL;

MemcardImagePreset g_MemcardImagePresets[16] = {
    {   0,   0,  0 },
    {  88, 180, -1 },
    { 104, 200,  0 },
    { 116, 160,  0 },
    { 116, 180,  0 },
    { 116, 180,  0 },
    { 116, 140,  0 },
    { 116, 180,  0 }
};

/* Title Screen & Squaresoft Logo asset bundle loaded from PE.IMG (0x80193254):
 * +0x00: g_MemcardImageBundle (count = 9)
 * +0x04: g_MemcardImageOffsets[0..8] = {40, 195900, 206288, 214084, 220152, 226220, 232288, 238356, 244424}
 * +0x14: g_MemcardImageOffsetsView4 (&g_MemcardImageOffsets[4], 20 bytes past bundle base)
 */
u8 __attribute__((aligned(16))) g_MemcardImageBundle[0x60000];
__asm__(
    ".globl g_MemcardImageOffsets\n"
    ".set g_MemcardImageOffsets, g_MemcardImageBundle + 4\n"
    ".globl g_MemcardImageOffsetsView4\n"
    ".set g_MemcardImageOffsetsView4, g_MemcardImageBundle + 20\n"
);

/* Screen-unit entry functions forwarding to the authentic fade.c implementations */
void MenuCard_InitFadeForNode(void *state);
s32  MenuCard_UpdateFadeCounter(void *state);
void MenuCard_AdvanceFade(void *state);
void MenuCard_UpdateFadeDisplay(void *state);
void MenuCard_DecreaseFade(void *state);
void MenuCard_CreateFadeImageNode(void);
void MenuCard_AttachFadePayload(void);
void MenuCard_StepFadeToImages(void *state);

void g_MenuCardInitFadeForNodeEntry(void *state) { MenuCard_InitFadeForNode(state); }
s32  g_MenuCardUpdateFadeCounterEntry(void *state) { return MenuCard_UpdateFadeCounter(state); }
void g_MenuCardAdvanceFadeEntry(void *state) { MenuCard_AdvanceFade(state); }
void g_MenuCardUpdateFadeDisplayEntry(void *state) { MenuCard_UpdateFadeDisplay(state); }
void g_MenuCardDecreaseFadeEntry(void *state) { MenuCard_DecreaseFade(state); }
void g_MenuCardCreateFadeImageNodeEntry(void) { MenuCard_CreateFadeImageNode(); }
void g_MenuCardAttachFadePayloadEntry(void) { MenuCard_AttachFadePayload(); }
void g_MenuCardStepFadeToImagesEntry(void *state) { MenuCard_StepFadeToImages(state); }

/* ── Decompiled Function Aliases & Handlers ────────────────────────────────── */

int MenuCard_RunListScreen(void);
void MenuCard_FlipScreenPages(void);

int func_801909B4(void) {
    return MenuCard_RunListScreen();
}

void func_8018F2F4(void) {
    MenuCard_FlipScreenPages();
}

void func_8019234C(void) {}
void func_801235DC(void) {}

void Boot_RunFrame(void) {
    PspInput_Poll();
    VSync(0);
}

void Draw_RenderFrame(void) {
    PspGpu_DrawOTag(g_OtBufferTable[g_ActiveDrawSlot], 0x1000);
    VSync(0);
}

void Draw_InitBuffers(void) {
    g_ActiveDrawSlot ^= 1;
}

void Draw_FlushPrimQueue(void) {}
void Draw_InitGpuHwRegs(void) {}

void Draw_PresentFrame(void) {
    VSync(0);
}

s32 Draw_RemapStatusFlags(void) {
    PspInput_Poll();
    return (s32)PspInput_GetButtons();
}

void Draw_ResetTextboxes(void) {}
void Draw_SelectBuffer(void) {}
void Draw_SetPresentEnabled(int enabled) { (void)enabled; }
void Draw_SetPresentImage(int img) { (void)img; }

int MemCard_HasAnyPresentFile(void) { return 0; }
int MemCard_IsPortPresent(s32 port) { (void)port; return 0; }
void MemCard_InitManager(void) {}
void MemCard_InitState(void) {}
void MemCard_SetDialogActive(int act) { (void)act; }
void MemCard_UpdateSavePolling(void) {}

int MenuCard_PlayVideo(int vid) {
    (void)vid;
    /* Configure 24-bit 320x240 display/draw environments matching MenuCard_SetVideoDisplay(0/1, 1) */
    SetDefDispEnv(&g_RenderDispEnvArray[0], 0, 240, 320, 240);
    g_RenderDispEnvArray[0].isrgb24 = 1;
    SetDefDrawEnv(&g_RenderDrawEnvArray[0], 0, 0, 320, 240);
    g_RenderDrawEnvArray[0].isbg = 1;
    g_RenderDrawEnvArray[0].dtd  = 1;

    SetDefDispEnv(&g_RenderDispEnvArray[1], 0, 0, 320, 240);
    g_RenderDispEnvArray[1].isrgb24 = 1;
    SetDefDrawEnv(&g_RenderDrawEnvArray[1], 0, 240, 320, 240);
    g_RenderDrawEnvArray[1].isbg = 1;
    g_RenderDrawEnvArray[1].dtd  = 1;
    return 0;
}

void Menu_ConsumeEquipSlotFlag(void) {}
void Menu_DrawTextboxEntries(void) {}
int  Menu_IsEquipSlotActive(void) { return 0; }
void Menu_OpenMemCardSelectDialog(int a0) { (void)a0; }
void Menu_PlayConfirmSound(void) {}
void Menu_PlayMoveSound(void) {}
void Menu_ResetEquipSlotState(void) {}
int  Menu_RunFrameWithArg(int a0) { (void)a0; return 0; }
void Menu_SetEquipSlotIndex(int idx) { (void)idx; }
void *MenuWidget_FindByModeAndSelectedBase(void) { return NULL; }

void Render_PrepareFrame(void) {}
void Render_ResetGteScreenOffset(void) {}
void Render_SetCDDCSlot(void) {}
void Render_SetGteScreenOffset(void) {}
void Render_SetupColorTable(int index, int mode, short *colors) {
    (void)index; (void)mode; (void)colors;
}
void Render_SetupFogLayer(void *source) {
    (void)source;
}

void Save_InitSystem(void) {}
void Save_PostInitStub(void) {}
void Save_SetTitleStyleFlag(int flag) { (void)flag; }

void Scene_LoadFieldBg(void) {}
void Scene_LoadSceneData(void) {
    /* Read real startup notice TIM (LBA 1013, sector 0 of PE.IMG) into shadow VRAM */
    static uint8_t s_notice_buf[72 * 2048];
    if (PspCdrom_ReadDiscSectors(g_PeImageBaseLba, 0, s_notice_buf, 71) == 0) {
        Asset_LoadTimImage((TimFile *)s_notice_buf);
        fprintf(stderr, "[PE-PSP] Real Startup Notice TIM loaded from PE.IMG at LBA %u into shadow VRAM.\n",
                g_PeImageBaseLba);
    }
    /* Read full menu_memcard Title Screen & Squaresoft Logo asset bundle from PE.IMG:
     * Bundle starts at PE.IMG sector 965 + 0xAA64 (sector 986 + 0x264) and spans 128 sectors (~260 KB). */
    if (PspCdrom_ReadDiscSectors(g_PeImageBaseLba + 965 + 21, 0, g_MemcardImageBundle, 130) == 0) {
        memmove(g_MemcardImageBundle, g_MemcardImageBundle + 0x264, 128 * 2048);
        memcpy(g_MemcardImagePresets, g_MemcardImageBundle + 0x3DB08, 8 * sizeof(MemcardImagePreset));
        fprintf(stderr, "[PE-PSP] Real menu_memcard Title Screen & Logo bundle loaded from PE.IMG (128 sectors, offsets[0]=%d, offsets[8]=%d).\n",
                ((int *)(g_MemcardImageBundle + 4))[0], ((int *)(g_MemcardImageBundle + 4))[8]);
    }
}

void Scene_TickTimers(void) {}
void Script_ClearSfxTable(void) {}
void Script_InitNodePool(void) {}
void CdRom_InitScreenState(void) {}
int CdRom_ReadAssetSectors(unsigned int kind, unsigned int index, int channel,
                           void *destination, u32 sectors, int mode) {
    (void)kind; (void)index; (void)channel; (void)destination; (void)sectors; (void)mode;
    return 0;
}
void CdRom_SetSeekPos(int p) { (void)p; }
void Entity_ResetAllPools(void) {}
void Entity_ResetStateGlobals(void) {}
void GameTime_InitCounter0(void) {}
void GameTime_InitCounter2(void) {}

void Akao_Cmd_24(void) {}
void Akao_Cmd_98_9A_9C(void) {}
void Akao_Cmd_99_9B_9D(void) {}
void Akao_Cmd_D8(void) {}
void Akao_Cmd_F0(void) {}
void Akao_Cmd_F1(void) {}

int * __attribute__((weak)) Asset_LoadTimImage(TimFile *tim) {
    if (!tim) return NULL;
    TimBlock *clut = NULL;
    TimBlock *image = NULL;

    if (tim->flags & 8) {
        clut = &tim->first_block;
        image = (TimBlock *)&clut->bytes[clut->header.length];
    } else {
        image = &tim->first_block;
    }

    int *pixels = image->header.pixels;
    LoadImage(&image->header.rect, pixels);
    if (clut != NULL) {
        LoadImage(&clut->header.rect, clut->header.pixels);
    }
    return pixels;
}
