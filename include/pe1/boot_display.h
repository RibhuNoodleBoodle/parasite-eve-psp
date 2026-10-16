#ifndef PE1_BOOT_DISPLAY_H
#define PE1_BOOT_DISPLAY_H

#include "common.h"
#include "pe1/asset_tim.h"
#include "pe1/cdrom_buffers.h"
#include "pe1/display_env.h"
#include "pe1/gpu_image.h"
#include "pe1/psyq_callbacks.h"
#include "pe1/psyq_cd.h"
#include "pe1/psyq_ds.h"
#include "pe1/psyq_ds_queue.h"
#include "pe1/psyq_gpu.h"

/* Three byte channels in the boot screen image buffer. */
typedef struct BootRgb24Pixel {
    u8 red, green, blue;
} BootRgb24Pixel;

PE1_STATIC_ASSERT(sizeof(BootRgb24Pixel) == 3, boot_rgb24_pixel_size);

typedef struct {
    s32 tag;
    s32 code;
} BootDisplayDrawModePrim;

typedef struct {
    s32 tag;
    u8 r0;
    u8 g0;
    u8 b0;
    u8 code;
    u16 x0;
    u16 y0;
    u8 u0;
    u8 v0;
    u16 clut;
    u16 w;
    u16 h;
} BootDisplaySpritePrim;

typedef struct {
    BootDisplayDrawModePrim draw_mode;
    BootDisplaySpritePrim sprite;
} BootDisplaySpritePacket;

/* One 16-byte record per display buffer: GPU position, pixels, and decode target. */
typedef struct {
    u16 x, y;
    char *pixels;
    u16 transferX, transferY;
    char *decodeTarget;
} BootDisplayImageBuffer;
extern BootDisplayImageBuffer g_BootDisplayImageBuffers[2];

typedef struct {
    void (*init)(void);
    void (*step)(void);
    s32 (*text)(void);
    void (*glyph)(s16, s16, u8);
    void (*transition)(u8);
    void (*present)(void);
    void (*shutdown)(void);
} BootDisplayCallbacks;
extern BootDisplayCallbacks *g_BootDisplayCallbacks;
extern u16 g_BootDisplayFontStyle;
extern const u8 g_BootDisplayUppercaseWidths[][4];
extern const u8 g_BootDisplayUppercaseWidthsStyle2[][4];
extern const u8 g_BootDisplayLowercaseWidths[][4];
extern const u8 *g_BootDisplayCreditLines[];
extern const u8 **g_BootDisplayTextCursor;

extern char *volatile g_BootDisplayOrderingTable;
extern u16 g_BootDisplayTPage;
extern u16 g_BootDisplayClut;
extern char *volatile g_BootDisplayPrimitiveCursor;
extern s32 g_BootDisplayTextScrollX;
extern s32 g_BootDisplayTextScrollStepX;
extern s32 g_BootDisplayTransition;
extern s32 g_BootDisplayBufferIndex;
extern volatile s16 g_BootDisplayStateTimer;
extern s16 g_BootDisplayTransferProgress;
extern s32 g_BootDisplayState;
extern s16 *g_BootDisplayFontImageRect;
extern s16 *g_BootDisplayFontClutRect;
extern s32 g_BootDisplayFontImagePixels;
extern s32 g_BootDisplayFontClutPixels;
extern u16 g_BootDisplayDataLbaTbl;
extern u16 g_MainAssetLbaTbl[];
extern char g_BootDisplayFontTim[];
extern volatile s32 g_BootDisplayPlaybackCurrentPos;
extern volatile s32 g_BootDisplayPlaybackEndPos;
extern s32 g_BootDisplayPlaybackStartPos;
extern s32 g_BootDisplayPlaybackEnded;
void BootDisplay_RecordReadPosition(u8 mode, int value);
extern u16 g_CdDiskType;
extern volatile u16 g_BootDisplayPlaybackRingIndex;
extern volatile u16 g_BootDisplayPlaybackAltCount;
extern char *g_BootDisplayPlaybackSectorBuffer;
extern volatile s32 g_BootDisplayPlaybackReadCount;
extern char *g_BootDisplayPlaybackFilePath;
extern s32 g_ActiveDrawSlot;

void SetSemiTrans(void *primitive, s32 code);
void ResetGraph(int value);
int VSync(int value);
void Spu_SetVoiceVolume(s32 value);
int BootDisplay_IssuePlaybackRead(int command, int parameter);
int BootDisplay_SetCdFilter(int code);
s32 BootDisplay_WaitForCdAndRestoreCallbacks(void);
int DsSync(int, void *);
s32 BootDisplay_StartPlayback(void);
void BootDisplay_CheckPlaybackTime(void);
int DsSystemStatus(void);
int DsShellOpen(void);
/* PsyQ exit: the failure path never returns to the transfer callback. */
void exit(int) __attribute__((noreturn));
s32 BootDisplay_StepPlaybackTransfer(u8 mode);
void DsInit(void);
int DsQueueLen(void);
s32 BootDisplay_InitPlaybackFile(void);
void SetGraphDebug(int value);
void SetDispMask(int value);
void DecDCTReset(s32 value);
void MdecDiag_ExpandDecoderTable(void *address);
void BootDisplay_PresentFrame(void);
void BootDisplay_SetImageDisplay(u16 index);

/* Callable entry points inside neighboring boot-display code. */
s32 BootDisplay_GetGlyphIndex(u8 character);
u16 BootDisplay_GetGlyphWidth(u8 character);
void func_8012462C(void *primitive, u16 x, u16 y, s32 u, s32 v);

#endif
