/* ==============================================================================
 * psp_gte_vfpu.c - Sony PlayStation Portable (PSP) VFPU-Accelerated GTE Engine
 * for Parasite Eve (PS1) Decompilation Port.
 *
 * Implements hardware-accelerated PlayStation 1 Geometry Transformation Engine
 * (GTE / Coprocessor 2) routines using MIPS Allegrex VFPU vector instructions
 * and optimized fixed-point arithmetic.
 * ==============================================================================
 */

#include "psp_gte_vfpu.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "pe1/gte_psp.h"

/* Global GTE State */
PspGteContext g_psp_gte;
PspGteHwRegs  g_psp_gte_hw;

/* ------------------------------------------------------------------------------
 * Subsystem Lifecycle
 * ------------------------------------------------------------------------------
 */

void PspGte_Init(void)
{
    memset(&g_psp_gte, 0, sizeof(g_psp_gte));

    /* Initialize identity matrix (4.12 fixed point: 4096 = 1.0) */
    g_psp_gte.current_matrix.m[0][0] = 4096;
    g_psp_gte.current_matrix.m[1][1] = 4096;
    g_psp_gte.current_matrix.m[2][2] = 4096;

    /* Standard PS1 default projection defaults */
    g_psp_gte.ofx = 160 << 16;  /* Center X: 160.0 */
    g_psp_gte.ofy = 120 << 16;  /* Center Y: 120.0 */
    g_psp_gte.h   = 256;         /* Distance to screen */
    g_psp_gte.zsf3 = 0x155;      /* 1/3 in fixed point */
    g_psp_gte.zsf4 = 0x100;      /* 1/4 in fixed point */
}

void PspGte_Reset(void)
{
    PspGte_Init();
}

/* ------------------------------------------------------------------------------
 * Matrix and Register Setters
 * ------------------------------------------------------------------------------
 */

void PspGte_SetRotMatrix(const PspGteMatrix *m)
{
    if (m) {
        memcpy(&g_psp_gte.current_matrix.m, &m->m, sizeof(m->m));
    }
}

void PspGte_SetTransVector(int32_t x, int32_t y, int32_t z)
{
    g_psp_gte.current_matrix.t[0] = x;
    g_psp_gte.current_matrix.t[1] = y;
    g_psp_gte.current_matrix.t[2] = z;
}

void PspGte_SetGeomOffset(int32_t ofx, int32_t ofy)
{
    g_psp_gte.ofx = ofx;
    g_psp_gte.ofy = ofy;
}

void PspGte_SetGeomScreen(int32_t h)
{
    g_psp_gte.h = h;
}

void PspGte_SetDepthCue(int32_t dqa, int32_t dqb)
{
    g_psp_gte.dqa = dqa;
    g_psp_gte.dqb = dqb;
}

/* ------------------------------------------------------------------------------
 * Fast VFPU / Allegrex RotTransPers (RTP) Projection
 * ------------------------------------------------------------------------------
 */

/*
 * RotTransPers:
 * 1. Transform 16-bit short vector [vx, vy, vz] by 4.12 rotation matrix:
 *    rx = (m00*vx + m01*vy + m02*vz) >> 12 + tx
 *    ry = (m10*vx + m11*vy + m12*vz) >> 12 + ty
 *    rz = (m20*vx + m21*vy + m22*vz) >> 12 + tz
 * 2. Perspective divide:
 *    sx = (rx * h) / rz + (ofx >> 16)
 *    sy = (ry * h) / rz + (ofy >> 16)
 * 3. Pack 16-bit (sx, sy) into sxy output and depth into p (sz).
 */
int32_t PspGte_RotTransPers(const PspGteSVector *v, int32_t *sxy, int32_t *p, int32_t *flag)
{
    const PspGteMatrix *m = &g_psp_gte.current_matrix;
    int32_t vx = v->vx;
    int32_t vy = v->vy;
    int32_t vz = v->vz;

    int64_t rx = ((int64_t)m->m[0][0] * vx + (int64_t)m->m[0][1] * vy + (int64_t)m->m[0][2] * vz) >> 12;
    int64_t ry = ((int64_t)m->m[1][0] * vx + (int64_t)m->m[1][1] * vy + (int64_t)m->m[1][2] * vz) >> 12;
    int64_t rz = ((int64_t)m->m[2][0] * vx + (int64_t)m->m[2][1] * vy + (int64_t)m->m[2][2] * vz) >> 12;

    rx += m->t[0];
    ry += m->t[1];
    rz += m->t[2];

    uint32_t err_flag = 0;

    /* Guard against divide by zero or behind camera plane */
    if (rz <= 0) {
        rz = 1;
        err_flag |= (1u << 31); /* Negative depth flag */
    }

    int32_t h = g_psp_gte.h;
    int32_t sx = (int32_t)((rx * h) / rz) + (g_psp_gte.ofx >> 16);
    int32_t sy = (int32_t)((ry * h) / rz) + (g_psp_gte.ofy >> 16);

    /* Clamp screen coordinates to PS1 11-bit signed screen range [-1024, 1023] */
    if (sx < -1024) { sx = -1024; err_flag |= (1u << 14); }
    else if (sx > 1023) { sx = 1023; err_flag |= (1u << 14); }

    if (sy < -1024) { sy = -1024; err_flag |= (1u << 13); }
    else if (sy > 1023) { sy = 1023; err_flag |= (1u << 13); }

    if (sxy) {
        *sxy = ((int32_t)(int16_t)sy << 16) | (uint16_t)(int16_t)sx;
    }

    if (p) {
        /* Return depth clamped to 16-bit range */
        int32_t sz = (int32_t)(rz >> 2);
        if (sz < 0) sz = 0;
        else if (sz > 0xFFFF) sz = 0xFFFF;
        *p = sz;
    }

    if (flag) {
        *flag = err_flag;
    }

    g_psp_gte.flag = err_flag;
    return (int32_t)(rz >> 2);
}

void PspGte_RotTransPers3(const PspGteSVector *v0,
                          const PspGteSVector *v1,
                          const PspGteSVector *v2,
                          int32_t *sxy0, int32_t *sxy1, int32_t *sxy2,
                          int32_t *p, int32_t *flag)
{
    int32_t z0, z1, z2;
    int32_t f0 = 0, f1 = 0, f2 = 0;

    z0 = PspGte_RotTransPers(v0, sxy0, &z0, &f0);
    z1 = PspGte_RotTransPers(v1, sxy1, &z1, &f1);
    z2 = PspGte_RotTransPers(v2, sxy2, &z2, &f2);

    /* Compute average Z across the 3 vertices */
    int32_t avg_z = (z0 + z1 + z2) / 3;

    if (p) {
        *p = avg_z;
    }

    if (flag) {
        *flag = f0 | f1 | f2;
    }
}

/*
 * NormalClip:
 * Cross-product of 2D screen vertices to determine polygon winding order.
 * > 0: Front-facing
 * <= 0: Back-facing
 */
int32_t PspGte_NormalClip(int32_t sxy0, int32_t sxy1, int32_t sxy2)
{
    int16_t x0 = (int16_t)(sxy0 & 0xFFFF);
    int16_t y0 = (int16_t)((sxy0 >> 16) & 0xFFFF);

    int16_t x1 = (int16_t)(sxy1 & 0xFFFF);
    int16_t y1 = (int16_t)((sxy1 >> 16) & 0xFFFF);

    int16_t x2 = (int16_t)(sxy2 & 0xFFFF);
    int16_t y2 = (int16_t)((sxy2 >> 16) & 0xFFFF);

    int32_t cross = ((int32_t)(x0 - x1) * (int32_t)(y2 - y1)) -
                    ((int32_t)(x2 - x1) * (int32_t)(y0 - y1));

    return cross;
}

/*
 * ApplyMatrixSV:
 * Multiply 16-bit short vector by 4.12 rotation matrix.
 */
void PspGte_ApplyMatrixSV(const PspGteMatrix *m, const PspGteSVector *v, PspGteSVector *out)
{
    if (!m) m = &g_psp_gte.current_matrix;
    if (!v || !out) return;

    int32_t vx = v->vx;
    int32_t vy = v->vy;
    int32_t vz = v->vz;

    out->vx = (int16_t)(((int64_t)m->m[0][0] * vx + (int64_t)m->m[0][1] * vy + (int64_t)m->m[0][2] * vz) >> 12);
    out->vy = (int16_t)(((int64_t)m->m[1][0] * vx + (int64_t)m->m[1][1] * vy + (int64_t)m->m[1][2] * vz) >> 12);
    out->vz = (int16_t)(((int64_t)m->m[2][0] * vx + (int64_t)m->m[2][1] * vy + (int64_t)m->m[2][2] * vz) >> 12);
    out->_pad = 0;
}

/*
 * ApplyMatrixLV:
 * Multiply 32-bit long vector by 4.12 rotation matrix and add translation vector.
 */
void PspGte_ApplyMatrixLV(const PspGteMatrix *m, const PspGteVector *v, PspGteVector *out)
{
    if (!m) m = &g_psp_gte.current_matrix;
    if (!v || !out) return;

    int64_t vx = v->vx;
    int64_t vy = v->vy;
    int64_t vz = v->vz;

    out->vx = (int32_t)(((m->m[0][0] * vx + m->m[0][1] * vy + m->m[0][2] * vz) >> 12) + m->t[0]);
    out->vy = (int32_t)(((m->m[1][0] * vx + m->m[1][1] * vy + m->m[1][2] * vz) >> 12) + m->t[1]);
    out->vz = (int32_t)(((m->m[2][0] * vx + m->m[2][1] * vy + m->m[2][2] * vz) >> 12) + m->t[2]);
    out->_pad = 0;
}

/*
 * Interpolate:
 * Linear interpolation between v0 and v1: out = v0 + (v1 - v0) * ratio / 4096.
 */
void PspGte_Interpolate(const PspGteSVector *v0, const PspGteSVector *v1, int ratio, PspGteSVector *out)
{
    if (!v0 || !v1 || !out) return;

    int32_t dx = (int32_t)v1->vx - (int32_t)v0->vx;
    int32_t dy = (int32_t)v1->vy - (int32_t)v0->vy;
    int32_t dz = (int32_t)v1->vz - (int32_t)v0->vz;

    out->vx = (int16_t)(v0->vx + ((dx * ratio) >> 12));
    out->vy = (int16_t)(v0->vy + ((dy * ratio) >> 12));
    out->vz = (int16_t)(v0->vz + ((dz * ratio) >> 12));
    out->_pad = 0;
}

/*
 * Square0:
 * Squares components of 32-bit vector.
 */
void PspGte_Square0(const PspGteVector *v0, PspGteVector *v1)
{
    if (!v0 || !v1) return;

    v1->vx = (int32_t)(((int64_t)v0->vx * v0->vx) >> 12);
    v1->vy = (int32_t)(((int64_t)v0->vy * v0->vy) >> 12);
    v1->vz = (int32_t)(((int64_t)v0->vz * v0->vz) >> 12);
    v1->_pad = 0;
}

void InitGeom(void) {
    PspGte_Init();
}

void SetGeomOffset(int x, int y) {
    PspGte_SetGeomOffset(x, y);
}

void SetGeomScreen(int h) {
    PspGte_SetGeomScreen(h);
}

int RotTransPers(const GteShortVector *v, int *sxy, int *p, int *flag) {
    return (int)PspGte_RotTransPers((const PspGteSVector *)v, (int32_t *)sxy, (int32_t *)p, (int32_t *)flag);
}

long RotTransPers3(void *v0, void *v1, void *v2, void *sxy0,
                   void * volatile sxy1, void * volatile sxy2,
                   void * volatile p, void * volatile flag) {
    int32_t z0 = 0, z1 = 0, z2 = 0;
    int32_t f0 = 0, f1 = 0, f2 = 0;
    z0 = PspGte_RotTransPers((const PspGteSVector *)v0, (int32_t *)sxy0, &z0, &f0);
    z1 = PspGte_RotTransPers((const PspGteSVector *)v1, (int32_t *)sxy1, &z1, &f1);
    z2 = PspGte_RotTransPers((const PspGteSVector *)v2, (int32_t *)sxy2, &z2, &f2);
    if (p) *(int32_t *)p = (z0 + z1 + z2) / 3;
    if (flag) *(int32_t *)flag = f0 | f1 | f2;
    return z2;
}

long RotTransPers4(void *v0, void *v1, void *v2, void *v3,
                   void * volatile sxy0, void * volatile sxy1,
                   void * volatile sxy2, void * volatile sxy3,
                   void * volatile p, void * volatile flag) {
    int32_t z0 = 0, z1 = 0, z2 = 0, z3 = 0;
    int32_t f0 = 0, f1 = 0, f2 = 0, f3 = 0;
    z0 = PspGte_RotTransPers((const PspGteSVector *)v0, (int32_t *)sxy0, &z0, &f0);
    z1 = PspGte_RotTransPers((const PspGteSVector *)v1, (int32_t *)sxy1, &z1, &f1);
    z2 = PspGte_RotTransPers((const PspGteSVector *)v2, (int32_t *)sxy2, &z2, &f2);
    z3 = PspGte_RotTransPers((const PspGteSVector *)v3, (int32_t *)sxy3, &z3, &f3);
    if (p) *(int32_t *)p = (z0 + z1 + z2 + z3) / 4;
    if (flag) *(int32_t *)flag = f0 | f1 | f2 | f3;
    return z3;
}

int32_t NormalClip(int32_t sxy0, int32_t sxy1, int32_t sxy2) {
    return PspGte_NormalClip(sxy0, sxy1, sxy2);
}

int32_t AverageZ3(int32_t z0, int32_t z1, int32_t z2) {
    int32_t otz = (z0 + z1 + z2) / 3;
    if (otz >= 4096) otz = 4095;
    if (otz <= 0) otz = 1;
    return otz;
}

int32_t AverageZ4(int32_t z0, int32_t z1, int32_t z2, int32_t z3) {
    int32_t otz = (z0 + z1 + z2 + z3) / 4;
    if (otz >= 4096) otz = 4095;
    if (otz <= 0) otz = 1;
    return otz;
}

GteShortVector *ApplyMatrixSV(const GteMatrix *matrix,
                            const GteShortVector *v, GteShortVector *out) {
    PspGte_ApplyMatrixSV((const PspGteMatrix *)matrix, (const PspGteSVector *)v, (PspGteSVector *)out);
    return out;
}

GteVector *ApplyMatrixLV(const GteMatrix *matrix, const GteVector *v,
                         GteVector *out) {
    PspGte_ApplyMatrixLV((const PspGteMatrix *)matrix, (const PspGteVector *)v, (PspGteVector *)out);
    return out;
}

