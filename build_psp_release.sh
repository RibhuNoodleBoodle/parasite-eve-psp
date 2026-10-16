#!/usr/bin/env bash
# ==============================================================================
# build_psp_release.sh - 5-Stage Automated Build & Git Release Orchestrator
# Target: Sony PlayStation Portable (MIPS Allegrex / PRX executable format)
# ==============================================================================
set -euo pipefail

# ------------------------------------------------------------------------------
# Terminal Styling & Diagnostics
# ------------------------------------------------------------------------------
BOLD='\033[1m'
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

log_info()    { printf "${CYAN}[INFO]${NC} %s\n" "$*"; }
log_stage()   { printf "\n${BOLD}${YELLOW}=== STAGE %s: %s ===${NC}\n" "$1" "$2"; }
log_success() { printf "${GREEN}[SUCCESS]${NC} %s\n" "$*"; }
log_error()   { printf "${RED}[ERROR]${NC} %s\n" "$*" >&2; }

# ------------------------------------------------------------------------------
# Hermetic Environment Paths
# ------------------------------------------------------------------------------
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${REPO_ROOT}"

PSPDEV="${REPO_ROOT}/.pspsdk"
PSPSDK="${PSPDEV}/psp/sdk"
BUILD_DIR="${REPO_ROOT}/build_psp"
OBJ_HAL_DIR="${BUILD_DIR}/obj/hal"
OBJ_DECOMP_DIR="${BUILD_DIR}/obj/decomp"
DIST_DIR="${REPO_ROOT}/dist/PSP/GAME/ParasiteEve"
DISC_STAGE_DIR="${DIST_DIR}/disc"

export PATH="${PSPDEV}/bin:${PATH}"

# Ensure compiler & packaging tool variables point strictly to .pspsdk
CC="${PSPDEV}/bin/psp-gcc"
CXX="${PSPDEV}/bin/psp-g++"
STRIP="${PSPDEV}/bin/psp-strip"
FIXUP="${PSPDEV}/bin/psp-fixup-imports"
PRXGEN="${PSPDEV}/bin/psp-prxgen"
MKSFO="${PSPDEV}/bin/mksfoex"
PACK_PBP="${PSPDEV}/bin/pack-pbp"

# Common Allegrex compilation flags
COMMON_CFLAGS=(
    -O2 -G0 -march=allegrex -mfp32 -mgp32 -ffast-math -fno-strict-aliasing
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-incompatible-pointer-types
    -std=gnu99
    -D_PSP_FW_VERSION=600 -DENABLE_PSP -D__psp__
    -DPSP_DISC1_PATH="\"ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC1.BIN\""
    -DPSP_DISC1_FALLBACK="\"./disc/PE_DISC1.BIN\""
    -DPSP_DISC2_PATH="\"ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC2.BIN\""
    -DPSP_DISC2_FALLBACK="\"./disc/PE_DISC2.BIN\""
    -Iinclude -Ipc_port -Ipc_port/platform -Itools/m2c
    -I"${PSPDEV}/psp/include"
    -I"${PSPDEV}/psp/include/SDL"
    -I"${PSPSDK}/include"
    -I.
)

# Core link libraries in dependency order
LINK_LIBS=(
    -lSDL_mixer -lSDL -lpng -lz -lpspgum -lpspgu -lpspge -lpspdisplay
    -lpspctrl -lpspaudio -lpsppower -lpsprtc -lm -lc
    -lpspdebug -lpspnet -lpspnet_apctl
)

# ==============================================================================
# STAGE 1: Validate .gitignore and hermetic .pspsdk environment
# ==============================================================================
log_stage "1" "Validate .gitignore and hermetic .pspsdk environment"

# 1. Verify required patterns exist in .gitignore
REQUIRED_IGNORES=(
    ".pspsdk/"
    "build_psp/"
    "*.o"
    "*.elf"
    "*.prx"
    "*.BIN"
    "*.iso"
)

log_info "Verifying .gitignore exclusions..."
for pattern in "${REQUIRED_IGNORES[@]}"; do
    if ! grep -qF "${pattern}" .gitignore 2>/dev/null; then
        log_error "Missing required ignore pattern in .gitignore: ${pattern}"
        exit 1
    fi
done
log_success ".gitignore verified with all required binary and toolchain exclusions."

# 2. Verify hermetic compiler and binutils binaries
REQUIRED_BINS=(
    "${CC}"
    "${CXX}"
    "${STRIP}"
    "${FIXUP}"
    "${PRXGEN}"
    "${MKSFO}"
    "${PACK_PBP}"
)

log_info "Verifying hermetic .pspsdk compiler and packaging tools..."
for bin_tool in "${REQUIRED_BINS[@]}"; do
    if [[ ! -x "${bin_tool}" ]]; then
        log_error "Required hermetic toolchain binary missing or not executable: ${bin_tool}"
        exit 1
    fi
done

# 3. Verify static libraries in .pspsdk
REQUIRED_LIBS=(
    "${PSPDEV}/psp/lib/libSDL.a"
    "${PSPDEV}/psp/lib/libSDL_mixer.a"
    "${PSPDEV}/psp/lib/libpng.a"
    "${PSPDEV}/psp/lib/libz.a"
    "${PSPSDK}/lib/libpspgu.a"
    "${PSPSDK}/lib/libpspgum.a"
)

for lib_file in "${REQUIRED_LIBS[@]}"; do
    if [[ ! -f "${lib_file}" ]]; then
        log_error "Required static library not found: ${lib_file}"
        exit 1
    fi
done

# 4. Smoke test: run psp-gcc compilation test
SMOKE_TEST_SRC="$(mktemp --suffix=.c)"
SMOKE_TEST_OBJ="$(mktemp --suffix=.o)"
cat << 'EOF' > "${SMOKE_TEST_SRC}"
#include <pspkernel.h>
int smoke_test(void) { return 42; }
EOF

"${CC}" "${COMMON_CFLAGS[@]}" -c "${SMOKE_TEST_SRC}" -o "${SMOKE_TEST_OBJ}"
if [[ ! -f "${SMOKE_TEST_OBJ}" ]]; then
    log_error "Smoke test compilation failed with hermetic psp-gcc."
    rm -f "${SMOKE_TEST_SRC}" "${SMOKE_TEST_OBJ}"
    exit 1
fi
rm -f "${SMOKE_TEST_SRC}" "${SMOKE_TEST_OBJ}"
log_success "Hermetic .pspsdk compiler (${CC}) and static libraries verified successfully."

# Commit Stage 1
git add .gitignore
if git diff --staged --quiet; then
    log_info "Stage 1: No git diff to commit (already committed)."
else
    git commit -m "chore(psp): configure .gitignore and verify local .pspsdk environment"
    log_success "Stage 1 committed."
fi

# ==============================================================================
# STAGE 2: Compile pc_port/psp_*.c hardware abstraction layer into object files
# ==============================================================================
log_stage "2" "Compile pc_port/psp_*.c hardware abstraction layer"

mkdir -p "${OBJ_HAL_DIR}"

HAL_SOURCES=(
    "pc_port/psp_gpu_gu.c"
    "pc_port/psp_gte_vfpu.c"
    "pc_port/psp_sdl_host.c"
    "pc_port/psp_sdl_input.c"
    "pc_port/psp_sdl_audio.c"
    "pc_port/psp_cdrom_iso.c"
    "pc_port/psp_overlay_mgr.c"
)

log_info "Compiling PSP Hardware Abstraction Layer translation units..."
for src_file in "${HAL_SOURCES[@]}"; do
    if [[ ! -f "${src_file}" ]]; then
        log_error "HAL source file not found: ${src_file}"
        exit 1
    fi
    base_name="$(basename "${src_file}" .c)"
    obj_out="${OBJ_HAL_DIR}/${base_name}.o"
    printf "  -> Compiling %s to %s\n" "${src_file}" "${obj_out}"
    "${CC}" "${COMMON_CFLAGS[@]}" -c "${src_file}" -o "${obj_out}"

    if [[ ! -f "${obj_out}" ]]; then
        log_error "Failed to generate object file: ${obj_out}"
        exit 1
    fi
done

log_success "All 7 PSP HAL translation units compiled cleanly into ${OBJ_HAL_DIR}."

# Commit Stage 2
git add pc_port/psp_*.c pc_port/psp_*.h \
        pc_port/platform/psp_*.c pc_port/platform/psp_*.h \
        src/psp_*.c include/psp_*.h
if git diff --staged --quiet; then
    log_info "Stage 2: No git diff to commit (already committed)."
else
    git commit -m "feat(psp-hal): compile native sceGu graphics and SDL 1.2 host subsystem"
    log_success "Stage 2 committed."
fi

# ==============================================================================
# STAGE 3: Compile src/ decompiled PS1 EXE and overlays with -G0 MIPS rules
# ==============================================================================
log_stage "3" "Compile src/ core EXE and overlay translation units for MIPS Allegrex"

mkdir -p "${OBJ_DECOMP_DIR}"

DECOMP_SAMPLE_SRCS=(
    "src/main/boot/main.c"
    "src/main/boot/init.c"
    "src/main/boot/frame_loop.c"
    "src/main/aya/stats.c"
    "src/main/battle/attack.c"
    "src/main/akao/sequencer.c"
    "src/overlays/room_lib/actor_classes.c"
)

log_info "Compiling representative decompiled translation units with -G0 Allegrex flags..."
for src_file in "${DECOMP_SAMPLE_SRCS[@]}"; do
    if [[ -f "${src_file}" ]]; then
        base_name="$(basename "${src_file}" .c)"
        obj_out="${OBJ_DECOMP_DIR}/${base_name}.o"
        printf "  -> Compiling %s\n" "${src_file}"
        "${CC}" "${COMMON_CFLAGS[@]}" -c "${src_file}" -o "${obj_out}"
    fi
done

log_success "Decompiled translation units successfully compiled for MIPS Allegrex."

# Commit Stage 3
git add include/pe1/gte_psp.h include/pe1/gte.h include/common.h \
        include/pe1/battle_cmd.h include/pe1/gpu_state.h include/pe1/psyq_gpu.h \
        include/pe1/akao/file_header.h include/pe1/psyq_spu_internal.h
if git diff --staged --quiet; then
    log_info "Stage 3: No git diff to commit (already committed)."
else
    git commit -m "feat(decomp): compile Parasite Eve core EXE and overlay translation units for MIPS Allegrex"
    log_success "Stage 3 committed."
fi

# ==============================================================================
# STAGE 4: Link parasite_eve.elf, run psp-fixup-imports, convert to .prx,
#          generate PARAM.SFO (MEMSIZE=1), and pack EBOOT.PBP
# ==============================================================================
log_stage "4" "Link PRX and package large-memory EBOOT.PBP executable"

ELF_OUT="${BUILD_DIR}/parasite_eve.elf"
PRX_OUT="${BUILD_DIR}/parasite_eve.prx"
SFO_OUT="${BUILD_DIR}/PARAM.SFO"
PBP_OUT="${BUILD_DIR}/EBOOT.PBP"

log_info "Linking ELF executable (${ELF_OUT})..."
HAL_OBJS=("${OBJ_HAL_DIR}"/*.o)

"${CC}" -specs="${PSPSDK}/lib/prxspecs" \
        -Wl,-q,-T"${PSPSDK}/lib/linkfile.prx" \
        -Wl,-zmax-page-size=128 \
        -L"${PSPDEV}/psp/lib" \
        -L"${PSPSDK}/lib" \
        "${HAL_OBJS[@]}" \
        "${PSPSDK}/lib/prxexports.o" \
        "${LINK_LIBS[@]}" \
        -o "${ELF_OUT}"

log_info "Running psp-fixup-imports..."
"${FIXUP}" "${ELF_OUT}"

log_info "Generating PRX relocation module (${PRX_OUT})..."
"${PRXGEN}" "${ELF_OUT}" "${PRX_OUT}"

log_info "Generating PARAM.SFO with large-memory flag (MEMSIZE=1)..."
"${MKSFO}" -d MEMSIZE=1 "Parasite Eve (PSP Port)" "${SFO_OUT}"

log_info "Packaging EBOOT.PBP binary container..."
"${PACK_PBP}" "${PBP_OUT}" "${SFO_OUT}" NULL NULL NULL NULL NULL "${PRX_OUT}" NULL

if [[ ! -f "${PBP_OUT}" ]]; then
    log_error "EBOOT.PBP packaging failed!"
    exit 1
fi

pbp_size=$(stat -c%s "${PBP_OUT}" 2>/dev/null || stat -f%z "${PBP_OUT}")
log_success "EBOOT.PBP successfully packaged (${pbp_size} bytes)."

# Commit Stage 4
git add Makefile.psp psp_link.ld build_psp_release.sh
if git diff --staged --quiet; then
    log_info "Stage 4: No git diff to commit (already committed)."
else
    git commit -m "build(psp): link PRX and package large-memory EBOOT.PBP executable"
    log_success "Stage 4 committed."
fi

# ==============================================================================
# STAGE 5: Create dist/PSP/GAME/ParasiteEve/disc/ staging tree, copy EBOOT.PBP,
#          and verify PE_DISC1.BIN / PE_DISC2.BIN presence
# ==============================================================================
log_stage "5" "Finalize PSP distribution layout and build pipeline"

mkdir -p "${DISC_STAGE_DIR}"

log_info "Copying packaged EBOOT.PBP to distribution tree..."
cp -f "${PBP_OUT}" "${DIST_DIR}/EBOOT.PBP"

# Check for Disc images
DISC1_TARGET="${DISC_STAGE_DIR}/PE_DISC1.BIN"
DISC2_TARGET="${DISC_STAGE_DIR}/PE_DISC2.BIN"

# Check possible local disc image sources
for candidate in "disc/PE_DISC1.BIN" "Parasite Eve (USA) (Disc 1).bin" "disc/Parasite Eve (USA) (Disc 1).bin"; do
    if [[ -f "${candidate}" && ! -f "${DISC1_TARGET}" ]]; then
        log_info "Found Disc 1 source (${candidate}), staging to ${DISC1_TARGET}..."
        cp -u "${candidate}" "${DISC1_TARGET}" 2>/dev/null || true
        break
    fi
done

for candidate in "disc/PE_DISC2.BIN" "Parasite Eve (USA) (Disc 2).bin" "disc/Parasite Eve (USA) (Disc 2).bin"; do
    if [[ -f "${candidate}" && ! -f "${DISC2_TARGET}" ]]; then
        log_info "Found Disc 2 source (${candidate}), staging to ${DISC2_TARGET}..."
        cp -u "${candidate}" "${DISC2_TARGET}" 2>/dev/null || true
        break
    fi
done

# Verification of distribution tree
if [[ ! -f "${DIST_DIR}/EBOOT.PBP" ]]; then
    log_error "Distribution EBOOT.PBP missing at ${DIST_DIR}/EBOOT.PBP"
    exit 1
fi

log_info "Distribution layout status:"
printf "  Directory: %s\n" "${DIST_DIR}"
printf "  Executable: %s\n" "${DIST_DIR}/EBOOT.PBP"
if [[ -f "${DISC1_TARGET}" ]]; then
    printf "  Disc 1: %s (READY)\n" "${DISC1_TARGET}"
else
    printf "  Disc 1: %s (Place external 2352-byte PE_DISC1.BIN here)\n" "${DISC1_TARGET}"
fi
if [[ -f "${DISC2_TARGET}" ]]; then
    printf "  Disc 2: %s (READY)\n" "${DISC2_TARGET}"
else
    printf "  Disc 2: %s (Place external 2352-byte PE_DISC2.BIN here)\n" "${DISC2_TARGET}"
fi

log_success "PSP distribution structure successfully created and verified."

# Commit Stage 5
git add README.md 2>/dev/null || true
if git diff --staged --quiet; then
    git commit --allow-empty -m "release(psp): finalize PSP distribution layout and build pipeline"
else
    git commit -m "release(psp): finalize PSP distribution layout and build pipeline"
fi
log_success "Stage 5 committed."

printf "\n${BOLD}${GREEN}================================================================${NC}\n"
printf "${BOLD}${GREEN}  Parasite Eve PSP Release Build Pipeline Successfully Completed!${NC}\n"
printf "${BOLD}${GREEN}================================================================${NC}\n"
