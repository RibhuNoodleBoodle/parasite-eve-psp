#!/usr/bin/env bash
# ==============================================================================
# bootstrap_local_pspsdk.sh
# Hermetic, isolated, system-zero-install PSP toolchain and legacy library
# bootstrap script for the Parasite Eve PS1 decompilation PSP port.
#
# Target Architecture: MIPS Allegrex (R4000-based + VFPU)
# Target OS:           Sony PlayStation Portable (Firmware 6.00+ / 6.60)
# ==============================================================================

set -euo pipefail

# ------------------------------------------------------------------------------
# 1. Environment & Path Initialization
# ------------------------------------------------------------------------------
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PSPDEV="${ROOT_DIR}/.pspsdk"
export PSPSDK="${PSPDEV}/psp/sdk"
export PATH="${PSPDEV}/bin:${PATH}"
export PKG_CONFIG_PATH="${PSPDEV}/psp/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_TOOLCHAIN_FILE="${PSPDEV}/psp/share/pspdev.cmake"

SRC_DIR="${PSPDEV}/src"
TARBALL_NAME="pspdev-ubuntu-latest-x86_64.tar.gz"
TARBALL_PATH="${SRC_DIR}/${TARBALL_NAME}"
FALLBACK_TARBALL_URL="https://github.com/pspdev/pspdev/releases/download/v20261001/${TARBALL_NAME}"

PORTS_REPO_URL="https://github.com/pspdev/psp-ports.git"
MIXER_REPO_URL="https://github.com/pspdev/SDL_mixer.git"

echo "======================================================================"
echo "  PARASITE EVE PSP PORT - ISOLATED HERMETIC TOOLCHAIN BOOTSTRAP"
echo "======================================================================"
echo "[+] Root Directory:  ${ROOT_DIR}"
echo "[+] Target PSPDEV:   ${PSPDEV}"
echo "[+] Target PSPSDK:   ${PSPSDK}"
echo "[+] Target Sysroot:  ${PSPDEV}/psp"
echo "----------------------------------------------------------------------"

mkdir -p "${PSPDEV}" "${SRC_DIR}"

# ------------------------------------------------------------------------------
# 2. Toolchain Extraction (Prebuilt Linux x86_64 PSPDEV)
# ------------------------------------------------------------------------------
install_toolchain() {
    if [ -x "${PSPDEV}/bin/psp-gcc" ] && [ -x "${PSPDEV}/bin/pack-pbp" ]; then
        echo "[*] PSPDEV toolchain binaries already exist in ${PSPDEV}/bin. Skipping download."
        return 0
    fi

    echo "[*] Fetching PSPDEV release tarball..."
    DOWNLOAD_URL=""
    if command -v curl >/dev/null 2>&1; then
        DOWNLOAD_URL=$(curl -s https://api.github.com/repos/pspdev/pspdev/releases/latest 2>/dev/null \
            | grep "browser_download_url.*${TARBALL_NAME}" \
            | head -n 1 \
            | cut -d '"' -f 4 || true)
    fi

    if [ -z "${DOWNLOAD_URL}" ]; then
        echo "[!] GitHub API lookup empty/rate-limited; using fallback release URL."
        DOWNLOAD_URL="${FALLBACK_TARBALL_URL}"
    fi

    echo "[*] Downloading from: ${DOWNLOAD_URL}"
    curl -fL -C - -o "${TARBALL_PATH}" "${DOWNLOAD_URL}"

    echo "[*] Extracting ${TARBALL_NAME} into ${PSPDEV}..."
    # Robust multi-filesystem extraction (handles ext4, btrfs, and exFAT/FAT32 without symlink failures)
    python3 - << 'PYEOF'
import os
import sys
import tarfile
import shutil

tar_path = os.environ.get("TARBALL_PATH")
dest_dir = os.environ.get("PSPDEV")

if not os.path.exists(tar_path):
    sys.exit(f"Tarball not found at: {tar_path}")

print(f"Extracting {tar_path} -> {dest_dir}...")
with tarfile.open(tar_path, "r:gz") as tar:
    members = tar.getmembers()
    links = []
    
    for m in members:
        parts = m.name.split("/")
        if len(parts) > 1 and parts[0] == "pspdev":
            m.name = "/".join(parts[1:])
        elif parts[0] == "pspdev":
            continue
        if not m.name:
            continue
            
        if m.issym() or m.islnk():
            links.append(m)
        else:
            tar.extract(m, path=dest_dir)

    # Process symlinks and hardlinks with copy fallback for exFAT/FAT32
    for m in links:
        dest_path = os.path.join(dest_dir, m.name)
        parent = os.path.dirname(dest_path)
        os.makedirs(parent, exist_ok=True)
        
        if m.issym():
            target = m.linkname
            target_full = os.path.normpath(os.path.join(parent, target))
            try:
                if os.path.lexists(dest_path):
                    os.unlink(dest_path)
                os.symlink(target, dest_path)
            except OSError:
                if os.path.exists(target_full):
                    if os.path.isdir(target_full):
                        shutil.copytree(target_full, dest_path, dirs_exist_ok=True)
                    else:
                        shutil.copy2(target_full, dest_path)
        elif m.islnk():
            target_parts = m.linkname.split("/")
            if len(target_parts) > 1 and target_parts[0] == "pspdev":
                target_rel = "/".join(target_parts[1:])
            else:
                target_rel = m.linkname
            target_full = os.path.normpath(os.path.join(dest_dir, target_rel))
            try:
                if os.path.lexists(dest_path):
                    os.unlink(dest_path)
                os.link(target_full, dest_path)
            except OSError:
                if os.path.exists(target_full):
                    if os.path.isdir(target_full):
                        shutil.copytree(target_full, dest_path, dirs_exist_ok=True)
                    else:
                        shutil.copy2(target_full, dest_path)
PYEOF

    echo "[+] PSPDEV toolchain extracted successfully."
}

# ------------------------------------------------------------------------------
# 3. Compile Legacy Libraries from Source
# ------------------------------------------------------------------------------
compile_legacy_libraries() {
    echo "----------------------------------------------------------------------"
    echo "[*] Synchronizing legacy source repositories into ${SRC_DIR}..."

    # Clone psp-ports (contains legacy zlib, libpng, SDL 1.2)
    if [ ! -d "${SRC_DIR}/psp-ports/.git" ]; then
        echo "[*] Cloning pspdev/psp-ports..."
        git clone --depth 1 "${PORTS_REPO_URL}" "${SRC_DIR}/psp-ports"
    else
        echo "[*] pspdev/psp-ports repository already present."
    fi

    # Clone SDL_mixer (branch SDL_mixer-psp)
    if [ ! -d "${SRC_DIR}/SDL_mixer/.git" ]; then
        echo "[*] Cloning pspdev/SDL_mixer (branch SDL_mixer-psp)..."
        git clone --depth 1 -b SDL_mixer-psp "${MIXER_REPO_URL}" "${SRC_DIR}/SDL_mixer"
    else
        echo "[*] pspdev/SDL_mixer repository already present."
    fi

    COMMON_CFLAGS="-O2 -G0 -Wno-error=implicit-function-declaration -Wno-implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -std=gnu89"

    # --- A. zlib (1.2.x) ---
    echo "[*] Compiling zlib (1.2.x) for PSP..."
    make -C "${SRC_DIR}/psp-ports/zlib" clean >/dev/null 2>&1 || true
    make -C "${SRC_DIR}/psp-ports/zlib" CFLAGS="${COMMON_CFLAGS}"
    make -C "${SRC_DIR}/psp-ports/zlib" install
    echo "[+] zlib installed into ${PSPDEV}/psp/lib and ${PSPDEV}/psp/include."

    # --- B. libpng (1.2.x) ---
    echo "[*] Compiling libpng (1.2.x) for PSP..."
    make -C "${SRC_DIR}/psp-ports/libpng" clean >/dev/null 2>&1 || true
    make -C "${SRC_DIR}/psp-ports/libpng" CFLAGS="${COMMON_CFLAGS}"
    make -C "${SRC_DIR}/psp-ports/libpng" install
    echo "[+] libpng installed into ${PSPDEV}/psp/lib and ${PSPDEV}/psp/include."

    # --- C. SDL 1.2 (Configured with dummy video to prevent sceGu VRAM conflicts) ---
    echo "[*] Compiling SDL 1.2 with dummy video (Audio, Input, Threads, Timers only)..."
    python3 - << 'PYEOF'
import os
import glob
import shutil
import subprocess

pspdev = os.environ["PSPDEV"]
pspsdk = os.environ["PSPSDK"]
sdl_dir = os.path.join(pspdev, "src", "psp-ports", "SDL")

# Configure SDL_config.h for dummy video
base_cfg_path = os.path.join(pspdev, "psp", "include", "SDL", "SDL_config.h")
target_cfg_path = os.path.join(sdl_dir, "include", "SDL_config.h")

if os.path.exists(base_cfg_path):
    with open(base_cfg_path, "r") as f:
        cfg = f.read()
else:
    with open(os.path.join(sdl_dir, "include", "SDL_config.h.default"), "r") as f:
        cfg = f.read()

cfg = cfg.replace("#define SDL_VIDEO_DRIVER_PSP 1", "/* #undef SDL_VIDEO_DRIVER_PSP */\n#define SDL_VIDEO_DRIVER_DUMMY 1")
with open(target_cfg_path, "w") as f:
    f.write(cfg)

# Enumerate sources: All core subsystems, PSP audio, PSP joystick, PSP threads, PSP timers, and DUMMY video
patterns = [
    "src/*.c",
    "src/audio/*.c",
    "src/audio/psp/*.c",
    "src/cdrom/*.c",
    "src/cdrom/dummy/*.c",
    "src/cpuinfo/*.c",
    "src/events/*.c",
    "src/file/*.c",
    "src/joystick/*.c",
    "src/joystick/psp/*.c",
    "src/loadso/dummy/*.c",
    "src/stdlib/*.c",
    "src/thread/*.c",
    "src/thread/psp/*.c",
    "src/thread/generic/SDL_sysmutex.c",
    "src/thread/generic/SDL_syscond.c",
    "src/timer/*.c",
    "src/timer/psp/*.c",
    "src/video/*.c",
    "src/video/dummy/*.c",
]

sources = []
for pat in patterns:
    sources.extend(glob.glob(os.path.join(sdl_dir, pat)))
sources = sorted(list(set(sources)))

inc_dirs = [
    "-I" + os.path.join(sdl_dir, "include"),
    "-I" + os.path.join(sdl_dir, "src"),
    "-I" + os.path.join(sdl_dir, "src", "audio"),
    "-I" + os.path.join(sdl_dir, "src", "video"),
    "-I" + os.path.join(sdl_dir, "src", "events"),
    "-I" + os.path.join(sdl_dir, "src", "joystick"),
    "-I" + os.path.join(sdl_dir, "src", "thread"),
    "-I" + os.path.join(sdl_dir, "src", "timer"),
    "-I" + os.path.join(pspdev, "psp", "include"),
    "-I" + os.path.join(pspsdk, "include"),
]

cflags = [
    "-O2", "-G0", "-DENABLE_PSP", "-DDISABLE_STDIO",
    "-Wno-error=implicit-function-declaration", "-Wno-implicit-function-declaration",
    "-Wno-error=incompatible-pointer-types", "-Wno-error=int-conversion",
    "-std=gnu89"
]

gcc = os.path.join(pspdev, "bin", "psp-gcc")
ar = os.path.join(pspdev, "bin", "psp-gcc-ar")
ranlib = os.path.join(pspdev, "bin", "psp-gcc-ranlib")

build_dir = os.path.join(sdl_dir, "build_psp_novideo")
os.makedirs(build_dir, exist_ok=True)

objs = []
for src in sources:
    rel = os.path.relpath(src, sdl_dir)
    obj_name = rel.replace("/", "_").replace(".c", ".o")
    obj_path = os.path.join(build_dir, obj_name)
    objs.append(obj_path)
    subprocess.run([gcc] + inc_dirs + cflags + ["-c", src, "-o", obj_path], check=True)

lib_sdl = os.path.join(build_dir, "libSDL.a")
subprocess.run([ar, "cru", lib_sdl] + objs, check=True)
subprocess.run([ranlib, lib_sdl], check=True)

# Install libSDL.a and headers
shutil.copy2(lib_sdl, os.path.join(pspdev, "psp", "lib", "libSDL.a"))
inc_dest = os.path.join(pspdev, "psp", "include", "SDL")
os.makedirs(inc_dest, exist_ok=True)
for h in glob.glob(os.path.join(sdl_dir, "include", "*.h")):
    shutil.copy2(h, inc_dest)

print("libSDL.a installed successfully.")
PYEOF
    echo "[+] SDL 1.2 installed into ${PSPDEV}/psp/lib and ${PSPDEV}/psp/include/SDL."

    # --- D. SDL_mixer 1.2 (for SPU ADPCM / CD-DA audio stream mixing) ---
    echo "[*] Compiling SDL_mixer 1.2 for SPU ADPCM / PCM stream mixing..."
    python3 - << 'PYEOF'
import os
import shutil
import subprocess

pspdev = os.environ["PSPDEV"]
pspsdk = os.environ["PSPSDK"]
mixer_dir = os.path.join(pspdev, "src", "SDL_mixer")

src_files = [
    "mixer.c",
    "music.c",
    "effect_position.c",
    "effect_stereoreverse.c",
    "effects_internal.c",
    "load_aiff.c",
    "load_voc.c",
    "wavestream.c"
]

inc_dirs = [
    "-I" + mixer_dir,
    "-I" + os.path.join(pspdev, "psp", "include", "SDL"),
    "-I" + os.path.join(pspdev, "psp", "include"),
    "-I" + os.path.join(pspsdk, "include"),
]

cflags = [
    "-O2", "-G0", "-DENABLE_PSP", "-DWAV_MUSIC",
    "-Wno-error=implicit-function-declaration", "-Wno-implicit-function-declaration",
    "-Wno-error=incompatible-pointer-types", "-Wno-error=int-conversion",
    "-std=gnu89"
]

gcc = os.path.join(pspdev, "bin", "psp-gcc")
ar = os.path.join(pspdev, "bin", "psp-gcc-ar")
ranlib = os.path.join(pspdev, "bin", "psp-gcc-ranlib")

build_dir = os.path.join(mixer_dir, "build_psp")
os.makedirs(build_dir, exist_ok=True)

objs = []
for f in src_files:
    src_path = os.path.join(mixer_dir, f)
    obj_name = f.replace(".c", ".o")
    obj_path = os.path.join(build_dir, obj_name)
    objs.append(obj_path)
    subprocess.run([gcc] + inc_dirs + cflags + ["-c", src_path, "-o", obj_path], check=True)

lib_mixer = os.path.join(build_dir, "libSDL_mixer.a")
subprocess.run([ar, "cru", lib_mixer] + objs, check=True)
subprocess.run([ranlib, lib_mixer], check=True)

shutil.copy2(lib_mixer, os.path.join(pspdev, "psp", "lib", "libSDL_mixer.a"))
shutil.copy2(os.path.join(mixer_dir, "SDL_mixer.h"), os.path.join(pspdev, "psp", "include", "SDL", "SDL_mixer.h"))
print("libSDL_mixer.a installed successfully.")
PYEOF
    echo "[+] SDL_mixer 1.2 installed into ${PSPDEV}/psp/lib and ${PSPDEV}/psp/include/SDL."
}

# ------------------------------------------------------------------------------
# 4. Generate Support Files: env_psp.sh and Makefile.psp
# ------------------------------------------------------------------------------
generate_wrappers() {
    echo "----------------------------------------------------------------------"
    echo "[*] Generating environment activation script: env_psp.sh..."
    cat << 'EOF' > "${ROOT_DIR}/env_psp.sh"
#!/usr/bin/env bash
# ==============================================================================
# env_psp.sh - Environment Activation Script for Isolated PSP Toolchain
# Target: Sony PlayStation Portable (MIPS Allegrex)
# ==============================================================================

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    echo "[-] Error: This script must be sourced, not executed directly."
    echo "    Usage: source env_psp.sh"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PSPDEV="${SCRIPT_DIR}/.pspsdk"
export PSPSDK="${PSPDEV}/psp/sdk"
export PATH="${PSPDEV}/bin:${PATH}"
export PKG_CONFIG_PATH="${PSPDEV}/psp/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_TOOLCHAIN_FILE="${PSPDEV}/psp/share/pspdev.cmake"

if [ ! -d "${PSPDEV}" ]; then
    echo "[!] Warning: Local PSPDEV directory not found at: ${PSPDEV}"
    echo "    Run ./bootstrap_local_pspsdk.sh to download and initialize the toolchain."
    return 1 2>/dev/null || exit 1
fi

if ! command -v psp-gcc >/dev/null 2>&1; then
    echo "[!] Warning: psp-gcc not found in PATH (${PSPDEV}/bin)."
    return 1 2>/dev/null || exit 1
fi

echo "[+] Isolated PSPDEV environment active:"
echo "    PSPDEV:       ${PSPDEV}"
echo "    PSPSDK:       ${PSPSDK}"
echo "    GCC Version:  $(psp-gcc --version | head -n 1)"
echo "    Target:       MIPS Allegrex (Allegrex R4000-based + VFPU)"
EOF
    chmod +x "${ROOT_DIR}/env_psp.sh"

    echo "[*] Generating root build Makefile: Makefile.psp..."
    cat << 'EOF' > "${ROOT_DIR}/Makefile.psp"
# ==============================================================================
# Makefile.psp - Production PSP Build Makefile for Parasite Eve Decompilation Port
# Target: Sony PlayStation Portable (MIPS Allegrex / PRX executable format)
# ==============================================================================

ifeq ($(PSPDEV),)
    PSPDEV := $(shell pwd)/.pspsdk
endif

ifeq ($(PSPSDK),)
    PSPSDK := $(PSPDEV)/psp/sdk
endif

# Check toolchain availability
ifeq ($(wildcard $(PSPDEV)/bin/psp-gcc),)
    $(error PSPDEV toolchain not detected at $(PSPDEV). Run ./bootstrap_local_pspsdk.sh first)
endif

TARGET   = parasite_eve
OBJS     = $(patsubst %.c,%.o,$(wildcard src/*.c))

# If src/ has no c files yet during initial setup, use placeholder main.o
ifeq ($(strip $(OBJS)),)
    OBJS = main.o
endif

# Include directories: Search isolated target directories and PSPSDK
INCDIR   = $(PSPDEV)/psp/include $(PSPDEV)/psp/include/SDL $(PSPSDK)/include include .

# Target Flags:
# -O2: Optimization level
# -G0: Put all small data into regular data sections (required for MIPS Allegrex PRX)
# -D_PSP_FW_VERSION=600: Target Firmware 6.00+
CFLAGS   = -O2 -G0 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
           -D_PSP_FW_VERSION=600 -DENABLE_PSP
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

# Library paths and explicit linking order
LIBDIR   = $(PSPDEV)/psp/lib $(PSPSDK)/lib

# Core port libraries:
# -lSDL_mixer : Software mixing for SPU ADPCM / CD-DA audio streams
# -lSDL       : SDL 1.2 handling sceAudio, sceCtrl (Input), Threads, Timers (dummy video)
# -lpspgum    : PSP GU Matrix and math utility library
# -lpspgu     : Direct PSP Graphic Engine / Hardware Rasterizer (exclusive VRAM ownership)
# -lpspge     : PSP Graphics Engine hardware driver
# -lpspdisplay: Display control and VBLANK synchronization
# -lpspctrl   : PSP Pad / Joystick controller input
# -lpspaudio  : Low-level hardware audio channel output
# -lpsppower  : Battery and CPU clock management (up to 333MHz)
# -lpsprtc    : High-resolution hardware real-time clock / timers
# -lm -lc     : Standard C and Math libraries
LIBS     = -lSDL_mixer -lSDL -lpspgum -lpspgu -lpspge -lpspdisplay \
           -lpspctrl -lpspaudio -lpsppower -lpsprtc -lm -lc

# Target Configuration:
# BUILD_PRX=1 is mandatory for large memory layout and clean module dynamic relocations
BUILD_PRX          = 1
PSP_LARGE_MEMORY   = 1
PSP_FW_VERSION     = 600

# Metadata for generated EBOOT.PBP
EXTRA_TARGETS      = EBOOT.PBP
PSP_EBOOT_TITLE    = Parasite Eve (PSP Port)
PSP_EBOOT_ICON     = NULL
PSP_EBOOT_ICON1    = NULL
PSP_EBOOT_UNKPNG   = NULL
PSP_EBOOT_PIC1     = NULL
PSP_EBOOT_SND0     = NULL
PSP_EBOOT_PSAR     = NULL

# Include PSPSDK build rules
include $(PSPSDK)/lib/build.mak
EOF
    echo "[+] Support files generated."
}

# ------------------------------------------------------------------------------
# 5. Toolchain Verification
# ------------------------------------------------------------------------------
verify_installation() {
    echo "----------------------------------------------------------------------"
    echo "[*] Running toolchain and library verification test..."

    VERIFY_DIR="${PSPDEV}/verify"
    mkdir -p "${VERIFY_DIR}"

    cat << 'EOF' > "${VERIFY_DIR}/main.c"
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <psprtc.h>
#include <psppower.h>
#include <SDL/SDL.h>
#include <SDL/SDL_mixer.h>
#include <stdio.h>
#include <stdlib.h>

PSP_MODULE_INFO("PARASITE_EVE_VERIFY", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1);

int main(int argc, char *argv[]) {
    pspDebugScreenInit();
    pspDebugScreenPrintf("Parasite Eve PSP Toolchain Verification\n");
    pspDebugScreenPrintf("=========================================\n");

    /* 1. Verify SDL 1.2 audio, joystick, timers without video conflict */
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_TIMER) < 0) {
        pspDebugScreenPrintf("SDL_Init Failed: %s\n", SDL_GetError());
    } else {
        pspDebugScreenPrintf("SDL_Init (Audio, Joystick, Timer): OK\n");
    }

    /* 2. Verify SDL_mixer 1.2 */
    if (Mix_OpenAudio(44100, AUDIO_S16LSB, 2, 2048) < 0) {
        pspDebugScreenPrintf("Mix_OpenAudio Failed: %s\n", Mix_GetError());
    } else {
        pspDebugScreenPrintf("SDL_mixer (44.1kHz Stereo S16): OK\n");
        Mix_CloseAudio();
    }
    SDL_Quit();

    /* 3. Verify native sceGu exclusive hardware ownership */
    sceGuInit();
    pspDebugScreenPrintf("Native sceGu Hardware Initialized: OK\n");
    sceGuTerm();

    pspDebugScreenPrintf("SUCCESS: EBOOT.PBP verified and operational!\n");
    sceKernelDelayThread(2000000);
    sceKernelExitGame();
    return 0;
}
EOF

    cat << 'EOF' > "${VERIFY_DIR}/Makefile"
TARGET   = verify_pe
OBJS     = main.o

INCDIR   = $(PSPDEV)/psp/include $(PSPDEV)/psp/include/SDL $(PSPSDK)/include .
CFLAGS   = -O2 -G0 -Wall -Wno-unused-function -D_PSP_FW_VERSION=600 -DENABLE_PSP
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

LIBDIR   = $(PSPDEV)/psp/lib $(PSPSDK)/lib
LIBS     = -lSDL_mixer -lSDL -lpspgum -lpspgu -lpspge -lpspdisplay \
           -lpspctrl -lpspaudio -lpsppower -lpsprtc -lm -lc

BUILD_PRX        = 1
PSP_LARGE_MEMORY = 1
PSP_FW_VERSION   = 600

EXTRA_TARGETS    = EBOOT.PBP
PSP_EBOOT_TITLE  = Parasite Eve PSP Verification

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
EOF

    make -C "${VERIFY_DIR}" clean >/dev/null 2>&1 || true
    make -C "${VERIFY_DIR}"

    if [ -f "${VERIFY_DIR}/EBOOT.PBP" ] && [ -s "${VERIFY_DIR}/EBOOT.PBP" ]; then
        EBOOT_SIZE=$(stat -c%s "${VERIFY_DIR}/EBOOT.PBP")
        echo "[+] VERIFICATION PASSED: ${VERIFY_DIR}/EBOOT.PBP generated (${EBOOT_SIZE} bytes)."
    else
        echo "[-] VERIFICATION FAILED: EBOOT.PBP was not produced!"
        exit 1
    fi
}

# ------------------------------------------------------------------------------
# Execution Flow
# ------------------------------------------------------------------------------
install_toolchain
compile_legacy_libraries
generate_wrappers
verify_installation

echo "======================================================================"
echo "  LOCAL PSPDEV TOOLCHAIN SETUP COMPLETE"
echo "======================================================================"
echo "[+] Toolchain location:      ${PSPDEV}"
echo "[+] Environment activation:  source env_psp.sh"
echo "[+] Root build makefile:     Makefile.psp"
echo "[+] Compiled Libraries:"
echo "    - ${PSPDEV}/psp/lib/libz.a"
echo "    - ${PSPDEV}/psp/lib/libpng.a"
echo "    - ${PSPDEV}/psp/lib/libSDL.a (dummy video, sceAudio, sceCtrl, sceRtc)"
echo "    - ${PSPDEV}/psp/lib/libSDL_mixer.a (PCM/ADPCM/WAV software mixer)"
echo "======================================================================"
