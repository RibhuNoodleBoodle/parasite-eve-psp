#ifndef PE1_GPU_STATE_H
#define PE1_GPU_STATE_H

#include "pe1/gpu_callbacks.h"
#include "pe1/psyq_callbacks.h"
#include "pe1/psyq_gpu.h"

/*
 * libgpu/sys.c state immediately following the GPU dispatch and debug-print
 * callbacks.  The byte offsets are observed by SetGraph*, checkRECT, and
 * DrawSyncCallback; the DRAWENV cache starts immediately after this record.
 */
typedef struct GpuQueueState {
    /* 0x00 */ unsigned char queue;
    /* 0x01 */ unsigned char debugLevel;
} GpuQueueState;

typedef struct GpuDebugState {
    /* 0x00 */ unsigned char type;
    /* 0x01 */ GpuQueueState queueState;
    /* 0x03 */ unsigned char reverse;
    /* 0x04 */ short width;
    /* 0x06 */ short height;
    /* 0x08 */ unsigned int syncCallbackPending;
    /* 0x0C */ void (*drawSyncCallback)();
} GpuDebugState;

#define PE1_GPU_STATE_STATIC_ASSERT(expr, name) \
    typedef char pe1_gpu_state_assert_##name[(expr) ? 1 : -1]
#ifndef PE1_GPU_STATE_OFFSETOF
#if defined(__GNUC__) && (__GNUC__ >= 4)
#define PE1_GPU_STATE_OFFSETOF(type, member) __builtin_offsetof(type, member)
#else
#define PE1_GPU_STATE_OFFSETOF(type, member) ((unsigned int)&(((type *)0)->member))
#endif
#endif

/* Complete SYS.OBJ state, including its cached drawing/display settings. */
typedef struct GpuSystemState {
    GpuDebugState status;
    DRAWENV drawCache;
    DISPENV displayCache;
} GpuSystemState;

PE1_GPU_STATE_STATIC_ASSERT(PE1_GPU_STATE_OFFSETOF(GpuSystemState, drawCache) == 0x10, gpu_draw_cache_offset);
PE1_GPU_STATE_STATIC_ASSERT(PE1_GPU_STATE_OFFSETOF(GpuSystemState, displayCache) == 0x6C, gpu_display_cache_offset);
PE1_GPU_STATE_STATIC_ASSERT(sizeof(GpuSystemState) == 0x80, gpu_system_state_size);

#undef PE1_GPU_STATE_OFFSETOF
#undef PE1_GPU_STATE_STATIC_ASSERT

extern GpuCallbacks *g_GpuCallbacks;
extern GpuDebugPrintf g_GpuDebugPrintf;
extern GpuDebugState g_GpuSystemState;
/* Debug level byte inside the SYS.OBJ state; PutDrawEnv and friends read
 * the state through a byte pointer. */
extern unsigned char g_GraphDebug[];
extern char g_SetGraphQueFormat[];

/* SYS.OBJ display cache (GpuSystemState.displayCache) as its own symbol;
 * GetDrawEnv reads the draw cache through the whole GpuSystemState. */
extern DISPENV g_GpuActiveDispEnv;

#endif /* PE1_GPU_STATE_H */
