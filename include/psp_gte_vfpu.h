/* ==============================================================================
 * psp_gte_vfpu.h - Sony PlayStation Portable (PSP) VFPU-Accelerated GTE Engine
 * for Parasite Eve (PS1) Decompilation Port.
 *
 * Implements hardware-accelerated PlayStation 1 Geometry Transformation Engine
 * (GTE / Coprocessor 2) routines using MIPS Allegrex VFPU vector instructions
 * and sceGum matrix math.
 * ==============================================================================
 */

#ifndef PSP_GTE_VFPU_H
#define PSP_GTE_VFPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PS1 GTE fixed-point data structures */
typedef struct PspGteMatrix {
    int16_t m[3][3];    /* 4.12 fixed point rotation matrix */
    int16_t _pad;
    int32_t t[3];       /* 32-bit translation vector */
} PspGteMatrix;

typedef struct PspGteSVector {
    int16_t vx;
    int16_t vy;
    int16_t vz;
    int16_t _pad;
} PspGteSVector;

typedef struct PspGteVector {
    int32_t vx;
    int32_t vy;
    int32_t vz;
    int32_t _pad;
} PspGteVector;

/* GTE Runtime State */
typedef struct PspGteContext {
    PspGteMatrix current_matrix;
    int32_t      ofx;       /* Screen offset X (16.16) */
    int32_t      ofy;       /* Screen offset Y (16.16) */
    int32_t      h;         /* Projection plane distance (viewport distance) */
    int32_t      dqa;       /* Depth cue coefficient */
    int32_t      dqb;       /* Depth cue offset */
    int32_t      zsf3;      /* Average-Z scale factor (3 terms) */
    int32_t      zsf4;      /* Average-Z scale factor (4 terms) */
    uint32_t     flag;      /* GTE status / error flags */
    int32_t      mac0;      /* GTE MAC0 accumulator */
    int32_t      ir0;       /* GTE IR0 interpolation register */
} PspGteContext;

extern PspGteContext g_psp_gte;

/* Subsystem Lifecycle */
void PspGte_Init(void);
void PspGte_Reset(void);

/* Register & Matrix Setup */
void PspGte_SetRotMatrix(const PspGteMatrix *m);
void PspGte_SetTransVector(int32_t x, int32_t y, int32_t z);
void PspGte_SetGeomOffset(int32_t ofx, int32_t ofy);
void PspGte_SetGeomScreen(int32_t h);
void PspGte_SetDepthCue(int32_t dqa, int32_t dqb);

/* Accelerated Geometry Transformation Routines */
int32_t PspGte_RotTransPers(const PspGteSVector *v, int32_t *sxy, int32_t *p, int32_t *flag);

void PspGte_RotTransPers3(const PspGteSVector *v0,
                          const PspGteSVector *v1,
                          const PspGteSVector *v2,
                          int32_t *sxy0, int32_t *sxy1, int32_t *sxy2,
                          int32_t *p, int32_t *flag);

int32_t PspGte_NormalClip(int32_t sxy0, int32_t sxy1, int32_t sxy2);

void PspGte_ApplyMatrixSV(const PspGteMatrix *m, const PspGteSVector *v, PspGteSVector *out);
void PspGte_ApplyMatrixLV(const PspGteMatrix *m, const PspGteVector *v, PspGteVector *out);

void PspGte_Interpolate(const PspGteSVector *v0, const PspGteSVector *v1, int ratio, PspGteSVector *out);
void PspGte_Square0(const PspGteVector *v0, PspGteVector *v1);

#ifdef __cplusplus
}
#endif

#endif /* PSP_GTE_VFPU_H */
