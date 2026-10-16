#!/usr/bin/env bash
# ==============================================================================
# debug_ppsspp.sh - exFAT-Safe Portable PPSSPP AppImage Runner & Log Analyzer
# ==============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

PPSSPP_DIR="$ROOT_DIR/.ppsspp"
LOG_FILE="$PPSSPP_DIR/ppsspp_debug.log"
INI_DIR="$PPSSPP_DIR/.config/ppsspp/PSP/SYSTEM"
INI_FILE="$INI_DIR/ppsspp.ini"
EBOOT_PATH="$ROOT_DIR/dist/PSP/GAME/ParasiteEve/EBOOT.PBP"
MEMSTICK_GAME_DIR="$PPSSPP_DIR/.config/ppsspp/PSP/GAME/ParasiteEve"

mkdir -p "$PPSSPP_DIR" "$INI_DIR" "$MEMSTICK_GAME_DIR" "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc"

echo "=== [Step 1] Verifying Portable PPSSPP AppImage in .ppsspp/ ==="
shopt -s nullglob
APPIMAGES=("$PPSSPP_DIR"/*.AppImage)
shopt -u nullglob

if [[ ${#APPIMAGES[@]} -eq 0 ]]; then
    echo "Downloading portable PPSSPP Linux x86_64 AppImage into $PPSSPP_DIR/..."
    APPIMAGE_URL="$(curl -sL https://api.github.com/repos/pkgforge-dev/PPSSPP-AppImage/releases/latest \
        | grep -oE 'https://[^"]+x86_64\.AppImage' | head -n 1 || true)"
    if [[ -z "$APPIMAGE_URL" ]]; then
        echo "ERROR: Could not resolve PPSSPP AppImage URL." >&2
        exit 1
    fi
    curl -L "$APPIMAGE_URL" -o "$PPSSPP_DIR/PPSSPP.AppImage"
    chmod +x "$PPSSPP_DIR/PPSSPP.AppImage" 2>/dev/null || true
    APPIMAGES=("$PPSSPP_DIR/PPSSPP.AppImage")
fi

APPIMAGE_BIN="${APPIMAGES[0]}"
echo "Using portable AppImage: $APPIMAGE_BIN"

echo ""
echo "=== [Step 2] Staging EBOOT.PBP & Disc Images (exFAT-safe, zero symlinks) ==="
if [[ ! -f "$EBOOT_PATH" ]]; then
    echo "Building EBOOT.PBP via Makefile.psp..."
    make -f Makefile.psp
    cp -f "$ROOT_DIR/EBOOT.PBP" "$EBOOT_PATH"
fi

# Always keep EBOOT.PBP synced in dist/ and portable memstick GAME directory using cp -f (never ln -s)
if [[ -f "$ROOT_DIR/EBOOT.PBP" ]]; then
    cp -f "$ROOT_DIR/EBOOT.PBP" "$EBOOT_PATH"
fi
cp -f "$EBOOT_PATH" "$MEMSTICK_GAME_DIR/EBOOT.PBP"

# Copy disc files into dist/PSP/GAME/ParasiteEve/disc/ and portable memstick only if not already present (no symlinks)
mkdir -p "$MEMSTICK_GAME_DIR/disc"
for dst_disc_dir in "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc" "$MEMSTICK_GAME_DIR/disc"; do
    if [[ -f "$ROOT_DIR/disc/PE_DISC1.bin" && ! -f "$dst_disc_dir/PE_DISC1.bin" ]]; then
        echo "Copying PE_DISC1.bin to $dst_disc_dir/..."
        cp -f "$ROOT_DIR/disc/PE_DISC1.bin" "$dst_disc_dir/PE_DISC1.bin"
    elif [[ -f "$ROOT_DIR/disc/PE_DISC1.BIN" && ! -f "$dst_disc_dir/PE_DISC1.BIN" ]]; then
        echo "Copying PE_DISC1.BIN to $dst_disc_dir/..."
        cp -f "$ROOT_DIR/disc/PE_DISC1.BIN" "$dst_disc_dir/PE_DISC1.BIN"
    fi

    if [[ -f "$ROOT_DIR/disc/PE_DISC2.BIN" && ! -f "$dst_disc_dir/PE_DISC2.BIN" ]]; then
        echo "Copying PE_DISC2.BIN to $dst_disc_dir/..."
        cp -f "$ROOT_DIR/disc/PE_DISC2.BIN" "$dst_disc_dir/PE_DISC2.BIN"
    elif [[ -f "$ROOT_DIR/disc/PE_DISC2.bin" && ! -f "$dst_disc_dir/PE_DISC2.bin" ]]; then
        echo "Copying PE_DISC2.bin to $dst_disc_dir/..."
        cp -f "$ROOT_DIR/disc/PE_DISC2.bin" "$dst_disc_dir/PE_DISC2.bin"
    fi
done

# Configure portable PPSSPP ppsspp.ini
cat << EOF > "$INI_FILE"
[General]
FirstRun = False
AutoRun = True
Enable Logging = True
IgnoreBadMemAccess = True
MemStickPath = $ROOT_DIR/dist/
MemStickDirectory = $ROOT_DIR/dist
MemStickInserted = True
MemStickSize = 16
[CPU]
CPUCore = 0
FastMemoryAccess = False
[Graphics]
GraphicsBackend = 0
RenderingMode = 1
SoftwareRendering = False
InternalResolution = 1
DisplayResolution = 1
[Log]
SystemLevel = 6
G3DLevel = 6
HLELevel = 6
IOLevel = 6
FramebufLevel = 6
SCEIO = 6
PRINTF = 6
EOF

echo ""
echo "=== [Step 3] Running Portable PPSSPP AppImage (12s timeout) ==="
rm -f "$LOG_FILE" "$PPSSPP_DIR/ probe"*.log 2>/dev/null || true

if [[ -x "$APPIMAGE_BIN" ]]; then
    HOME="$(pwd)/.ppsspp" timeout 12s ./.ppsspp/*.AppImage --appimage-extract-and-run -v --log="$(pwd)/.ppsspp/ppsspp_debug.log" "$(pwd)/dist/PSP/GAME/ParasiteEve/EBOOT.PBP" || true
else
    HOME="$(pwd)/.ppsspp" timeout 12s /lib64/ld-linux-x86-64.so.2 ./.ppsspp/*.AppImage --appimage-extract-and-run -v --log="$(pwd)/.ppsspp/ppsspp_debug.log" "$(pwd)/dist/PSP/GAME/ParasiteEve/EBOOT.PBP" || true
fi

# Sync frame_dump.ppm into .ppsspp/frame_dump.ppm if written via ms0:/ or host0:/
for candidate_ppm in \
    "$PPSSPP_DIR/.config/ppsspp/frame_dump.ppm" \
    "$PPSSPP_DIR/.config/ppsspp/PSP/GAME/ParasiteEve/frame_dump.ppm" \
    "$ROOT_DIR/dist/frame_dump.ppm" \
    "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/frame_dump.ppm"; do
    if [[ -f "$candidate_ppm" ]]; then
        cp -f "$candidate_ppm" "$PPSSPP_DIR/frame_dump.ppm"
        break
    fi
done

echo ""
echo "=== [Step 4] Automated Telemetry & Log Analysis ==="
if [[ ! -f "$LOG_FILE" ]]; then
    echo "ERROR: Log file $LOG_FILE was not generated!" >&2
    exit 1
fi

echo "--- [PE-PSP] & [VRAM-CHECK] Checkpoints ---"
grep -E "\[PE-PSP\]|\[VRAM-CHECK\]|\[PSP_HOST\]|\[PSP_CDROM\]|\[PSP_GPU\]" "$LOG_FILE" || echo "No [PE-PSP] / [VRAM-CHECK] checkpoints found."

echo ""
echo "--- File I/O (IoFileMgr / SCEIO) ---"
grep -E "IoFileMgr|sceIoOpen|sceIoRead|Host0 path" "$LOG_FILE" | head -n 30 || echo "No IoFileMgr lines."

echo ""
echo "--- Errors & Exceptions (ERROR / Exception) ---"
grep -E "ERROR|Exception|FATAL|ERRNO_FILE_NOT_FOUND|E\[LOADER\]|E\[BOOT\]" "$LOG_FILE" | head -n 30 || echo "No errors or exceptions found."

echo ""
echo "=== Telemetry Analysis Complete ==="
