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
