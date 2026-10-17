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
echo "Building EBOOT.PBP via Makefile.psp..."
make -f Makefile.psp
cp -f "$ROOT_DIR/EBOOT.PBP" "$EBOOT_PATH"

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
CPUCore = 1
FastMemoryAccess = True
[Graphics]
GraphicsBackend = 0
RenderingMode = 1
SoftwareRendering = False
InternalResolution = 1
DisplayResolution = 1
VerticalSync = False
[Log]
SystemLevel = 3
G3DLevel = 3
HLELevel = 3
IOLevel = 3
FramebufLevel = 3
SCEIO = 3
PRINTF = 6
EOF

echo ""
RUN_TIMEOUT="${RUN_TIMEOUT:-40s}"
echo "=== [Step 3] Running Portable PPSSPP AppImage (${RUN_TIMEOUT} timeout) ==="
rm -f "$LOG_FILE" "$PPSSPP_DIR/ probe"*.log 2>/dev/null || true
rm -f "$ROOT_DIR/autotest.flag" "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/autotest.flag" "$MEMSTICK_GAME_DIR/autotest.flag" 2>/dev/null || true
touch "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/frame_dump_enable.flag" 2>/dev/null || true
touch "$MEMSTICK_GAME_DIR/frame_dump_enable.flag" 2>/dev/null || true

if [[ "${PE_AUTOTEST:-0}" == "1" ]]; then
    rm -f "$ROOT_DIR"/frame_dump*.ppm "$ROOT_DIR/dist"/frame_dump*.ppm "$ROOT_DIR/dist/PSP/GAME/ParasiteEve"/frame_dump*.ppm "$PPSSPP_DIR"/frame_dump*.ppm "$PPSSPP_DIR/.config/ppsspp"/frame_dump*.ppm "$PPSSPP_DIR/.config/ppsspp/PSP/GAME/ParasiteEve"/frame_dump*.ppm 2>/dev/null || true
    touch "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/autotest.flag" 2>/dev/null || true
    touch "$MEMSTICK_GAME_DIR/autotest.flag" 2>/dev/null || true
else
    for name in frame_dump.ppm frame_dump_notice.ppm frame_dump_logo.ppm frame_dump_intro_fmv.ppm; do
        rm -f "$ROOT_DIR/$name" "$ROOT_DIR/dist/$name" "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/$name" "$PPSSPP_DIR/$name" "$PPSSPP_DIR/.config/ppsspp/$name" "$PPSSPP_DIR/.config/ppsspp/PSP/GAME/ParasiteEve/$name" 2>/dev/null || true
    done
fi

if [[ -x "$APPIMAGE_BIN" ]]; then
    HOME="$(pwd)/.ppsspp" PE_AUTOTEST="${PE_AUTOTEST:-0}" timeout "${RUN_TIMEOUT}" ./.ppsspp/*.AppImage --appimage-extract-and-run -v --log="$(pwd)/.ppsspp/ppsspp_debug.log" "$(pwd)/dist/PSP/GAME/ParasiteEve/EBOOT.PBP" || true
else
    HOME="$(pwd)/.ppsspp" PE_AUTOTEST="${PE_AUTOTEST:-0}" timeout "${RUN_TIMEOUT}" /lib64/ld-linux-x86-64.so.2 ./.ppsspp/*.AppImage --appimage-extract-and-run -v --log="$(pwd)/.ppsspp/ppsspp_debug.log" "$(pwd)/dist/PSP/GAME/ParasiteEve/EBOOT.PBP" || true
fi

rm -f "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/frame_dump_enable.flag" "$MEMSTICK_GAME_DIR/frame_dump_enable.flag" 2>/dev/null || true
rm -f "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/autotest.flag" "$MEMSTICK_GAME_DIR/autotest.flag" 2>/dev/null || true

# Sync frame dumps into .ppsspp/, dist/, and project root
for ppm_name in "frame_dump.ppm" "frame_dump_notice.ppm" "frame_dump_logo.ppm" "frame_dump_intro_fmv.ppm" "frame_dump_title.ppm" "frame_dump_newgame.ppm" "frame_dump_carnegie.ppm" "frame_dump_str.ppm"; do
    for candidate_ppm in \
        "$PPSSPP_DIR/.config/ppsspp/PSP/GAME/ParasiteEve/$ppm_name" \
        "$PPSSPP_DIR/.config/ppsspp/$ppm_name" \
        "$PPSSPP_DIR/$ppm_name" \
        "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/$ppm_name" \
        "$ROOT_DIR/dist/$ppm_name" \
        "$ROOT_DIR/$ppm_name"; do
        if [[ -f "$candidate_ppm" ]]; then
            if [[ "$candidate_ppm" != "$PPSSPP_DIR/$ppm_name" ]]; then
                cp -f "$candidate_ppm" "$PPSSPP_DIR/$ppm_name"
            fi
            if [[ "$candidate_ppm" != "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/$ppm_name" ]]; then
                cp -f "$candidate_ppm" "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/$ppm_name"
            fi
            if [[ "$candidate_ppm" != "$ROOT_DIR/$ppm_name" ]]; then
                cp -f "$candidate_ppm" "$ROOT_DIR/$ppm_name"
            fi
            break
        fi
    done
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
echo "--- Running Anti-Cheat Frame Verification (verify_frames.py) ---"
if [[ -f "$ROOT_DIR/verify_frames.py" ]]; then
    python3 "$ROOT_DIR/verify_frames.py"
fi

echo ""
echo "=== Telemetry Analysis Complete ==="
