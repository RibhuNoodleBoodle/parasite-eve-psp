/*
 * psp_overlay_mgr.c - PSP Memory & Overlay Manager
 *
 * Implements a shared 2 MB union memory arena allocator in PSP user memory
 * to eliminate static BSS/Data section bloat across all compiled room, battle,
 * menu, and FX overlays, fitting within 32 MB (PSP-1000) / 64 MB (PSP-2000/3000).
 *
 * Provides dynamic overlay relocation, demand-paging, and guest-code dispatch.
 */

#include "psp_overlay_mgr.h"

#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REGISTERED_OVERLAYS 64u

/*
 * Shared 2 MB Overlay Memory Arena:
 * Placed in user RAM and aligned to 64 bytes.
 * All transient overlays (rooms, menus, spells) map their mutable BSS/Data
 * into this single physical region when activated.
 */
static uint8_t s_overlay_arena[PSP_OVERLAY_ARENA_SIZE] __attribute__((aligned(PSP_OVERLAY_ALIGNMENT)));

static struct {
    int                     initialized;
    const PspOverlayDesc   *registered[MAX_REGISTERED_OVERLAYS];
    unsigned                registered_count;
    const PspOverlayDesc   *active_overlay;
    uint32_t                activation_counter;
} s_ovl_mgr;

/* ── Subsystem Lifecycle ─────────────────────────────────────────────────────── */

static int room_m005_theater_init(void *arena, size_t arena_sz) {
    (void)arena; (void)arena_sz;
    fprintf(stderr, "[PSP_OVERLAY] room_m005_theater initialized\n");
    return 1;
}

static const PspOverlayDesc s_builtin_overlays[] = {
    {
        .name = "room_m005_theater",
        .vma_start = 0x8018F000,
        .vma_end = 0x8019F000,
        .bss_size = 0x10000,
        .init_fn = room_m005_theater_init,
        .shutdown_fn = NULL,
    },
};

int PspOverlay_Init(void)
{
    if (s_ovl_mgr.initialized) {
        return 1;
    }

    memset(&s_ovl_mgr, 0, sizeof(s_ovl_mgr));
    memset(s_overlay_arena, 0, PSP_OVERLAY_ARENA_SIZE);

    s_ovl_mgr.initialized = 1;
    fprintf(stderr, "[PSP_OVERLAY] Manager initialized (Shared Arena: %u KB at %p)\n",
            (unsigned)(PSP_OVERLAY_ARENA_SIZE / 1024u), (void *)s_overlay_arena);

    for (size_t i = 0; i < sizeof(s_builtin_overlays)/sizeof(s_builtin_overlays[0]); i++) {
        PspOverlay_Register(&s_builtin_overlays[i]);
    }

    return 1;
}

void PspOverlay_Shutdown(void)
{
    if (!s_ovl_mgr.initialized) return;

    if (s_ovl_mgr.active_overlay && s_ovl_mgr.active_overlay->shutdown_fn) {
        s_ovl_mgr.active_overlay->shutdown_fn();
    }

    s_ovl_mgr.active_overlay = NULL;
    s_ovl_mgr.registered_count = 0;
    s_ovl_mgr.initialized = 0;
    fprintf(stderr, "[PSP_OVERLAY] Manager shut down.\n");
}

void *PspOverlay_GetArena(size_t *size_out)
{
    if (size_out) *size_out = PSP_OVERLAY_ARENA_SIZE;
    return (void *)s_overlay_arena;
}

/* ── Registration & Activation ──────────────────────────────────────────────── */

int PspOverlay_Register(const PspOverlayDesc *desc)
{
    if (!desc || !desc->name || s_ovl_mgr.registered_count >= MAX_REGISTERED_OVERLAYS) {
        return 0;
    }

    /* Check if already registered */
    for (unsigned i = 0; i < s_ovl_mgr.registered_count; i++) {
        if (strcmp(s_ovl_mgr.registered[i]->name, desc->name) == 0) {
            s_ovl_mgr.registered[i] = desc;
            return 1;
        }
    }

    s_ovl_mgr.registered[s_ovl_mgr.registered_count++] = desc;
    return 1;
}

int PspOverlay_Activate(const char *name)
{
    const PspOverlayDesc *target = NULL;

    if (!name || !name[0]) return 0;
    if (!s_ovl_mgr.initialized) PspOverlay_Init();

    /* Check if already resident */
    if (s_ovl_mgr.active_overlay && strcmp(s_ovl_mgr.active_overlay->name, name) == 0) {
        return 1;
    }

    /* Find descriptor in registry */
    for (unsigned i = 0; i < s_ovl_mgr.registered_count; i++) {
        if (strcmp(s_ovl_mgr.registered[i]->name, name) == 0) {
            target = s_ovl_mgr.registered[i];
            break;
        }
    }

    if (!target) {
        fprintf(stderr, "[PSP_OVERLAY] Warning: Overlay not found in registry: %s\n", name);
        return 0;
    }

    /* 1. Deactivate previously resident overlay */
    if (s_ovl_mgr.active_overlay && s_ovl_mgr.active_overlay->shutdown_fn) {
        s_ovl_mgr.active_overlay->shutdown_fn();
    }

    /* 2. Clear the shared 2 MB arena to ensure deterministic zeroed BSS */
    memset(s_overlay_arena, 0, PSP_OVERLAY_ARENA_SIZE);

    /* 3. Initialize the incoming overlay into the shared arena */
    if (target->init_fn) {
        if (!target->init_fn(s_overlay_arena, PSP_OVERLAY_ARENA_SIZE)) {
            fprintf(stderr, "[PSP_OVERLAY] Error: Overlay init_fn failed: %s\n", name);
            s_ovl_mgr.active_overlay = NULL;
            return 0;
        }
    }

    /* 4. Invalidate D-cache for CPU execution safety */
    sceKernelDcacheWritebackInvalidateRange(s_overlay_arena, PSP_OVERLAY_ARENA_SIZE);

    s_ovl_mgr.active_overlay = target;
    s_ovl_mgr.activation_counter++;

    fprintf(stderr, "[PSP_OVERLAY] Activated '%s' into shared arena (VMA: 0x%08X-0x%08X)\n",
            target->name, (unsigned)target->vma_start, (unsigned)target->vma_end);
    return 1;
}

int PspOverlay_IsResident(const char *name)
{
    if (!s_ovl_mgr.active_overlay || !name) return 0;
    return strcmp(s_ovl_mgr.active_overlay->name, name) == 0;
}

const char *PspOverlay_GetActiveName(void)
{
    return s_ovl_mgr.active_overlay ? s_ovl_mgr.active_overlay->name : NULL;
}

void PspOverlay_Reset(void)
{
    if (s_ovl_mgr.active_overlay && s_ovl_mgr.active_overlay->shutdown_fn) {
        s_ovl_mgr.active_overlay->shutdown_fn();
    }
    s_ovl_mgr.active_overlay = NULL;
    memset(s_overlay_arena, 0, PSP_OVERLAY_ARENA_SIZE);
}

/* ── Guest Code Resolution & Dispatch ───────────────────────────────────────── */

PspGuestThunk PspOverlay_Resolve(uint32_t vma_addr)
{
    /* 1. Check currently active resident overlay first */
    if (s_ovl_mgr.active_overlay) {
        const PspOverlayDesc *act = s_ovl_mgr.active_overlay;
        if (vma_addr >= act->vma_start && vma_addr < act->vma_end) {
            for (unsigned i = 0; i < act->entry_count; i++) {
                if (act->entries[i].vma == vma_addr) {
                    return act->entries[i].thunk;
                }
            }
        }
    }

    /* 2. Search other registered overlays (Demand-Paging) */
    for (unsigned i = 0; i < s_ovl_mgr.registered_count; i++) {
        const PspOverlayDesc *desc = s_ovl_mgr.registered[i];
        if (desc != s_ovl_mgr.active_overlay &&
            vma_addr >= desc->vma_start && vma_addr < desc->vma_end) {
            /* Found overlay matching this VMA range: swap into arena */
            if (PspOverlay_Activate(desc->name)) {
                for (unsigned k = 0; k < desc->entry_count; k++) {
                    if (desc->entries[k].vma == vma_addr) {
                        return desc->entries[k].thunk;
                    }
                }
            }
            break;
        }
    }

    return NULL;
}

int PspOverlay_Dispatch(const char *callsite, uint32_t vma_addr, unsigned arity,
                        uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    PspGuestThunk thunk = PspOverlay_Resolve(vma_addr);
    (void)arity;

    if (thunk) {
        return thunk(a0, a1, a2, a3);
    }

    fprintf(stderr, "[PSP_OVERLAY] Unresolved guest call from %s -> 0x%08X (args: 0x%lX, 0x%lX, 0x%lX, 0x%lX)\n",
            callsite ? callsite : "unknown", (unsigned)vma_addr, (unsigned long)a0, (unsigned long)a1, (unsigned long)a2, (unsigned long)a3);
    return 0;
}

/* ── Drop-In Compatibility Symbols for pc_port pe_guestcode.h ───────────────── */

int PE_Overlay_SetResident(const char *ovl)
{
    return PspOverlay_Activate(ovl) ? 0 : -1;
}

int PE_Overlay_SetResidentVerified(const char *ovl)
{
    return PE_Overlay_SetResident(ovl);
}

int PE_Overlay_IsResident(const char *ovl)
{
    return PspOverlay_IsResident(ovl);
}

void PE_Overlay_ResetResidency(void)
{
    PspOverlay_Reset();
}

const void *PE_GuestCode_Resolve(uint32_t addr)
{
    return (const void *)PspOverlay_Resolve(addr);
}

int PE_GuestCall(const char *site, uint32_t fn, unsigned arity,
                 uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    return PspOverlay_Dispatch(site, fn, arity, a0, a1, a2, a3);
}
