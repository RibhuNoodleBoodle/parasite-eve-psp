# Parasite Eve — PlayStation Portable (PSP) Native Port & Decompilation

[![PSP Target](https://img.shields.io/badge/Platform-Sony%20PSP%20(Allegrex)-003791?logo=playstation&logoColor=white)](https://github.com/pspdev/pspdev)
[![Architecture](https://img.shields.io/badge/Architecture-MIPS%20Allegrex%20R4000%20%2B%20VFPU-blue)](#architecture--subsystems)
[![Toolchain](https://img.shields.io/badge/Toolchain-Isolated%20.pspsdk%20(Hermetic)-brightgreen)](#toolchain-setup)
[![Audio Backend](https://img.shields.io/badge/Audio-SDL%201.2%20%2B%20Native%20SPU-orange)](#4-spu-audio-engine--mixing-psp_sdl_audioc)
[![Graphics Backend](https://img.shields.io/badge/Graphics-Native%20libgu%20%2F%20sceGu-purple)](#2-native-hardware-gpu-rasterizer-psp_gpu_guc)
[![License](https://img.shields.io/badge/License-Clean--Room%20Reimplementation-lightgrey)](LICENSE.md)

A high-performance, native Sony PlayStation Portable (PSP) C port and verified matching decompilation of the North American PlayStation release of **Parasite Eve** (`SLUS-006.62`).

This repository provides both the original PS1 byte-matching decompilation toolchain and the standalone, hermetic PSP embedded engine targeting MIPS Allegrex (`EBOOT.PBP` / PRX).

---

## Table of Contents

- [Overview](#overview)
- [Key Features](#key-features)
- [Architecture & Subsystems](#architecture--subsystems)
  - [1. Host Platform & System Clocks (`psp_sdl_host.c`)](#1-host-platform--system-clocks-psp_sdl_hostc)
  - [2. Native Hardware GPU Rasterizer (`psp_gpu_gu.c`)](#2-native-hardware-gpu-rasterizer-psp_gpu_guc)
  - [3. Input Mapping & Chording (`psp_sdl_input.c`)](#3-input-mapping--chording-psp_sdl_inputc)
  - [4. SPU Audio Engine & Mixing (`psp_sdl_audio.c`)](#4-spu-audio-engine--mixing-psp_sdl_audioc)
  - [5. Async CD-ROM Streaming & Disc Hot-Swap (`psp_cdrom_iso.c`)](#5-async-cd-rom-streaming--disc-hot-swap-psp_cdrom_isoc)
  - [6. FMV STR / MDEC Video Decoder (`psp_cdrom_iso.c`)](#6-fmv-str--mdec-video-decoder-psp_cdrom_isoc)
  - [7. Shared 2 MB Overlay Arena Manager (`psp_overlay_mgr.c`)](#7-shared-2-mb-overlay-arena-manager-psp_overlay_mgrc)
  - [8. Custom PRX Linker Script (`psp_link.ld`)](#8-custom-prx-linker-script-psp_linkld)
- [Memory Layout & Limits (32 MB vs 64 MB)](#memory-layout--limits-32-mb-vs-64-mb)
- [Quick Start: Building for PSP](#quick-start-building-for-psp)
  - [1. Hermetic Toolchain Bootstrap](#1-hermetic-toolchain-bootstrap)
  - [2. Compile the PSP EBOOT.PBP](#2-compile-the-psp-ebootpbp)
  - [3. Installation to PSP Memory Stick](#3-installation-to-psp-memory-stick)
- [Controls & Button Mappings](#controls--button-mappings)
- [Repository Layout](#repository-layout)
- [Original PS1 Matching Decompilation](#original-ps1-matching-decompilation)
- [License & Disclaimer](#license--disclaimer)

---

## Overview

Parasite Eve originally utilized Sony's proprietary Psy-Q library, R3000A MIPS CPU, GTE vector coprocessor, and custom SPU/GPU hardware. Running natively on the PSP without emulation presents unique hardware constraints:
1. **Memory Ceiling:** The PSP-1000 provides only 32 MB total RAM (24 MB user accessible), while PSP-2000/3000 provides 64 MB. Static allocation of all 20+ room overlays and scenes would exhaust RAM immediately.
2. **Display & VRAM Ownership:** Native PSP graphics are driven exclusively by the PSP Graphics Engine (`sceGu`) and EDRAM (2 MB). SDL 1.2 is used strictly for non-graphical subsystems (Audio, Joystick, Timers, Threads) with zero display interference.
3. **Storage Latency:** Streaming 2352-byte raw CD sectors from a Memory Stick requires double-buffering and asynchronous prefetching to avoid stutter during cutscenes and room transitions.

This port completely solves these challenges with zero system-wide package dependencies, zero dynamic heap bloat, and full native MIPS Allegrex hardware acceleration.

---

## Key Features

- **Hermetic Build Environment:** 100% self-contained toolchain inside `$(pwd)/.pspsdk`. Zero `sudo`, `apt`, or global system writes required.
- **Hardware-Accelerated Rasterizer:** Direct rendering via PSP `sceGu` with bilinear/nearest filtering, aspect-correct 4:3 pillarboxing, 16:9 widescreen stretch, and 1:1 pixel-perfect centering.
- **Full PS1 Primitive Support:** Native display list translation for `POLY_F3`, `POLY_FT3`, `POLY_G3`, `POLY_GT3`, `POLY_F4`, `POLY_FT4`, `POLY_G4`, `POLY_GT4`, `LINE_F2`, `LINE_G2`, `SPRT`, `TILE`, and all ABR 0..3 semi-transparency blending modes.
- **Overclocking by Default:** Automatically locks the PSP CPU to 333 MHz and system bus to 166 MHz via `scePowerSetClockFrequency` for fluid 60 fps rendering.
- **Asynchronous CD Streaming:** Dedicated background thread with a 256 KB prefetch ring buffer (110 raw sectors) eliminating drive read stalls.
- **Dual-Disc Hot-Swapping:** Real-time hot-swapping between Disc 1 and Disc 2 via programmatic API or in-game controller chord (`Select + L + R`).
- **In-Place MDEC Movie Player:** Fixed-point 8x8 IDCT decoder streaming 15 fps `STR` video directly to `sceGu` texture buffers with a minimal 1.5 KB static scratch buffer (zero dynamic heap allocation).
- **24-Voice SPU Engine:** 44.1 kHz 16-bit stereo mixing, 4-bit ADPCM decoding with the 512-entry PS1 Gaussian interpolation table, pitch modulation, ADSR envelopes, and CD-XA / STR audio sector mixing.
- **Unified 2 MB Overlay Arena:** Custom PRX linker script (`psp_link.ld`) and overlay manager (`psp_overlay_mgr.c`) mapping all compiled C room, battle, menu, and effect overlays into a single 2 MB shared memory arena.
- **Native Save System:** Transparent persistence of PS1 Memory Card (`bu00:`) sectors and frames directly to `ms0:/PSP/SAVEDATA/SLUS00662/SLUS00662.mcd`.

---

## Architecture & Subsystems

```
                                +-----------------------------+
                                |      Parasite Eve Main      |
                                |       Decompiled Game       |
                                +--------------+--------------+
                                               |
             +--------------------+------------+------------+--------------------+
             |                    |                         |                    |
             v                    v                         v                    v
     +---------------+    +---------------+         +---------------+    +---------------+
     |  psp_gpu_gu   |    | psp_overlay   |         | psp_cdrom_iso |    |  psp_sdl_host |
     |  (libgu/sceGu)|    | (Shared Arena)|         | (Async Reader)|    |  (Clocks/Saves|
     +-------+-------+    +-------+-------+         +-------+-------+    +-------+-------+
             |                    |                         |                    |
             v                    v                         v                    v
     +---------------+    +---------------+         +---------------+    +---------------+
     | PSP 2MB EDRAM |    |  2MB Union    |         | 256KB Ring    |    | ms0:/SAVEDATA |
     | 480x272 PSM555|    |  User Memory  |         | Buffer Thread |    | Memory Stick  |
     +---------------+    +---------------+         +---------------+    +---------------+
                                                            |                    |
                                              +-------------+-------------+      |
                                              |                           |      |
                                              v                           v      v
                                      +---------------+           +---------------+
                                      |   MDEC IDCT   |           | psp_sdl_audio |
                                      |  15 fps Movie |           | (44.1kHz SPU) |
                                      +---------------+           +---------------+
                                                                          |
                                                                          v
                                                                  +---------------+
                                                                  |  psp_sdl_input|
                                                                  |  (Nub + Chords|
                                                                  +---------------+
```

### 1. Host Platform & System Clocks ([`psp_sdl_host.c`](src/psp_sdl_host.c))
- Sets the PSP hardware clock to 333 MHz CPU and 166 MHz Bus frequency.
- Registers kernel exit callback threads (`sceKernelRegisterExitCallback`) so the console safely handles the PSP HOME button.
- Strictly calls `SDL_Init(SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_TIMER | SDL_INIT_NOPARACHUTE)`, omitting `SDL_INIT_VIDEO` to guarantee zero display interference with native `sceGu`.
- Maps PS1 Memory Card (`bu00:`) operations to `ms0:/PSP/SAVEDATA/SLUS00662/SLUS00662.mcd`, providing sector-level (8 KiB) and frame-level (128 bytes) read/write persistence.

### 2. Native Hardware GPU Rasterizer ([`psp_gpu_gu.c`](src/psp_gpu_gu.c))
- Allocates a double-buffered display target in 2 MB PSP EDRAM at 480x272 (`GU_PSM_5551`) with 16-bit depth buffer (`sceGeEdramGetAddr()`).
- Maintains a 1024x512 16-bit software shadow of PS1 VRAM in main RAM with dirty-rectangle tracking and texture uploading via `sceGuCopyImage` and D-cache writeback.
- Translates all PS1 primitive opcodes (`0x20` to `0x7F`) into a 256 KB static `sceGu` display list.
- Hardware semi-transparency blending matches PS1 ABR modes (ABR 0: 50%+50%, ABR 1: 100%+100%, ABR 2: 100%-100%, ABR 3: 100%+25%).
- Configurable presentation: Aspect-correct 4:3 pillarbox, 1:1 pixel-perfect center, and 16:9 widescreen stretch with bilinear or point filtering.

### 3. Input Mapping & Chording ([`psp_sdl_input.c`](src/psp_sdl_input.c))
- Reads digital inputs and the analog nub via `SDL_JoystickOpen(0)`.
- Directional deadzone thresholding (`HOST_PAD_STICK_DEADZONE = 12,000`) allows using the analog nub for digital navigation.
- **Chorded L2/R2 Modifiers:**
  - `Select + L-Trigger` = `HOST_PAD_L2` (suppresses Select and L1).
  - `Select + R-Trigger` = `HOST_PAD_R2` (suppresses Select and R1).
  - `Select + Start` = Fast-forward turbo toggle.
  - Plain `Select` acts as normal `HOST_PAD_SELECT`.
- Synthesizes the full 8-byte PS1 DualShock controller packet (`PspPadPacket`, ID `0x73`).

### 4. SPU Audio Engine & Mixing ([`psp_sdl_audio.c`](src/psp_sdl_audio.c))
- Configures `SDL_OpenAudio` at 44100 Hz, 16-bit stereo (`AUDIO_S16SYS`), 1024-sample hardware buffer.
- Employs a 16,384-frame thread-safe circular ring buffer protected by `SDL_mutex`.
- Full 24-voice SPU hardware emulation:
  - 4-bit ADPCM decoding using the 5 Sony predictor filter pairs (`k0`, `k1`).
  - 4-point Gaussian interpolation using the verified 512-entry PS1 hardware table (`k_gauss`).
  - Voice pitch modulation (PMON), 4-phase ADSR envelopes (Attack, Decay, Sustain, Release), and volume panning.
  - Streaming mixing of asynchronous CD-XA / STR / CD-DA audio sectors.

### 5. Async CD-ROM Streaming & Disc Hot-Swap ([`psp_cdrom_iso.c`](src/psp_cdrom_iso.c))
- Dedicated background worker thread continuously prefetches raw 2352-byte MODE2/2352 sectors into a 256 KB ring buffer (110 sectors).
- Implements non-blocking `CdRead`, `CdSeekL`, `CdControl`, `CdReady`, and `CdPosToInt`.
- **Disc Hot-Swapping:**
  - Programmatic API `PspCdrom_SwapDisc(disc_num)` and in-game button chord (`Select + L + R` or `Select + L2 + R2`) swap between `Disc 1.bin` and `Disc 2.bin` without exiting the game.

### 6. FMV STR / MDEC Video Decoder ([`psp_cdrom_iso.c`](src/psp_cdrom_iso.c))
- Parses Form 2 STR movie sectors and chunk headers (magic `0x0160`).
- Performs in-place fixed-point 8x8 2D IDCT with separable row/column passes and zigzag reordering.
- Converts 4:2:0 YCbCr macroblocks (16x16 pixels) directly into destination `sceGu` texture buffers in 16-bit RGB5551 or 32-bit RGBA8888 format.
- Uses a static 1.5 KB scratch buffer (`int32_t mb_blocks[6][64]`), eliminating dynamic heap allocation.

### 7. Shared 2 MB Overlay Arena Manager ([`psp_overlay_mgr.c`](src/psp_overlay_mgr.c))
- Allocates a single 2 MB shared memory arena (`s_overlay_arena`, 64-byte aligned).
- All 20+ room overlays, battle actors, menu overlays, and FX modules share this arena.
- Automatically handles deactivating previously loaded overlays, clearing the arena, and initializing new overlays.
- Provides demand-paging guest dispatch (`PE_Overlay_SetResident`, `PE_GuestCode_Resolve`, `PE_GuestCall`).

### 8. Custom PRX Linker Script ([`psp_link.ld`](psp_link.ld))
- Configured for MIPS Allegrex PRX (`ORIGIN = 0x08804000, LENGTH = 24M`).
- Features a designated `.overlay_arena (NOLOAD)` section. Marked `NOLOAD` so the 2 MB shared overlay region takes zero file space in the generated `EBOOT.PBP` binary on the Memory Stick.

---

## Memory Layout & Limits (32 MB vs 64 MB)

| Memory Segment | Address Range / Size | Purpose |
|---|---|---|
| **PSP Kernel Reserve** | `0x08000000..0x08804000` (8 MB) | System OS, drivers, thread manager |
| **User PRX Executable** | `0x08804000..` (~4–8 MB) | Code (`.text`), read-only constants (`.rodata`), data (`.data`) |
| **Shared Overlay Arena** | `.overlay_arena` (2 MB, 64-byte aligned) | Transient room, battle, menu BSS/Data (NOLOAD) |
| **CD-ROM Prefetch Ring**| 256 KB | 110 raw 2352-byte sector prefetch cache |
| **SPU Audio Ring Buffer**| ~64 KB | 16,384 stereo frame playback pool |
| **PSP EDRAM** | `0x04000000` (2 MB) | Hardware draw/display/depth buffers (480x272) |
| **User Heap & Stack** | Remaining User RAM | Dynamic allocations, stacks (fits in 24 MB PSP-1000 limit) |

---

## Quick Start: Building for PSP

### 1. Hermetic Toolchain Bootstrap

Run the automated bootstrap script. It fetches the static Linux x86_64 PSPDEV toolchain and cross-compiles `zlib`, `libpng`, `SDL 1.2`, and `SDL_mixer 1.2` locally into `.pspsdk/`:

```bash
./bootstrap_local_pspsdk.sh
```

Activate the environment in your shell:

```bash
source env_psp.sh
```

### 2. Compile the PSP EBOOT.PBP

Compile the native platform engine and packaging targets:

```bash
make -f Makefile.psp
```

This compiles all modules with `-O2 -G0 -Wall -Wextra -D_PSP_FW_VERSION=600 -DENABLE_PSP`, links via `psp_link.ld`, converts to PRX, and generates `EBOOT.PBP`.

### 3. Installation to PSP Memory Stick

Connect your PSP via USB or insert your Memory Stick Duo into your PC:

1. Copy `EBOOT.PBP` to:
   ```text
   ms0:/PSP/GAME/ParasiteEve/EBOOT.PBP
   ```
2. Place your legally dumped raw BIN/CUE disc images in:
   ```text
   ms0:/PSP/GAME/ParasiteEve/disc/Parasite Eve (USA) (Disc 1).bin
   ms0:/PSP/GAME/ParasiteEve/disc/Parasite Eve (USA) (Disc 2).bin
   ```
3. Save files will automatically be managed in:
   ```text
   ms0:/PSP/SAVEDATA/SLUS00662/SLUS00662.mcd
   ```

---

## Controls & Button Mappings

| PSP Control | PS1 Action | Notes |
|---|---|---|
| **D-Pad / Analog Nub** | Movement / D-Pad | Nub uses 12,000 threshold deadzone |
| **Cross ($\times$)** | Cross / Confirm / Run | Standard PS1 mapping |
| **Circle ($\bigcirc$)** | Circle / Cancel | Standard PS1 mapping |
| **Square ($\square$)** | Square / Menu | Standard PS1 mapping |
| **Triangle ($\triangle$)** | Triangle | Standard PS1 mapping |
| **L-Trigger** | L1 Shoulder | Standard PS1 mapping |
| **R-Trigger** | R1 Shoulder | Standard PS1 mapping |
| **Select + L-Trigger** | **L2 Trigger** | Chorded modifier |
| **Select + R-Trigger** | **R2 Trigger** | Chorded modifier |
| **Select + Start** | Fast-Forward (Turbo) | Toggles emulation fast-forward |
| **Select + L + R** | **Disc Hot-Swap** | Toggles between Disc 1 and Disc 2 |
| **Start** | Start / Pause | Standard PS1 mapping |
| **HOME Button** | Exit to XMB | Handled via kernel exit callback |

---

## Repository Layout

```text
├── Makefile.psp                   # Root PSP build makefile (EBOOT.PBP target)
├── bootstrap_local_pspsdk.sh      # Hermetic local toolchain installer
├── env_psp.sh                     # Shell environment activation script
├── psp_link.ld                    # MIPS Allegrex PRX linker script with .overlay_arena
├── include/
│   ├── psp_gpu_gu.h               # Native sceGu graphics rasterizer header
│   ├── psp_sdl_host.h             # PSP host lifecycle, clock, and save data header
│   ├── psp_sdl_input.h            # Input mapping, chording, and packet header
│   ├── psp_sdl_audio.h            # SPU audio mixing and SDL callback header
│   ├── psp_cdrom_iso.h            # Async CD reader and MDEC video player header
│   └── psp_overlay_mgr.h          # Shared 2 MB overlay arena manager header
├── src/
│   ├── psp_gpu_gu.c               # sceGu primitive renderer & VRAM shadow
│   ├── psp_sdl_host.c             # 333/166MHz clocks & Memory Card persistence
│   ├── psp_sdl_input.c            # SDL joystick polling & L2/R2 chord synthesis
│   ├── psp_sdl_audio.c            # 24-voice SPU ADPCM / Gauss audio mixer
│   ├── psp_cdrom_iso.c            # Async sector streamer & in-place MDEC decoder
│   ├── psp_overlay_mgr.c          # 2 MB overlay arena allocator & guest dispatch
│   └── overlays/                  # Decompiled room and scene overlay sources
├── pc_port/                       # Upstream reference C platform layer
├── scripts/                       # Decompiled room bytecode scripts (.pes)
└── tools/                         # Decompilation, matching, and script tooling
```

---

## Original PS1 Matching Decompilation

To build and verify the original PlayStation 1 byte-matching binary:

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

mkdir -p assets/USA
cp /path/to/your/SLUS_006.62 assets/USA/main.exe

make split
make build
make check
```

- `make split`: Generates assembly files and linker scripts from the retail executable.
- `make build`: Compiles `build/USA/main.exe` using stock GCC 2.7.2 and `maspsx`.
- `make check`: Verifies the SHA-1 checksum against the retail Disc 1 binary (`452fb033f2eaa4b18aa20a5bca60b8125af3a37b`).

---

## License & Disclaimer

- See [`LICENSE.md`](LICENSE.md) for licensing details.
- Parasite Eve is a registered trademark of Square Enix Co., Ltd.
- This repository contains clean-room reverse-engineered and decompiled source code. It does **not** contain copyrighted game assets, ROMs, BIOS files, or disc images. Users must provide their own legally obtained copies of the original discs.
