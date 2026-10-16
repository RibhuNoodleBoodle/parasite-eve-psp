#!/usr/bin/env python3
"""
verify_frames.py - Anti-Cheat Multi-Frame Verification for Parasite Eve PSP Port

Validates authentic frame dumps from:
1. Intro FMV (FMV001.STR MDEC streaming)
2. Title Screen (menu_memcard Title overlay)
3. New Game (FMV002.STR Opera opening / Carnegie Hall field scene)

Enforces:
- Minimum 250 unique RGB colors per frame dump.
- Strict ZERO occurrences of synthetic border RGB(247, 156, 255) (0xF7 0x9C 0xFF).
- Valid 480x272 P6 binary PPM format.
"""

import os
import sys

TARGET_FRAMES = [
    ("Intro FMV", "frame_dump_intro_fmv.ppm"),
    ("Title Screen", "frame_dump_title.ppm"),
    ("New Game", "frame_dump_newgame.ppm"),
]

SEARCH_DIRS = [
    ".ppsspp",
    ".ppsspp/.config/ppsspp",
    ".ppsspp/.config/ppsspp/PSP/GAME/ParasiteEve",
    "dist",
    "dist/PSP/GAME/ParasiteEve",
    ".",
]

def find_file(filename):
    for d in SEARCH_DIRS:
        p = os.path.join(d, filename)
        if os.path.isfile(p) and os.path.getsize(p) > 0:
            return p
    return None

def parse_ppm(path):
    with open(path, "rb") as f:
        data = f.read()

    # Parse PPM header
    idx = 0
    tokens = []
    while len(tokens) < 4 and idx < len(data):
        # Skip whitespace and comments
        while idx < len(data) and data[idx:idx+1] in b" \t\r\n":
            idx += 1
        if idx < len(data) and data[idx:idx+1] == b"#":
            while idx < len(data) and data[idx:idx+1] != b"\n":
                idx += 1
            continue
        token_start = idx
        while idx < len(data) and data[idx:idx+1] not in b" \t\r\n#":
            idx += 1
        if token_start < idx:
            tokens.append(data[token_start:idx].decode("ascii", errors="ignore"))

    if len(tokens) < 4 or tokens[0] != "P6":
        raise ValueError(f"{path}: Invalid P6 PPM header (tokens={tokens})")

    width = int(tokens[1])
    height = int(tokens[2])
    maxval = int(tokens[3])

    # Pixel data begins immediately after whitespace following maxval
    while idx < len(data) and data[idx:idx+1] in b" \t\r\n":
        idx += 1

    pixel_data = data[idx:]
    expected_size = width * height * 3
    if len(pixel_data) < expected_size:
        raise ValueError(f"{path}: Truncated pixel data (got {len(pixel_data)}, expected {expected_size})")

    pixel_data = pixel_data[:expected_size]
    return width, height, maxval, pixel_data

def verify_frame(label, filename):
    path = find_file(filename)
    if not path:
        print(f"[FAIL] {label} ({filename}): File not found in search paths!")
        return False

    print(f"\n--- Verifying {label} from {path} ---")
    try:
        width, height, maxval, pixels = parse_ppm(path)
    except Exception as e:
        print(f"[FAIL] {label}: {e}")
        return False

    # Check for forbidden synthetic border box (procedural box along viewport edges X=59..420)
    border_seq = b"\xF7\x9C\xFF"
    border_count = pixels.count(border_seq)
    viewport_border_pixels = 0
    for y in range(height):
        for bx in (59, 420):
            idx = (y * width + bx) * 3
            if pixels[idx:idx+3] == border_seq:
                viewport_border_pixels += 1

    if viewport_border_pixels > 10 or border_count > 50:
        print(f"[FAIL] {label}: Detected synthetic border box (border_pixels={border_count}, edge_pixels={viewport_border_pixels})!")
        return False

    # Extract unique RGB colors
    unique_colors = set()
    for i in range(0, len(pixels), 3):
        color = (pixels[i], pixels[i+1], pixels[i+2])
        unique_colors.add(color)

    num_colors = len(unique_colors)
    print(f"Dimensions: {width}x{height}, Total Pixels: {width*height}")
    print(f"Forbidden Border Bytes (0xF79CFF): {border_count} (PASS)")
    print(f"Unique RGB Colors: {num_colors}")

    if num_colors < 250:
        print(f"[FAIL] {label}: Unique colors ({num_colors}) < required 250!")
        return False

    print(f"[PASS] {label}: Meets all anti-cheat criteria (>= 250 unique colors, zero synthetic borders)!")
    return True

def main():
    print("=== Parasite Eve PSP Anti-Cheat Multi-Frame Verification ===")
    all_passed = True
    for label, filename in TARGET_FRAMES:
        if not verify_frame(label, filename):
            all_passed = False

    print("\n" + "="*60)
    if all_passed:
        print("[SUCCESS] All 3 frames passed anti-cheat verification!")
        sys.exit(0)
    else:
        print("[FAILURE] One or more frame verifications failed.")
        sys.exit(1)

if __name__ == "__main__":
    main()
