#ifndef PSP_SDL_INPUT_H
#define PSP_SDL_INPUT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PS1 Controller Button Layout (Active-High Bitmask)
 * Directly matches pc_port host_pad.h and pe_plat/input.h
 */
#define HOST_PAD_SELECT   0x0001u
#define HOST_PAD_START    0x0008u
#define HOST_PAD_UP       0x0010u
#define HOST_PAD_RIGHT    0x0020u
#define HOST_PAD_DOWN     0x0040u
#define HOST_PAD_LEFT     0x0080u
#define HOST_PAD_L2       0x0100u
#define HOST_PAD_R2       0x0200u
#define HOST_PAD_L1       0x0400u
#define HOST_PAD_R1       0x0800u
#define HOST_PAD_TRIANGLE 0x1000u
#define HOST_PAD_CIRCLE   0x2000u
#define HOST_PAD_CROSS    0x4000u
#define HOST_PAD_SQUARE   0x8000u

/* Host hotkeys */
#define HOST_PAD_HOTKEY_FF_HOLD   0x01u
#define HOST_PAD_HOTKEY_FF_TOGGLE 0x02u

/* Left-stick deadzone for directional thresholding (-32768..32767) */
#define HOST_PAD_STICK_DEADZONE   12000

/*
 * PS1 Pad Status Packet (DualShock / Analog mode 0x73, 8 bytes):
 * Used by PS1 kernel and SIO pad polling routines.
 */
typedef struct {
    uint8_t status;     /* 0x00 = Controller ready / packet valid */
    uint8_t id;         /* 0x73 = DualShock / Analog, 0x41 = Digital */
    uint8_t buttons_lo; /* Active-low button mask lower 8 bits */
    uint8_t buttons_hi; /* Active-low button mask upper 8 bits */
    uint8_t right_x;    /* Right analog stick X (128 = centered) */
    uint8_t right_y;    /* Right analog stick Y (128 = centered) */
    uint8_t left_x;     /* Left analog nub X (0..255, 128 = centered) */
    uint8_t left_y;     /* Left analog nub Y (0..255, 128 = centered) */
} PspPadPacket;

/* Subsystem lifecycle */
int      PspInput_Init(void);
void     PspInput_Poll(void);
void     PspInput_Shutdown(void);

/* Query state */
uint16_t PspInput_GetButtons(void);             /* Active-high bitmask (HOST_PAD_*) */
uint16_t PspInput_GetPadWord(void);             /* Active-low PS1 digital pad word */
void     PspInput_GetAnalog(int *raw_x, int *raw_y); /* -32768..32767 */
int      PspInput_GetPacket(PspPadPacket *packet);
uint8_t  PspInput_GetHotkeys(void);

/* Direct drop-in compatibility symbols for pc_port host_pad.h & host_window.h */
void     HostPad_Poll(void);
uint16_t HostPad_Held(void);
uint8_t  HostPad_Hotkeys(void);
void     HostPad_Close(void);
uint16_t HostWindow_PadRaw(void);

#ifdef __cplusplus
}
#endif

#endif /* PSP_SDL_INPUT_H */
