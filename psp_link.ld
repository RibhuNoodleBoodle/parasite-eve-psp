/*
 * psp_link.ld - Custom Linker Script for Parasite Eve (MIPS Allegrex PSP PRX)
 *
 * Configures the MIPS Allegrex executable memory layout with an explicit
 * .overlay_arena shared union section to support the 32 MB PSP-1000 and
 * 64 MB PSP-2000/3000 user memory constraints.
 */

OUTPUT_FORMAT("elf32-littlemips", "elf32-bigmips", "elf32-littlemips")
OUTPUT_ARCH(mips:allegrex)
ENTRY(_start)

MEMORY
{
    /* User RAM layout: standard PSP user space begins at 0x08804000 */
    ram (rwx) : ORIGIN = 0x08804000, LENGTH = 24M
}

SECTIONS
{
    /* ── Read-Only Executable Code ─────────────────────────────────────────── */
    . = 0x08804000;
    PROVIDE(__executable_start = .);

    .text :
    {
        _ftext = .;
        *(.text.startup)
        *(.text)
        *(.text.*)
        *(.gnu.linkonce.t.*)
        . = ALIGN(16);
        _etext = .;
    } > ram

    /* ── PSP System Stubs & Module Entitlements ────────────────────────────── */
    .sceStub.text : {
        *(.sceStub.text)
        *(SORT(.sceStub.text.*))
    } > ram

    .lib.ent.top : { *(.lib.ent.top) } > ram
    .lib.ent     : { *(.lib.ent) } > ram
    .lib.ent.btm : { *(.lib.ent.btm) } > ram

    .lib.stub.top : { *(.lib.stub.top) } > ram
    .lib.stub     : { *(.lib.stub) } > ram
    .lib.stub.btm : { *(.lib.stub.btm) } > ram

    /* ── Read-Only Data ────────────────────────────────────────────────────── */
    .rodata :
    {
        _frodata = .;
        *(.rodata)
        *(.rodata.*)
        *(.gnu.linkonce.r.*)
        . = ALIGN(16);
        _erodata = .;
    } > ram

    .rodata.sceModuleInfo : { *(.rodata.sceModuleInfo) } > ram
    .rodata.sceResident   : { *(.rodata.sceResident) } > ram
    .rodata.sceNid        : { *(.rodata.sceNid) } > ram
    .rodata.sceVstub      : { *(.rodata.sceVstub) *(SORT(.rodata.sceVstub.*)) } > ram

    /* ── Initialized Data ──────────────────────────────────────────────────── */
    . = ALIGN(64);
    .data :
    {
        _fdata = .;
        *(.data)
        *(.data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(16);
        _edata = .;
    } > ram

    /* ── Small Data (.sdata) & Global Pointer (_gp) ────────────────────────── */
    . = ALIGN(16);
    _gp = . + 0x7FF0;
    .sdata :
    {
        *(.sdata)
        *(.sdata.*)
        *(.gnu.linkonce.s.*)
    } > ram

    /*
     * ── Shared Overlay Memory Arena (NOLOAD) ────────────────────────────────
     * All transient overlays (rooms, menus, battle FX) map their dynamic
     * state and scratch allocations into this 2 MB region. Marked NOLOAD
     * so it consumes zero space in the PRX file on the Memory Stick.
     */
    . = ALIGN(64);
    .overlay_arena (NOLOAD) :
    {
        __overlay_arena_start = .;
        *(.overlay_arena)
        *(.overlay_arena.*)
        . = ALIGN(64);
        __overlay_arena_end = .;
    } > ram

    /* ── Uninitialized BSS ─────────────────────────────────────────────────── */
    . = ALIGN(64);
    __bss_start = .;
    _fbss = .;

    .sbss :
    {
        *(.sbss)
        *(.sbss.*)
        *(.gnu.linkonce.sb.*)
        *(.scommon)
    } > ram

    .bss :
    {
        *(.bss)
        *(.bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(16);
    } > ram

    _end = .;
    PROVIDE(end = .);

    /* ── Discard Unused Metadata ───────────────────────────────────────────── */
    /DISCARD/ :
    {
        *(.MIPS.options)
        *(.note.GNU-stack)
        *(.comment)
        *(.pdr)
    }
}
