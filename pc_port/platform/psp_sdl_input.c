/*
 * psp_sdl_input.c - PlayStation Portable Controller Input Implementation
 *
 * Maps PSP buttons and analog nub via SDL 1.2 (SDL_JoystickOpen(0)) to the
 * PS1 controller layout, implementing L2/R2 chording (Select + L/R),
 * directional deadzone filtering, and PS1 pad packet synthesis.
 */

#include "psp_sdl_input.h"

#include <SDL/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal State */
static SDL_Joystick *s_joystick = NULL;
static int           s_input_initialized = 0;

static uint16_t      s_held_buttons = 0;   /* Active-high PS1 button mask */
static uint16_t      s_pad_word     = 0xFFFFu; /* Active-low PS1 pad word */
static uint8_t       s_hotkeys      = 0;
static int16_t       s_raw_axis_x   = 0;
static int16_t       s_raw_axis_y   = 0;
static uint8_t       s_analog_lx    = 128;
static uint8_t       s_analog_ly    = 128;

int PspInput_Init(void)
{
    if (s_input_initialized && s_joystick) {
        return 1;
    }

    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0) {
            fprintf(stderr, "[PSP_INPUT] Failed to init SDL joystick: %s\n", SDL_GetError());
            return 0;
        }
    }

    SDL_JoystickEventState(SDL_ENABLE);

    if (SDL_NumJoysticks() > 0) {
        s_joystick = SDL_JoystickOpen(0);
        if (s_joystick) {
            fprintf(stderr, "[PSP_INPUT] Opened joystick 0: %s (Axes: %d, Buttons: %d)\n",
                    SDL_JoystickName(0),
                    SDL_JoystickNumAxes(s_joystick),
                    SDL_JoystickNumButtons(s_joystick));
        } else {
            fprintf(stderr, "[PSP_INPUT] Warning: Failed to open joystick 0: %s\n", SDL_GetError());
        }
    } else {
        fprintf(stderr, "[PSP_INPUT] Warning: No joysticks detected by SDL\n");
    }

    s_held_buttons = 0;
    s_pad_word = 0xFFFFu;
    s_hotkeys = 0;
    s_raw_axis_x = 0;
    s_raw_axis_y = 0;
    s_analog_lx = 128;
    s_analog_ly = 128;

    s_input_initialized = 1;
    return 1;
}

void PspInput_Shutdown(void)
{
    if (s_joystick) {
        SDL_JoystickClose(s_joystick);
        s_joystick = NULL;
    }
    s_input_initialized = 0;
    s_held_buttons = 0;
    s_pad_word = 0xFFFFu;
    s_hotkeys = 0;
}

void PspInput_Poll(void)
{
    uint16_t buttons = 0;
    uint8_t hotkeys = 0;
    int chord_l2 = 0;
    int chord_r2 = 0;
    int select_consumed = 0;

    if (!s_input_initialized) {
        PspInput_Init();
    }

    /* Pump SDL events / update joystick state */
    SDL_JoystickUpdate();

    if (!s_joystick) {
        s_held_buttons = 0;
        s_pad_word = 0xFFFFu;
        s_hotkeys = 0;
        return;
    }

    /*
     * PSP Button indices in SDL 1.2:
     *   0: Triangle
     *   1: Circle
     *   2: Cross
     *   3: Square
     *   4: L-Trigger
     *   5: R-Trigger
     *   6: Down
     *   7: Left
     *   8: Up
     *   9: Right
     *  10: Select
     *  11: Start
     */
    uint8_t btn_triangle = SDL_JoystickGetButton(s_joystick, 0);
    uint8_t btn_circle   = SDL_JoystickGetButton(s_joystick, 1);
    uint8_t btn_cross    = SDL_JoystickGetButton(s_joystick, 2);
    uint8_t btn_square   = SDL_JoystickGetButton(s_joystick, 3);
    uint8_t btn_l        = SDL_JoystickGetButton(s_joystick, 4);
    uint8_t btn_r        = SDL_JoystickGetButton(s_joystick, 5);
    uint8_t btn_down     = SDL_JoystickGetButton(s_joystick, 6);
    uint8_t btn_left     = SDL_JoystickGetButton(s_joystick, 7);
    uint8_t btn_up       = SDL_JoystickGetButton(s_joystick, 8);
    uint8_t btn_right    = SDL_JoystickGetButton(s_joystick, 9);
    uint8_t btn_select   = SDL_JoystickGetButton(s_joystick, 10);
    uint8_t btn_start    = SDL_JoystickGetButton(s_joystick, 11);

    /* 1. Map Face Buttons and D-Pad */
    if (btn_triangle) buttons |= HOST_PAD_TRIANGLE;
    if (btn_circle)   buttons |= HOST_PAD_CIRCLE;
    if (btn_cross)    buttons |= HOST_PAD_CROSS;
    if (btn_square)   buttons |= HOST_PAD_SQUARE;
    if (btn_down)     buttons |= HOST_PAD_DOWN;
    if (btn_left)     buttons |= HOST_PAD_LEFT;
    if (btn_up)       buttons |= HOST_PAD_UP;
    if (btn_right)    buttons |= HOST_PAD_RIGHT;
    if (btn_start)    buttons |= HOST_PAD_START;

    /*
     * 2. Chorded L2 / R2 Modifier:
     * Since the PSP hardware has only physical L1 / R1 triggers,
     * holding Select transforms L -> L2 and R -> R2, cleanly
     * suppressing the normal Select and L1/R1 signals.
     */
    if (btn_select) {
        if (btn_l) {
            chord_l2 = 1;
            select_consumed = 1;
        }
        if (btn_r) {
            chord_r2 = 1;
            select_consumed = 1;
        }

        /* Fast-Forward Chord: Select + Start toggles turbo mode */
        if (btn_start) {
            hotkeys |= HOST_PAD_HOTKEY_FF_TOGGLE;
            select_consumed = 1;
        }

        if (chord_l2) {
            buttons |= HOST_PAD_L2;
        }
        if (chord_r2) {
            buttons |= HOST_PAD_R2;
        }

        /* Only emit HOST_PAD_SELECT if not consumed as a chord modifier */
        if (!select_consumed) {
            buttons |= HOST_PAD_SELECT;
        }
    } else {
        /* Standard shoulder triggers when Select is not held */
        if (btn_l) buttons |= HOST_PAD_L1;
        if (btn_r) buttons |= HOST_PAD_R1;
    }

    /*
     * 3. PSP Analog Nub (Axis 0 = X, Axis 1 = Y, -32768..32767):
     * - Threshold against deadzone for digital movement in menus/gameplay.
     * - Convert to 8-bit unsigned integer (0..255, 128 center) for PS1 packet.
     */
    s_raw_axis_x = SDL_JoystickGetAxis(s_joystick, 0);
    s_raw_axis_y = SDL_JoystickGetAxis(s_joystick, 1);

    if (s_raw_axis_x <= -HOST_PAD_STICK_DEADZONE) buttons |= HOST_PAD_LEFT;
    if (s_raw_axis_x >=  HOST_PAD_STICK_DEADZONE) buttons |= HOST_PAD_RIGHT;
    if (s_raw_axis_y <= -HOST_PAD_STICK_DEADZONE) buttons |= HOST_PAD_UP;
    if (s_raw_axis_y >=  HOST_PAD_STICK_DEADZONE) buttons |= HOST_PAD_DOWN;

    /* Map -32768..32767 to 0..255 with 128 as neutral */
    s_analog_lx = (uint8_t)(((int32_t)s_raw_axis_x + 32768) >> 8);
    s_analog_ly = (uint8_t)(((int32_t)s_raw_axis_y + 32768) >> 8);

    /* Update cached results */
    s_held_buttons = buttons;
    s_pad_word     = (uint16_t)~buttons; /* Active-low PS1 pad word */
    s_hotkeys      = hotkeys;
}

uint16_t PspInput_GetButtons(void)
{
    return s_held_buttons;
}

uint16_t PspInput_GetPadWord(void)
{
    return s_pad_word;
}

void PspInput_GetAnalog(int *raw_x, int *raw_y)
{
    if (raw_x) *raw_x = s_raw_axis_x;
    if (raw_y) *raw_y = s_raw_axis_y;
}

int PspInput_GetPacket(PspPadPacket *packet)
{
    if (!packet) return 0;

    packet->status     = 0x00;  /* 0x00 = Controller communication OK */
    packet->id         = 0x73;  /* 0x73 = DualShock / Analog Controller */
    packet->buttons_lo = (uint8_t)(s_pad_word & 0xFFu);
    packet->buttons_hi = (uint8_t)((s_pad_word >> 8) & 0xFFu);
    packet->right_x    = 128;   /* Centered (PSP has no right stick) */
    packet->right_y    = 128;
    packet->left_x     = s_analog_lx;
    packet->left_y     = s_analog_ly;

    return 1;
}

uint8_t PspInput_GetHotkeys(void)
{
    return s_hotkeys;
}

/* ── pc_port Platform Interface Compatibility Symbols ──────────────────────── */

void HostPad_Poll(void)
{
    PspInput_Poll();
}

uint16_t HostPad_Held(void)
{
    return s_held_buttons;
}

uint8_t HostPad_Hotkeys(void)
{
    return s_hotkeys;
}

void HostPad_Close(void)
{
    PspInput_Shutdown();
}

uint16_t HostWindow_PadRaw(void)
{
    /* Returns the active-low PS1 pad word directly for port_main.c / field ticks */
    PspInput_Poll();
    return s_pad_word;
}
