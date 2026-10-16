#!/usr/bin/env bash
# ==============================================================================
# debug_ppsspp.sh - Automated PPSSPP Portable Runner & Telemetry Analyzer
# ==============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPSSPP_DIR="$ROOT_DIR/.ppsspp"
PPSSPP_BIN="$PPSSPP_DIR/PPSSPP.AppImage"
EXTRACTED_BIN="$PPSSPP_DIR/AppDir/AppRun"
LOG_FILE="$PPSSPP_DIR/ppsspp_debug.log"
CONFIG_FILE="$PPSSPP_DIR/ppsspp_debug.ini"
EBOOT_PATH="$ROOT_DIR/dist/PSP/GAME/ParasiteEve/EBOOT.PBP"
TIMEOUT_SECS=10

mkdir -p "$PPSSPP_DIR"

echo "=== [Step 1] Verifying Portable PPSSPP Binary ==="
if [[ ! -f "$PPSSPP_BIN" && ! -f "$EXTRACTED_BIN" ]]; then
    echo "Downloading portable PPSSPP AppImage..."
    APPIMAGE_URL="https://github.com/hrydgard/ppsspp/releases/download/v1.20.4/PPSSPP-v1.20.4-anylinux-x86_64.AppImage"
    curl -fsSL -o "$PPSSPP_BIN" "$APPIMAGE_URL"
    chmod +x "$PPSSPP_BIN"
fi

RUNNER_BIN=""
if [[ -f "$PPSSPP_BIN" ]]; then
    chmod +x "$PPSSPP_BIN" 2>/dev/null || true
    # Test if AppImage runs directly (FUSE available)
    if "$PPSSPP_BIN" --version >/dev/null 2>&1; then
        RUNNER_BIN="$PPSSPP_BIN"
    else
        echo "FUSE not available or direct execution failed; checking extracted AppDir..."
        if [[ ! -f "$EXTRACTED_BIN" ]]; then
            echo "Extracting AppImage into $PPSSPP_DIR/AppDir..."
            (cd "$PPSSPP_DIR" && "$PPSSPP_BIN" --appimage-extract >/dev/null 2>&1 || true)
            if [[ -d "$PPSSPP_DIR/squashfs-root" ]]; then
                rm -rf "$PPSSPP_DIR/AppDir"
                mv "$PPSSPP_DIR/squashfs-root" "$PPSSPP_DIR/AppDir"
            fi
        fi
        if [[ -f "$EXTRACTED_BIN" ]]; then
            RUNNER_BIN="$EXTRACTED_BIN"
        fi
    fi
elif [[ -f "$EXTRACTED_BIN" ]]; then
    RUNNER_BIN="$EXTRACTED_BIN"
fi

if [[ -z "$RUNNER_BIN" ]]; then
    echo "ERROR: Unable to find or execute PPSSPP binary!" >&2
    exit 1
fi

echo "PPSSPP Binary: $RUNNER_BIN"
"$RUNNER_BIN" --version

echo ""
echo "=== [Step 2] Configuring Portable Telemetry & MemStick ==="
if [[ ! -f "$EBOOT_PATH" ]]; then
    echo "ERROR: Target EBOOT not found at $EBOOT_PATH" >&2
    echo "Please build the release first using Makefile.psp or build_psp_release.sh" >&2
    exit 1
fi

cat << EOF > "$CONFIG_FILE"
[General]
MemStickDirectory = $ROOT_DIR/dist
MemStickInserted = True
MemStickSize = 16
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
EOF

if [[ -d "/home/ribhu/.config/ppsspp/PSP/GAME/ParasiteEve" ]]; then
    cp -f "$EBOOT_PATH" "/home/ribhu/.config/ppsspp/PSP/GAME/ParasiteEve/EBOOT.PBP" 2>/dev/null || true
    mkdir -p "/home/ribhu/.config/ppsspp/PSP/GAME/ParasiteEve/disc"
    ln -sf "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc/PE_DISC1.bin" "/home/ribhu/.config/ppsspp/PSP/GAME/ParasiteEve/disc/PE_DISC1.bin" 2>/dev/null || true
    ln -sf "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc/PE_DISC2.bin" "/home/ribhu/.config/ppsspp/PSP/GAME/ParasiteEve/disc/PE_DISC2.bin" 2>/dev/null || true
fi

echo "MemStick Directory configured to: $ROOT_DIR/dist"
echo "Target EBOOT: $EBOOT_PATH"
echo "Logging output to: $LOG_FILE"

echo ""
echo "=== [Step 3] Running PPSSPP Telemetry ($TIMEOUT_SECS seconds) ==="
rm -f "$LOG_FILE"

# Launch PPSSPP with verbose logging
"$RUNNER_BIN" --appendconfig="$CONFIG_FILE" -v --log="$LOG_FILE" "$EBOOT_PATH" &
PPSSPP_PID=$!

sleep "$TIMEOUT_SECS" || true

if kill -0 "$PPSSPP_PID" 2>/dev/null; then
    echo "Stopping PPSSPP instance (PID $PPSSPP_PID)..."
    kill -15 "$PPSSPP_PID" 2>/dev/null || true
    sleep 1 || true
    if kill -0 "$PPSSPP_PID" 2>/dev/null; then
        kill -9 "$PPSSPP_PID" 2>/dev/null || true
    fi
fi
wait "$PPSSPP_PID" 2>/dev/null || true

echo "Telemetry capture complete."

echo ""
echo "=== [Step 4] Telemetry & Diagnostic Analysis ==="
if [[ ! -f "$LOG_FILE" ]]; then
    echo "ERROR: Log file $LOG_FILE was not generated!" >&2
    exit 1
fi

echo "--- Module & Boot Status ---"
grep -E "Module ParasiteEve|Booted|entry=|Allocated" "$LOG_FILE" | head -n 10 || true

echo ""
echo "--- PSP Subsystem Console Output ---"
grep -E "\[PSP_HOST\]|\[PSP_INPUT\]|\[PSP_AUDIO\]|\[PSP_CDROM\]|\[PSP_GPU\]" "$LOG_FILE" | head -n 40 || true

echo ""
echo "--- Unimplemented Syscalls & HLE Warnings ---"
grep -iE "unimplemented|unknown syscall|unknown HLE" "$LOG_FILE" | head -n 10 || echo "None found."

echo ""
echo "--- File I/O & Loader Errors ---"
grep -iE "ERRNO_FILE_NOT_FOUND|E\[FILESYS\]|E\[LOADER\]|Failed to load" "$LOG_FILE" | head -n 10 || echo "None found."

echo ""
echo "--- MIPS Exceptions & Crash Indicators ---"
grep -iE "MIPS Exception|invalid opcode|SIGSEGV|Fatal crash" "$LOG_FILE" | head -n 10 || echo "No MIPS crashes detected."

echo ""
echo "--- Framebuffer & GE Display List Status ---"
grep -iE "Display disabled|displaying only black|Framebuf|Switching render target|Displaying FBO|sceDisplaySetFrameBuf" "$LOG_FILE" | head -n 20 || echo "No framebuffer markers found."

echo ""
echo "=== Telemetry Inspection Finished ==="
