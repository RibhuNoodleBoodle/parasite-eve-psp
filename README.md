# Parasite Eve — PlayStation Portable (PSP) Native Port & Decompilation

[![Status: Work in Progress](https://img.shields.io/badge/Status-Work%20in%20Progress-yellow.svg)](#project-status)
[![Platform: Sony PSP](https://img.shields.io/badge/Platform-Sony%20PSP-003791?logo=playstation&logoColor=white)](https://github.com/pspdev/pspdev)
[![Architecture: MIPS Allegrex](https://img.shields.io/badge/Architecture-MIPS%20Allegrex-blue)](#overview)
[![License: Clean-Room](https://img.shields.io/badge/License-Clean--Room-lightgrey)](LICENSE.md)

> [!WARNING]
> **Work in Progress:** This project is an ongoing, experimental native C port and reverse-engineered decompilation of *Parasite Eve* (PS1 / `SLUS-00662`) for the Sony PlayStation Portable (PSP). Many systems are functioning (including the Title Screen, STR/MDEC FMV playback, and stage loader transitions), but active development is underway and the game is not yet fully playable from start to finish.

---

## Overview

This repository provides:
1. **A native PSP port (`EBOOT.PBP`)**: Runs natively on PSP hardware (MIPS Allegrex) without PS1 emulation, utilizing native `sceGu` hardware rasterization, SPU audio mixing, and in-place MDEC video decoding.
2. **PS1 Byte-Matching Decompilation**: Clean-room C source code matching the original North American PS1 retail release (`SLUS-00662`).

---

## Project Status

- [x] **Hermetic Build Toolchain**: Fully isolated `.pspsdk` environment; zero root permissions or system modifications required.
- [x] **FMV & Video Decoder**: In-place 15 fps MDEC / STR video streaming with authentic PS1 IDCT quantization matrices in 4:3 (`362x272` centered) aspect ratio.
- [x] **Title Screen & Menus**: Authentic 24-bit RGB rendering for the Title Screen and memory card screens.
- [x] **Stage Transition**: "New Game" successfully launches the opening movie and transitions into the Carnegie Hall theater scene (`room_m005_theater`).
- [x] **Audio**: 44.1 kHz 16-bit stereo mixing, ADPCM decoding, Gaussian interpolation, and CD-XA audio streaming.
- [ ] **Full Playability**: In-progress decompilation of remaining room scripts, battle logic, and cinematics.

---

## Quick Start (Building for PSP)

### 1. Bootstrap Local Toolchain
Run the automated bootstrap script to set up the hermetic PSP toolchain in `.pspsdk/`:
```bash
./bootstrap_local_pspsdk.sh
source env_psp.sh
```

### 2. Build `EBOOT.PBP`
Compile the native executable:
```bash
make -f Makefile.psp
```
This generates `EBOOT.PBP` ready for PSP hardware or emulators.

### 3. Testing with PPSSPP (Optional)
To test locally on Linux with the included portable PPSSPP setup:
```bash
./debug_ppsspp.sh
```

---

## Disc Image & Save Setup

This project does **not** include copyrighted game assets. You must provide legally obtained raw bin/cue dumps of your game discs.

1. **Copy `EBOOT.PBP` to your PSP Memory Stick**:
   ```text
   ms0:/PSP/GAME/ParasiteEve/EBOOT.PBP
   ```
2. **Place raw disc images in the `disc/` directory**:
   ```text
   ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC1.bin
   ms0:/PSP/GAME/ParasiteEve/disc/PE_DISC2.bin
   ```
   *(Uppercase `.BIN` filenames are also supported).*
3. **Save Data**:
   Memory Card saves are automatically persisted to:
   ```text
   ms0:/PSP/SAVEDATA/SLUS00662/SLUS00662.mcd
   ```

---

## Controls

| PSP Button | PS1 Mapping | Description |
|---|---|---|
| **D-Pad / Analog Nub** | D-Pad / Movement | Directional movement (12,000 deadzone) |
| **Cross ($\times$)** | Cross ($\times$) | Confirm / Run / Fire |
| **Circle ($\bigcirc$)** | Circle ($\bigcirc$) | Cancel |
| **Square ($\square$)** | Square ($\square$) | Menu / Status |
| **Triangle ($\triangle$)** | Triangle ($\triangle$) | Map / Secondary |
| **L-Trigger** | L1 | Left shoulder |
| **R-Trigger** | R1 | Right shoulder / Target lock |
| **Select + L-Trigger** | **L2** | Chorded shoulder modifier |
| **Select + R-Trigger** | **R2** | Chorded shoulder modifier |
| **Select + Start** | Turbo | Toggle fast-forward |
| **Select + L + R** | Disc Swap | Hot-swap between Disc 1 and Disc 2 |
| **Start** | Start | Pause / Menu |
| **HOME** | HOME | Safe exit to XMB |

---

## Original PS1 Byte-Matching Decompilation

To build and verify byte-matching PS1 binaries:
```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

mkdir -p assets/USA
cp /path/to/your/SLUS_006.62 assets/USA/main.exe

make split
make build
make check
```

---

## Disclaimer & Legal

- *Parasite Eve* is a registered trademark of Square Enix Co., Ltd.
- This repository is a clean-room educational reverse-engineering and decompilation project.
- No copyrighted game assets, audio tracks, disc sectors, or proprietary binaries are hosted in this repository.
