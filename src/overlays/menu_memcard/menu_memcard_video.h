#ifndef MENU_MEMCARD_VIDEO_H
#define MENU_MEMCARD_VIDEO_H

#include "common.h"
#include "pe1/cdrom.h"
#include "pe1/cdrom_buffers.h"
#include "pe1/disc_load_state.h"
#include "pe1/display_env.h"
#include "pe1/draw_state.h"
#include "pe1/game_state.h"
#include "pe1/gpu_image.h"
#include "pe1/psyq_bios.h"
#include "pe1/psyq_cd.h"
#include "pe1/psyq_ds.h"
#include "pe1/psyq_gpu.h"
#include "pe1/psyq_press.h"
#include "pe1/script_state.h"

typedef struct VideoFrame { u32 reserved[2], time, reserved0c; u16 width, height; } VideoFrame;
/* One entry of a video table: file name, colour depth and playback layout. */
typedef struct VideoEntry {
    char *name;            /* 0x00 file name appended to the directory */
    u8 wide;               /* 0x04 24-bit frames */
    s16 speed;             /* 0x06 */
    s16 end;               /* 0x08 last frame */
    u16 x, y;              /* 0x0A display origin */
    u8 reserved0e[6];
} VideoEntry;
typedef struct VideoRect { s16 x,y,w,h; } VideoRect;
/* Playback state of one video player copy. */
typedef struct VideoDisplay {
    u8 *decode[2];        /* 0x00 MDEC decode buffers */
    u8 decodeIndex;       /* 0x08 */
    u8 *buffers[2];       /* 0x0C slice upload buffers */
    u8 selector;          /* 0x14 */
    VideoRect regions[2]; /* 0x16 display areas of the two frame buffers */
    u8 region;            /* 0x26 */
    VideoRect rect;       /* 0x28 next slice */
    u8 done;              /* 0x30 frame fully uploaded */
} VideoDisplay;
extern s32 DsSystemStatus(void),DsShellOpen(void);
extern void exit(s32),Spu_SetVoiceVolume(s32);
extern VideoEntry *g_MenuCardScreenVideoEntry;
extern u8 g_MenuCardScreenVideoFinished;
/* Wide-screen flag of the playing movie (Pe1SceneAudioState movie_wide). */
extern s8 g_MovieWide;
/* g_SeqElapsed as a one-field record: MENUCARD_OPEN_VIDEO_NAME sets it with
 * its own address load, next to g_MovieWide and g_MovieIndex. */
typedef struct VideoFrameCount { s16 value; } VideoFrameCount;
extern VideoFrameCount g_SeqElapsedRecord;
extern s16 g_MenuCardScreenVideoLastTime,g_MenuCardScreenVideoSize[2];

void *MenuCard_UpdateVideoFrame(VideoDisplay *display);
void *MenuCardFirst_UpdateVideoFrame(VideoDisplay *display);

/* State of the first video player copy (linked at 0x80120D00). */
extern VideoEntry *g_MenuCardVideoEntry;
extern s16 g_MenuCardVideoLastTime, g_MenuCardVideoSize[2];
extern u8 g_MenuCardVideoFinished;

extern u8 g_MenuCardScreenVideoDepthFlip;
extern s32 g_ActiveDrawSlot;
extern void StCdInterrupt(void),DecDCTout(void *,s32),MenuCard_SetVideoDisplay(s8,s8);

void MenuCard_UploadVideoSlice(void);

extern DISPENV g_MenuCardScreenSavedDispEnv[2];
extern DRAWENV g_MenuCardScreenSavedDrawEnv[2];
extern u8 *g_MenuCardScreenVideoDecodeBuffer0,*g_MenuCardScreenVideoDecodeBuffer1,*g_MenuCardScreenVideoSliceBuffer0,*g_MenuCardScreenVideoSliceBuffer1,*g_MenuCardScreenVideoVlcTable,*g_MenuCardScreenVideoStreamRing;

s32 MenuCard_InitVideoBuffers(s8 count, u8 **buffers);
s32 MenuCardFirst_InitVideoBuffers(s8 count, u8 **buffers);
/* Buffers and saved environments of the first video player copy. */
extern u8 *g_MenuCardVideoDecodeBuffer0, *g_MenuCardVideoDecodeBuffer1, *g_MenuCardVideoSliceBuffer0, *g_MenuCardVideoSliceBuffer1, *g_MenuCardVideoVlcTable, *g_MenuCardVideoStreamRing;
extern DISPENV g_MenuCardVideoSavedDispEnv[2];
extern DRAWENV g_MenuCardVideoSavedDrawEnv[2];

typedef struct VideoDiscRange { u16 start,end; } VideoDiscRange;
extern VideoEntry g_MenuCardScreenVideoTable[];
extern u32 g_FieldPadBits;
s32 MenuCard_StepVideo(void);
extern void SetDispMask(s32), ResetGraph(s32), Field_HandleStateTransition(void), StUnSetRing(void), Draw_RenderFrame(void);

s32 MenuCard_PlayVideo(s32 index);

void MenuCard_SetVideoDisplay(s8 index, s8 wide);
void MenuCardFirst_SetVideoDisplay(s8 index, s8 wide);

/* Video open: file lookup, stream start and first decoded frame. */
extern u8 g_MovieIndex;
extern VideoEntry g_MenuCardVideoTable[];
extern DslFILE g_MenuCardVideoFile, g_MenuCardScreenVideoFile;
extern CdlLOC g_MenuCardVideoRestartLoc, g_MenuCardScreenVideoRestartLoc;
extern VideoDisplay g_MenuCardVideoDisplay, g_MenuCardScreenVideoDisplay;
extern char g_MenuCardVideoDirectory[], g_MenuCardVideoDirectoryLate[], g_MenuCardScreenVideoDirectory[], g_MenuCardScreenVideoDirectoryLate[];
#if defined(__psp__) || defined(ENABLE_PSP)
extern void StSetRing(void *, s32);
#else
extern void strcat(char *, char *), StSetRing(void *, s32);
#endif
extern void DecDCTvlc2(void *, void *, void *), MenuCardFirst_UploadVideoSlice(void);
extern s32 DsRead2(CdlLOC *, s32);
s32 MenuCardFirst_OpenVideo(s32 number);
s32 MenuCard_OpenVideo(s32 number);

/* Per-frame video step: decode, restart the stream on a stall, page flip. */
extern u8 g_MenuCardVideoDecodeMode, g_MenuCardVideoDepthFlip, g_MenuCardScreenVideoDecodeMode;
extern void DecDCTin(void *, s32);
s32 MenuCardFirst_StepVideo(void);

#endif
