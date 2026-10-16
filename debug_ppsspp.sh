#!/usr/bin/env bash
# ==============================================================================
# debug_ppsspp.sh - Automated PPSSPP Emulator Runner & Telemetry Analyzer
# ==============================================================================
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_FILE="$ROOT_DIR/ppsspp_debug.log"
CONFIG_FILE="$ROOT_DIR/ppsspp_debug.ini"
EBOOT_PATH="$ROOT_DIR/dist/PSP/GAME/ParasiteEve/EBOOT.PBP"
TIMEOUT_SECS=8

echo "=== [Step 1] Verifying ppsspp-emulator Binary ==="
PPSSPP_BIN="$(which ppsspp-emulator 2>/dev/null || true)"
if [[ -z "$PPSSPP_BIN" ]]; then
    echo "ERROR: ppsspp-emulator not found on system PATH!" >&2
    exit 1
fi

echo "ppsspp-emulator detected at: $PPSSPP_BIN"
"$PPSSPP_BIN" --version || true

echo ""
echo "=== [Step 2] Configuring Telemetry & Environment ==="
if [[ ! -f "$EBOOT_PATH" ]]; then
    echo "Target EBOOT not found at $EBOOT_PATH. Building via Makefile.psp..."
    make -f Makefile.psp
fi

# Ensure dist/ has disc files
mkdir -p "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc"
if [[ -f "$ROOT_DIR/disc/PE_DISC1.bin" ]]; then
    ln -sf "$ROOT_DIR/disc/PE_DISC1.bin" "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc/PE_DISC1.bin" 2>/dev/null || true
    ln -sf "$ROOT_DIR/disc/PE_DISC1.bin" "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc/PE_DISC1.BIN" 2>/dev/null || true
fi
if [[ -f "$ROOT_DIR/disc/PE_DISC2.BIN" ]]; then
    ln -sf "$ROOT_DIR/disc/PE_DISC2.BIN" "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc/PE_DISC2.BIN" 2>/dev/null || true
    ln -sf "$ROOT_DIR/disc/PE_DISC2.BIN" "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc/PE_DISC2.bin" 2>/dev/null || true
elif [[ -f "$ROOT_DIR/disc/PE_DISC2.bin" ]]; then
    ln -sf "$ROOT_DIR/disc/PE_DISC2.bin" "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc/PE_DISC2.bin" 2>/dev/null || true
    ln -sf "$ROOT_DIR/disc/PE_DISC2.bin" "$ROOT_DIR/dist/PSP/GAME/ParasiteEve/disc/PE_DISC2.BIN" 2>/dev/null || true
fi

# Synchronize with Snap PPSSPP base directory if present
SNAP_PPSSPP_DIR="/home/ribhu/snap/ppsspp-emulator/25/.config/ppsspp/PSP/GAME/ParasiteEve"
if [[ -d "/home/ribhu/snap/ppsspp-emulator/25/.config/ppsspp" ]]; then
    mkdir -p "$SNAP_PPSSPP_DIR/disc"
    cp -f "$EBOOT_PATH" "$SNAP_PPSSPP_DIR/EBOOT.PBP" 2>/dev/null || true
    if [[ -f "$ROOT_DIR/disc/PE_DISC1.bin" ]]; then
        ln -sf "$ROOT_DIR/disc/PE_DISC1.bin" "$SNAP_PPSSPP_DIR/disc/PE_DISC1.bin" 2>/dev/null || true
        ln -sf "$ROOT_DIR/disc/PE_DISC1.bin" "$SNAP_PPSSPP_DIR/disc/PE_DISC1.BIN" 2>/dev/null || true
    fi
    if [[ -f "$ROOT_DIR/disc/PE_DISC2.BIN" ]]; then
        ln -sf "$ROOT_DIR/disc/PE_DISC2.BIN" "$SNAP_PPSSPP_DIR/disc/PE_DISC2.BIN" 2>/dev/null || true
        ln -sf "$ROOT_DIR/disc/PE_DISC2.BIN" "$SNAP_PPSSPP_DIR/disc/PE_DISC2.bin" 2>/dev/null || true
    fi
fi

# Also synchronize with ~/.config/ppsspp if present
USER_PPSSPP_DIR="$HOME/.config/ppsspp/PSP/GAME/ParasiteEve"
if [[ -d "$HOME/.config/ppsspp" ]]; then
    mkdir -p "$USER_PPSSPP_DIR/disc"
    cp -f "$EBOOT_PATH" "$USER_PPSSPP_DIR/EBOOT.PBP" 2>/dev/null || true
    if [[ -f "$ROOT_DIR/disc/PE_DISC1.bin" ]]; then
        ln -sf "$ROOT_DIR/disc/PE_DISC1.bin" "$USER_PPSSPP_DIR/disc/PE_DISC1.bin" 2>/dev/null || true
        ln -sf "$ROOT_DIR/disc/PE_DISC1.bin" "$USER_PPSSPP_DIR/disc/PE_DISC1.BIN" 2>/dev/null || true
    fi
    if [[ -f "$ROOT_DIR/disc/PE_DISC2.BIN" ]]; then
        ln -sf "$ROOT_DIR/disc/PE_DISC2.BIN" "$USER_PPSSPP_DIR/disc/PE_DISC2.BIN" 2>/dev/null || true
        ln -sf "$ROOT_DIR/disc/PE_DISC2.BIN" "$USER_PPSSPP_DIR/disc/PE_DISC2.bin" 2>/dev/null || true
    fi
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

echo "Target EBOOT: $EBOOT_PATH"
echo "Logging output to: $LOG_FILE"

echo ""
echo "=== [Step 3] Running ppsspp-emulator ($TIMEOUT_SECS seconds) ==="
rm -f "$LOG_FILE"

# Launch ppsspp-emulator with verbose logging
"$PPSSPP_BIN" --appendconfig="$CONFIG_FILE" -v --log="$LOG_FILE" "$EBOOT_PATH" &
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
grep -E "\[PSP_HOST\]|\[PSP_INPUT\]|\[PSP_AUDIO\]|\[PSP_CDROM\]|\[PSP_GPU\]|\[PSP_TITLE\]" "$LOG_FILE" | head -n 40 || true

echo ""
echo "--- File I/O & Loader Status ---"
grep -iE "Opened PE_DISC|FATAL|ERRNO_FILE_NOT_FOUND|E\[FILESYS\]|E\[LOADER\]" "$LOG_FILE" | head -n 15 || echo "No file errors."

echo ""
echo "--- Framebuffer & GE Display List Status ---"
grep -iE "Display disabled|displaying only black|Framebuf|Switching render target|Displaying FBO|sceDisplaySetFrameBuf" "$LOG_FILE" | head -n 20 || echo "No framebuffer markers found."

echo ""
echo "=== Telemetry Inspection Finished ==="
