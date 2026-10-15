#ifndef PSP_OVERLAY_MGR_H
#define PSP_OVERLAY_MGR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PSP Memory & Overlay Manager
 *
 * Implements a shared union memory arena allocator in PSP user memory
 * (2 MB aligned to 64 bytes) to support the 32 MB (PSP-1000) / 64 MB (PSP-2000/3000)
 * physical RAM constraint, eliminating static BSS/Data section bloat across
 * all compiled room, battle, menu, and FX overlays.
 */

#define PSP_OVERLAY_ARENA_SIZE    (2u * 1024u * 1024u) /* 2 MB shared arena */
#define PSP_OVERLAY_ALIGNMENT     64u

/* Function signature for guest thunks */
typedef int (*PspGuestThunk)(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);

typedef struct PspGuestEntry {
    uint32_t      vma;         /* PS1 virtual entry address */
    PspGuestThunk thunk;       /* Host compiled C implementation */
    const char   *symbol_name; /* Function identifier */
} PspGuestEntry;

typedef struct PspOverlayDesc {
    const char          *name;        /* e.g. "room_m005_theater" */
    uint32_t             vma_start;   /* Lower bound of guest overlay load window */
    uint32_t             vma_end;     /* Upper bound */
    size_t               bss_size;    /* Overlay memory footprint */
    int                (*init_fn)(void *arena, size_t arena_size);
    void               (*shutdown_fn)(void);
    const PspGuestEntry *entries;
    unsigned             entry_count;
} PspOverlayDesc;

/* Subsystem Lifecycle */
int   PspOverlay_Init(void);
void  PspOverlay_Shutdown(void);
void *PspOverlay_GetArena(size_t *size_out);

/* Registration & Activation */
int   PspOverlay_Register(const PspOverlayDesc *desc);
int   PspOverlay_Activate(const char *name);
int   PspOverlay_IsResident(const char *name);
const char *PspOverlay_GetActiveName(void);
void  PspOverlay_Reset(void);

/* Guest Dispatch & Relay */
PspGuestThunk PspOverlay_Resolve(uint32_t vma_addr);
int   PspOverlay_Dispatch(const char *callsite, uint32_t vma_addr, unsigned arity,
                          uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);

/* Drop-in compatibility with pc_port pe_guestcode.h */
int         PE_Overlay_SetResident(const char *ovl);
int         PE_Overlay_SetResidentVerified(const char *ovl);
int         PE_Overlay_IsResident(const char *ovl);
void        PE_Overlay_ResetResidency(void);
const void *PE_GuestCode_Resolve(uint32_t addr);
int         PE_GuestCall(const char *site, uint32_t fn, unsigned arity,
                         uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);

#ifdef __cplusplus
}
#endif

#endif /* PSP_OVERLAY_MGR_H */
