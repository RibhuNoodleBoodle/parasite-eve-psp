/* ==============================================================================
 * gte_psp.h - Sony PlayStation Portable (PSP) MIPS Allegrex GTE Compatibility
 * for Parasite Eve (PS1) Decompilation Port.
 *
 * Emulates PS1 Coprocessor 2 (GTE) operations on PSP MIPS Allegrex without
 * PS1-specific COP2 assembly instructions (lwc2, swc2, cfc2, ctc2, mfc2, mtc2).
 * ==============================================================================
 */

#ifndef PE1_GTE_PSP_H
#define PE1_GTE_PSP_H

#include "pe1/gte_types.h"
#include "psp_gte_vfpu.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PspGteHwRegs {
    int16_t  v0[4];
    int16_t  v1[4];
    int16_t  v2[4];
    int32_t  ir[4];
    int32_t  mac[4];
    int32_t  mac0;
    int32_t  ir0;
    uint32_t sxy[4];
    uint32_t sz[4];
    uint32_t otz;
    uint32_t rgb[4];
    uint32_t flag;
    uint32_t lzcs;
    uint32_t lzcr;
    int16_t  rot[3][3];
    int32_t  trans[3];
    int16_t  light[3][3];
    int16_t  color[3][3];
    int32_t  bk[3];
    int32_t  ofx, ofy, h;
    int32_t  dqa, dqb;
} PspGteHwRegs;

extern PspGteHwRegs g_psp_gte_hw;

/* Load / Store Helpers */
static inline void psp_gte_ldv0(const void *p) {
    const int16_t *s = (const int16_t *)p;
    g_psp_gte_hw.v0[0] = s[0];
    g_psp_gte_hw.v0[1] = s[1];
    g_psp_gte_hw.v0[2] = s[2];
}

static inline void psp_gte_ldv1(const void *p) {
    const int16_t *s = (const int16_t *)p;
    g_psp_gte_hw.v1[0] = s[0];
    g_psp_gte_hw.v1[1] = s[1];
    g_psp_gte_hw.v1[2] = s[2];
}

static inline void psp_gte_ldv2(const void *p) {
    const int16_t *s = (const int16_t *)p;
    g_psp_gte_hw.v2[0] = s[0];
    g_psp_gte_hw.v2[1] = s[1];
    g_psp_gte_hw.v2[2] = s[2];
}

static inline void psp_gte_stmac(void *p) {
    int32_t *d = (int32_t *)p;
    d[0] = g_psp_gte_hw.mac[0];
    d[1] = g_psp_gte_hw.mac[1];
    d[2] = g_psp_gte_hw.mac[2];
}

static inline void psp_gte_ldrotmatrix(const void *p) {
    memcpy(g_psp_gte_hw.rot, p, sizeof(g_psp_gte_hw.rot));
    PspGte_SetRotMatrix((const PspGteMatrix *)p);
}

static inline void psp_gte_ldtransmatrix(const void *p) {
    const int32_t *t = (const int32_t *)p;
    g_psp_gte_hw.trans[0] = t[0];
    g_psp_gte_hw.trans[1] = t[1];
    g_psp_gte_hw.trans[2] = t[2];
    PspGte_SetTransVector(t[0], t[1], t[2]);
}

static inline void psp_gte_mvmva_impl(void) {
    int32_t vx = g_psp_gte_hw.v0[0];
    int32_t vy = g_psp_gte_hw.v0[1];
    int32_t vz = g_psp_gte_hw.v0[2];

    g_psp_gte_hw.mac[0] = (int32_t)(((int64_t)g_psp_gte_hw.rot[0][0] * vx +
                                     (int64_t)g_psp_gte_hw.rot[0][1] * vy +
                                     (int64_t)g_psp_gte_hw.rot[0][2] * vz) >> 12);
    g_psp_gte_hw.mac[1] = (int32_t)(((int64_t)g_psp_gte_hw.rot[1][0] * vx +
                                     (int64_t)g_psp_gte_hw.rot[1][1] * vy +
                                     (int64_t)g_psp_gte_hw.rot[1][2] * vz) >> 12);
    g_psp_gte_hw.mac[2] = (int32_t)(((int64_t)g_psp_gte_hw.rot[2][0] * vx +
                                     (int64_t)g_psp_gte_hw.rot[2][1] * vy +
                                     (int64_t)g_psp_gte_hw.rot[2][2] * vz) >> 12);
}

static inline void psp_gte_rtps_impl(void) {
    PspGteSVector sv;
    sv.vx = g_psp_gte_hw.v0[0];
    sv.vy = g_psp_gte_hw.v0[1];
    sv.vz = g_psp_gte_hw.v0[2];
    sv._pad = 0;
    int32_t sxy, p, f;
    PspGte_RotTransPers(&sv, &sxy, &p, &f);
    g_psp_gte_hw.sxy[0] = (uint32_t)sxy;
    g_psp_gte_hw.sz[0]  = g_psp_gte_hw.sz[3];
    uint32_t sz = (uint32_t)(p > 0 ? p : 1);
    g_psp_gte_hw.sz[3]  = sz;
    uint32_t otz = (sz < 4096) ? sz : 4095;
    g_psp_gte_hw.otz    = otz;
    g_psp_gte_hw.mac0   = (int32_t)otz;
    g_psp_gte_hw.flag   = (uint32_t)f;
}

static inline void psp_gte_rtpt_impl(void) {
    PspGteSVector v0 = { g_psp_gte_hw.v0[0], g_psp_gte_hw.v0[1], g_psp_gte_hw.v0[2], 0 };
    PspGteSVector v1 = { g_psp_gte_hw.v1[0], g_psp_gte_hw.v1[1], g_psp_gte_hw.v1[2], 0 };
    PspGteSVector v2 = { g_psp_gte_hw.v2[0], g_psp_gte_hw.v2[1], g_psp_gte_hw.v2[2], 0 };
    int32_t sxy0, sxy1, sxy2, p, f;
    PspGte_RotTransPers3(&v0, &v1, &v2, &sxy0, &sxy1, &sxy2, &p, &f);
    g_psp_gte_hw.sxy[0] = (uint32_t)sxy0;
    g_psp_gte_hw.sxy[1] = (uint32_t)sxy1;
    g_psp_gte_hw.sxy[2] = (uint32_t)sxy2;
    g_psp_gte_hw.sz[0]  = g_psp_gte_hw.sz[3];
    uint32_t sz = (uint32_t)(p > 0 ? p : 1);
    g_psp_gte_hw.sz[1]  = sz;
    g_psp_gte_hw.sz[2]  = sz;
    g_psp_gte_hw.sz[3]  = sz;
    uint32_t otz = (sz < 4096) ? sz : 4095;
    g_psp_gte_hw.otz    = otz;
    g_psp_gte_hw.mac0   = (int32_t)otz;
    g_psp_gte_hw.flag   = (uint32_t)f;
}

static inline void psp_gte_avsz3_impl(void) {
    uint32_t sum = g_psp_gte_hw.sz[1] + g_psp_gte_hw.sz[2] + g_psp_gte_hw.sz[3];
    uint32_t otz = sum / 3;
    if (otz >= 4096) otz = 4095;
    if (otz == 0) otz = 1;
    g_psp_gte_hw.otz = otz;
    g_psp_gte_hw.mac0 = (int32_t)otz;
}

static inline void psp_gte_avsz4_impl(void) {
    uint32_t sum = g_psp_gte_hw.sz[0] + g_psp_gte_hw.sz[1] + g_psp_gte_hw.sz[2] + g_psp_gte_hw.sz[3];
    uint32_t otz = sum / 4;
    if (otz >= 4096) otz = 4095;
    if (otz == 0) otz = 1;
    g_psp_gte_hw.otz = otz;
    g_psp_gte_hw.mac0 = (int32_t)otz;
}

static inline int32_t psp_gte_nclip_impl(void) {
    return PspGte_NormalClip(g_psp_gte_hw.sxy[0], g_psp_gte_hw.sxy[1], g_psp_gte_hw.sxy[2]);
}

/* Macro Mappings */
#define GTE_INSN_LWC2_0_0 ""
#define GTE_INSN_LWC2_1_4 ""

#define gte_lwc2_0_0(ptr) psp_gte_ldv0(ptr)
#define gte_lwc2_1_4(ptr) do {} while(0)
#define gte_lwc2_2_0(p)   psp_gte_ldv1(p)
#define gte_lwc2_3_4(p)   do {} while(0)
#define gte_lwc2_4_0(p)   psp_gte_ldv2(p)
#define gte_lwc2_5_4(p)   do {} while(0)

#define gte_swc2_11_16(ptr) do {} while(0)
#define gte_swc2_25_0(ptr)  do { ((int32_t*)(ptr))[0] = g_psp_gte_hw.mac[0]; } while(0)
#define gte_swc2_26_4(ptr)  do { ((int32_t*)(ptr))[0] = g_psp_gte_hw.mac[1]; } while(0)
#define gte_swc2_27_8(ptr)  do { ((int32_t*)(ptr))[0] = g_psp_gte_hw.mac[2]; } while(0)

#define gte_swc2_9_0(ptr)   do {} while(0)
#define gte_swc2_10_4(ptr)  do {} while(0)
#define gte_swc2_11_8(ptr)  do {} while(0)

#define gte_mfc2_25(val)    do { (val) = g_psp_gte_hw.mac[0]; } while(0)
#define gte_mfc2_26(val)    do { (val) = g_psp_gte_hw.mac[1]; } while(0)
#define gte_mfc2_27(val)    do { (val) = g_psp_gte_hw.mac[2]; } while(0)
#define gte_mfc2_9(val)     do { (val) = g_psp_gte_hw.ir[0]; } while(0)
#define gte_mfc2_10(val)    do { (val) = g_psp_gte_hw.ir[1]; } while(0)
#define gte_mfc2_11(val)    do { (val) = g_psp_gte_hw.ir[2]; } while(0)

#define gte_mtc2_0(v)       do {} while(0)
#define gte_mtc2_1(v)       do {} while(0)
#define gte_mtc2_9(v)       do { g_psp_gte_hw.ir[0] = (v); } while(0)
#define gte_mtc2_10(v)      do { g_psp_gte_hw.ir[1] = (v); } while(0)
#define gte_mtc2_11(v)      do { g_psp_gte_hw.ir[2] = (v); } while(0)

#define gte_ctc2_0(v)       do {} while(0)
#define gte_ctc2_1(v)       do {} while(0)
#define gte_ctc2_2(v)       do {} while(0)
#define gte_ctc2_3(v)       do {} while(0)
#define gte_ctc2_4(v)       do {} while(0)
#define gte_ctc2_5(v)       do {} while(0)
#define gte_ctc2_6(v)       do {} while(0)
#define gte_ctc2_7(v)       do {} while(0)
#define gte_ctc2_8(v)       do {} while(0)
#define gte_ctc2_9(v)       do {} while(0)
#define gte_ctc2_10(v)      do {} while(0)
#define gte_ctc2_11(v)      do {} while(0)
#define gte_ctc2_12(v)      do {} while(0)
#define gte_ctc2_16(v)      do {} while(0)
#define gte_ctc2_17(v)      do {} while(0)
#define gte_ctc2_18(v)      do {} while(0)
#define gte_ctc2_19(v)      do {} while(0)
#define gte_ctc2_20(v)      do {} while(0)
#define gte_ctc2_26(v)      do { g_psp_gte_hw.h = (v); PspGte_SetGeomScreen(v); } while(0)

#define gte_rtpt_command()                     psp_gte_rtpt_impl()
#define gte_rtps_command()                     psp_gte_rtps_impl()
#define gte_avsz3_command()                    psp_gte_avsz3_impl()
#define gte_avsz4_command()                    psp_gte_avsz4_impl()
#define gte_mvmva_rotation_v0_sf12()           psp_gte_mvmva_impl()
#define gte_mvmva_rotation_v0_translation_sf12() psp_gte_mvmva_impl()

#define gte_ldv0(ptr)                          psp_gte_ldv0(ptr)
#define gte_ldv1(ptr)                          psp_gte_ldv1(ptr)
#define gte_ldv2(ptr)                          psp_gte_ldv2(ptr)
#define gte_ldrotmatrix(ptr)                   psp_gte_ldrotmatrix(ptr)
#define gte_ldtransmatrix(ptr)                 psp_gte_ldtransmatrix(ptr)
#define gte_mvmva()                            psp_gte_mvmva_impl()
#define gte_stmac(ptr)                         psp_gte_stmac(ptr)
#define gte_stmac0(ptr)                        do { *(int32_t*)(ptr) = g_psp_gte_hw.mac0; } while(0)
#define gte_stotz(ptr)                         do { *(uint32_t*)(ptr) = g_psp_gte_hw.otz; } while(0)
#define gte_stotz_precise(ptr)                 do { *(uint32_t*)(ptr) = g_psp_gte_hw.otz; } while(0)

#define gte_rtps()                             psp_gte_rtps_impl()
#define gte_rtpt()                             psp_gte_rtpt_impl()
#define gte_rtpt_padded()                      psp_gte_rtpt_impl()
#define gte_rtps_three_outputs()               psp_gte_rtps_impl()
#define gte_rtpt_three_outputs()               psp_gte_rtpt_impl()

#define gte_nclip()                            psp_gte_nclip_impl()
#define gte_nclip_now()                        psp_gte_nclip_impl()

#define gte_stsxy0_precise(ptr)                do { *(uint32_t*)(ptr) = g_psp_gte_hw.sxy[0]; } while(0)
#define gte_stsxy1_precise(ptr)                do { *(uint32_t*)(ptr) = g_psp_gte_hw.sxy[1]; } while(0)
#define gte_stsxy2_precise(ptr)                do { *(uint32_t*)(ptr) = g_psp_gte_hw.sxy[2]; } while(0)
#define gte_stsxy2(ptr)                        do { *(uint32_t*)(ptr) = g_psp_gte_hw.sxy[2]; } while(0)
#define gte_stsxy2_mem(ptr)                    do { *(uint32_t*)(ptr) = g_psp_gte_hw.sxy[2]; } while(0)
#define gte_stsxy1_precise_keep_address(ptr)   do { *(uint32_t*)(ptr) = g_psp_gte_hw.sxy[1]; } while(0)
#define gte_stsxy2_precise_keep_address(ptr)   do { *(uint32_t*)(ptr) = g_psp_gte_hw.sxy[2]; } while(0)

#define gte_ldvxy0_mem(addr)                   psp_gte_ldv0(addr)
#define gte_ldvz0_mem(addr)                    do {} while(0)
#define gte_ldvxy0_precise(addr)               psp_gte_ldv0(addr)
#define gte_ldvz0_precise(addr)                do {} while(0)
#define gte_ldvxy0_precise_keep(addr, k)       psp_gte_ldv0(addr)
#define gte_ldvxy1_mem(addr)                   psp_gte_ldv1(addr)
#define gte_ldvxy1_precise(addr)               psp_gte_ldv1(addr)
#define gte_ldvz1_precise(addr)                do {} while(0)
#define gte_ldvxy2_precise(addr)               psp_gte_ldv2(addr)
#define gte_ldvz2_precise(addr)                do {} while(0)

#define gte_stmac0_precise(addr)               do { *(uint32_t*)(addr) = g_psp_gte_hw.mac0; } while(0)
#define gte_getflag(out)                       do { (out) = g_psp_gte_hw.flag; } while(0)
#define gte_cop2_hazard_slot()                 do {} while(0)

#define gte_bind_three_outputs(o0, o1, o2)     do {} while(0)
#define gte_bind_separate_outputs(o0, o1, o2)  do {} while(0)
#define gte_declare_three_outputs()            \
    void *gte_out0 __attribute__((unused)) = NULL; \
    void *gte_out1 __attribute__((unused)) = NULL; \
    void *gte_out2 __attribute__((unused)) = NULL

#define gte_strgb0_bound()                     do {} while(0)
#define gte_strgb1_bound()                     do {} while(0)
#define gte_strgb2_bound()                     do {} while(0)
#define gte_stsxy_three_0()                    do {} while(0)
#define gte_stsxy_three_1()                    do {} while(0)
#define gte_stsxy_three_2()                    do {} while(0)
#define gte_stsxy2_three_0()                   do {} while(0)
#define gte_stir0_three_1()                    do {} while(0)

#define gte_ldir0(v)                           do { g_psp_gte_hw.ir0 = (v); } while(0)
#define gte_stir0(p)                           do { *(int32_t*)(p) = g_psp_gte_hw.ir0; } while(0)
#define gte_ldmac123(x, y, z)                  do { g_psp_gte_hw.mac[0]=(x); g_psp_gte_hw.mac[1]=(y); g_psp_gte_hw.mac[2]=(z); } while(0)
#define gte_lddqa(v)                           do { g_psp_gte_hw.dqa = (v); } while(0)
#define gte_lddqb(v)                           do { g_psp_gte_hw.dqb = (v); } while(0)
#define gte_SetGeomOffset(x, y)                do { g_psp_gte_hw.ofx = (x); g_psp_gte_hw.ofy = (y); PspGte_SetGeomOffset(x, y); } while(0)
#define gte_SetGeomScreen(h)                   do { g_psp_gte_hw.h = (h); PspGte_SetGeomScreen(h); } while(0)
#define gte_SetBackColor(r, g, b)              do { g_psp_gte_hw.bk[0]=(r); g_psp_gte_hw.bk[1]=(g); g_psp_gte_hw.bk[2]=(b); } while(0)
#define gte_SetColorMatrix(m)                  do { memcpy(g_psp_gte_hw.color, (m), sizeof(g_psp_gte_hw.color)); } while(0)
#define gte_SetLightMatrix(m)                  do { memcpy(g_psp_gte_hw.light, (m), sizeof(g_psp_gte_hw.light)); } while(0)

#define gte_getsz3(out)                        do { (out) = g_psp_gte_hw.sz[0]; } while(0)
#define gte_getsz3_for_store(out)              do { (out) = g_psp_gte_hw.sz[0]; } while(0)
#define gte_getotz(out)                        do { (out) = g_psp_gte_hw.otz; } while(0)
#define gte_getmac0(out)                       do { (out) = g_psp_gte_hw.mac0; } while(0)
#define gte_getlzcr(out)                       do { (out) = g_psp_gte_hw.lzcr; } while(0)
#define gte_getlzcr_now(out)                   do { (out) = g_psp_gte_hw.lzcr; } while(0)
#define gte_ldlzcs(v)                          do { g_psp_gte_hw.lzcs = (v); } while(0)
#define gte_stlzcr(p)                          do { *(uint32_t*)(p) = g_psp_gte_hw.lzcr; } while(0)

#define gte_intpl()                            do {} while(0)
#define gte_gpf0()                             do {} while(0)
#define gte_gpf12()                            do {} while(0)
#define gte_gpl0()                             do {} while(0)
#define gte_gpl0_now()                         do {} while(0)
#define gte_gpl12()                            do {} while(0)
#define gte_dpcl()                             do {} while(0)
#define gte_dpct_separate()                    do {} while(0)
#define gte_avsz3()                            do {} while(0)
#define gte_avsz3_padded()                     do {} while(0)
#define gte_avsz4()                            do {} while(0)
#define gte_sqr()                              do {} while(0)
#define gte_sqr12()                            do {} while(0)
#define gte_op0()                              do {} while(0)
#define gte_op12()                             do {} while(0)
#define gte_op12_psyq()                        do {} while(0)
#define gte_rt()                               do {} while(0)
#define gte_rtir()                             do {} while(0)
#define gte_ncct()                             do {} while(0)
#define gte_MulMatrix0(m0, m1, out)            do {} while(0)
#define gte_CompMatrix(m0, m1, out)            do {} while(0)
#define gte_CompMatrixMac(m0, m1, out)         do {} while(0)
#define gte_ReadMatrix(m)                      do {} while(0)

#define gte_ldrgb(p)                           do {} while(0)
#define gte_ldrgb0(p)                          do {} while(0)
#define gte_ldrgb1(p)                          do {} while(0)
#define gte_ldrgb2(p)                          do {} while(0)
#define gte_strgb(p)                           do {} while(0)
#define gte_strgb3(p)                          do {} while(0)
#define gte_stsz3c(p)                          do {} while(0)
#define gte_stszotz(p)                         do {} while(0)
#define gte_stsxy3(p)                          do {} while(0)
#define gte_stsxy3c(p)                         do {} while(0)
#define gte_stsv(p)                            do {} while(0)
#define gte_stclmv(p)                          do {} while(0)
#define gte_stlvl(p)                           do {} while(0)
#define gte_stlvnl(p)                          do {} while(0)

#define gte_ldv0_word3(p)                      psp_gte_ldv0(p)
#define gte_ldv0_word3_at(p, o)                psp_gte_ldv0((const char*)(p) + (o))
#define gte_ldv3(p)                            psp_gte_ldv0(p)
#define gte_ldv3c(p)                           psp_gte_ldv0(p)
#define gte_ldlv0(p)                           do {} while(0)
#define gte_ldclmv(p)                          do {} while(0)
#define gte_ldopv(p)                           do {} while(0)
#define gte_ldopv1(p)                          do {} while(0)
#define gte_ldopv1_psyq(p)                     do {} while(0)
#define gte_ldopv2(p)                          do {} while(0)
#define gte_ldsz3(v)                           do {} while(0)
#define gte_ldsz4(v)                           do {} while(0)
#define gte_ldsxy0(v)                          do { g_psp_gte_hw.sxy[0] = (v); } while(0)
#define gte_ldsxy1(v)                          do { g_psp_gte_hw.sxy[1] = (v); } while(0)
#define gte_ldsxy2(v)                          do { g_psp_gte_hw.sxy[2] = (v); } while(0)
#define gte_ldsxy3(v)                          do { g_psp_gte_hw.sxy[3] = (v); } while(0)
#define gte_ldir123(x, y, z)                   do { g_psp_gte_hw.ir[0]=(x); g_psp_gte_hw.ir[1]=(y); g_psp_gte_hw.ir[2]=(z); } while(0)
#define gte_ldir123v(p)                        do { memcpy(g_psp_gte_hw.ir, (p), 12); } while(0)
#define gte_stir123(p)                         do { memcpy((p), g_psp_gte_hw.ir, 12); } while(0)
#define gte_stir123_column(p)                  do { memcpy((p), g_psp_gte_hw.ir, 12); } while(0)
#define gte_stir123_column_at(p, o)            do { memcpy((char*)(p) + (o), g_psp_gte_hw.ir, 12); } while(0)
#define gte_stir123_gpl(p)                     do { memcpy((p), g_psp_gte_hw.ir, 12); } while(0)
#define gte_mvmva_light_ir_sf0()               psp_gte_mvmva_impl()
#define gte_mvmva_rotation_ir_sf0()            psp_gte_mvmva_impl()
#define gte_mvmva_rotation_ir_sf12()           psp_gte_mvmva_impl()
#define gte_rtv0tr_mac()                       psp_gte_mvmva_impl()
#define gte_rtv0tr_sf0()                       psp_gte_mvmva_impl()
#define gte_getmac123_staged(x, y, z)          do { (x)=g_psp_gte_hw.mac[0]; (y)=g_psp_gte_hw.mac[1]; (z)=g_psp_gte_hw.mac[2]; } while(0)
#define gte_getmac12_staged(x, y)              do { (x)=g_psp_gte_hw.mac[0]; (y)=g_psp_gte_hw.mac[1]; } while(0)
#define gte_stmac_return()                     g_psp_gte_hw.mac[0]

#define gte_declare_rt4_results(f0, f1, d)     \
    int f0 __attribute__((unused)) = 0; \
    int f1 __attribute__((unused)) = 0; \
    int d  __attribute__((unused)) = 0

#define gte_set_shift12()                      do {} while(0)
#define gte_pushrotcol0()                      do {} while(0)
#define gte_poprotcol0()                       do {} while(0)
#define gte_setrotcol0()                       do {} while(0)
#define gte_load_byte2(p)                      do {} while(0)
#define gte_load_byte3(p)                      do {} while(0)
#define gte_ldrtir12_matrix_column(m, c)       do {} while(0)
#define gte_lightcolor(v)                      do {} while(0)

#ifdef __cplusplus
}
#endif

#endif /* PE1_GTE_PSP_H */
