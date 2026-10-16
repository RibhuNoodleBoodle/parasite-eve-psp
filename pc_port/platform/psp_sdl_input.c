/*
 * psp_sdl_input.c - PlayStation Portable Controller Input Implementation
 *
 * Maps PSP buttons and analog nub via SDL 1.2 (SDL_JoystickOpen(0)) to the
 * PS1 controller layout, implementing L2/R2 chording (Select + L/R),
 * directional deadzone filtering, and PS1 pad packet synthesis.
 */

#include "psp_sdl_input.h"

#include <SDL/SDL.h>
#if defined(__psp__) || defined(ENABLE_PSP)
#include <pspctrl.h>
#include <pspiofilemgr.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal State */
static SDL_Joystick *s_joystick = NULL;
static int           s_input_initialized = 0;
static uint8_t      *s_pad_direct_buf[2] = {NULL, NULL};

static uint16_t      s_held_buttons = 0;   /* Active-high PS1 button mask */
static uint16_t      s_pad_word     = 0xFFFFu; /* Active-low PS1 pad word */
static uint8_t       s_hotkeys      = 0;
static int16_t       s_raw_axis_x   = 0;
static int16_t       s_raw_axis_y   = 0;
static uint8_t       s_analog_lx    = 128;
static uint8_t       s_analog_ly    = 128;

int PspInput_Init(void)
{
    if (s_input_initialized) {
        return 1;
    }

#if defined(__psp__) || defined(ENABLE_PSP)
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
#endif

    if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0) {
            fprintf(stderr, "[PSP_INPUT] Failed to init SDL joystick: %s\n", SDL_GetError());
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
        }
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

#if defined(__psp__) || defined(ENABLE_PSP)
    uint8_t btn_triangle = 0;
    uint8_t btn_circle   = 0;
    uint8_t btn_cross    = 0;
    uint8_t btn_square   = 0;
    uint8_t btn_l        = 0;
    uint8_t btn_r        = 0;
    uint8_t btn_down     = 0;
    uint8_t btn_left     = 0;
    uint8_t btn_up       = 0;
    uint8_t btn_right    = 0;
    uint8_t btn_select   = 0;
    uint8_t btn_start    = 0;

    SceCtrlData ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    if (sceCtrlPeekBufferPositive(&ctrl, 1) > 0) {
        if (ctrl.Buttons & PSP_CTRL_TRIANGLE) btn_triangle = 1;
        if (ctrl.Buttons & PSP_CTRL_CIRCLE)   btn_circle   = 1;
        if (ctrl.Buttons & PSP_CTRL_CROSS)    btn_cross    = 1;
        if (ctrl.Buttons & PSP_CTRL_SQUARE)   btn_square   = 1;
        if (ctrl.Buttons & PSP_CTRL_LTRIGGER) btn_l        = 1;
        if (ctrl.Buttons & PSP_CTRL_RTRIGGER) btn_r        = 1;
        if (ctrl.Buttons & PSP_CTRL_DOWN)     btn_down     = 1;
        if (ctrl.Buttons & PSP_CTRL_LEFT)     btn_left     = 1;
        if (ctrl.Buttons & PSP_CTRL_UP)       btn_up       = 1;
        if (ctrl.Buttons & PSP_CTRL_RIGHT)    btn_right    = 1;
        if (ctrl.Buttons & PSP_CTRL_SELECT)   btn_select   = 1;
        if (ctrl.Buttons & PSP_CTRL_START)    btn_start    = 1;

        int16_t hw_ax = (int16_t)(((int)ctrl.Lx - 128) * 256);
        int16_t hw_ay = (int16_t)(((int)ctrl.Ly - 128) * 256);
        if (hw_ax <= -HOST_PAD_STICK_DEADZONE || hw_ax >= HOST_PAD_STICK_DEADZONE) {
            s_raw_axis_x = hw_ax;
        } else {
            s_raw_axis_x = 0;
        }
        if (hw_ay <= -HOST_PAD_STICK_DEADZONE || hw_ay >= HOST_PAD_STICK_DEADZONE) {
            s_raw_axis_y = hw_ay;
        } else {
            s_raw_axis_y = 0;
        }
    }
#else
    if (s_joystick) {
        SDL_JoystickUpdate();
    }
    uint8_t btn_triangle = s_joystick ? SDL_JoystickGetButton(s_joystick, 0) : 0;
    uint8_t btn_circle   = s_joystick ? SDL_JoystickGetButton(s_joystick, 1) : 0;
    uint8_t btn_cross    = s_joystick ? SDL_JoystickGetButton(s_joystick, 2) : 0;
    uint8_t btn_square   = s_joystick ? SDL_JoystickGetButton(s_joystick, 3) : 0;
    uint8_t btn_l        = s_joystick ? SDL_JoystickGetButton(s_joystick, 4) : 0;
    uint8_t btn_r        = s_joystick ? SDL_JoystickGetButton(s_joystick, 5) : 0;
    uint8_t btn_down     = s_joystick ? SDL_JoystickGetButton(s_joystick, 6) : 0;
    uint8_t btn_left     = s_joystick ? SDL_JoystickGetButton(s_joystick, 7) : 0;
    uint8_t btn_up       = s_joystick ? SDL_JoystickGetButton(s_joystick, 8) : 0;
    uint8_t btn_right    = s_joystick ? SDL_JoystickGetButton(s_joystick, 9) : 0;
    uint8_t btn_select   = s_joystick ? SDL_JoystickGetButton(s_joystick, 10) : 0;
    uint8_t btn_start    = s_joystick ? SDL_JoystickGetButton(s_joystick, 11) : 0;

    s_raw_axis_x = s_joystick ? SDL_JoystickGetAxis(s_joystick, 0) : 0;
    s_raw_axis_y = s_joystick ? SDL_JoystickGetAxis(s_joystick, 1) : 0;
#endif

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
     * Automated Input Injection: Active during headless tests until Step 4.
     */
    static int s_autotest = -1;
    static uint32_t s_poll_counter = 0;
    s_poll_counter++;

    if (s_autotest < 0) {
        const char *env = getenv("PE_AUTOTEST");
        if (env && (strcmp(env, "0") == 0 || strcmp(env, "false") == 0)) {
            s_autotest = 0;
        } else {
            s_autotest = 1;
        }
        if (s_autotest == 1) {
            fprintf(stderr, "[PE-AUTOTEST] PE_AUTOTEST active: automated test injection enabled.\n");
        }
    }

    if (s_autotest == 1) {
        if (s_poll_counter >= 70 && s_poll_counter <= 85) {
            buttons |= HOST_PAD_START;
            if (s_poll_counter == 70) {
                fprintf(stderr, "[NEW-GAME-TRACE] Auto-input: START injected (frame %u)\n", (unsigned)s_poll_counter);
            }
        }
        if (s_poll_counter >= 160 && s_poll_counter <= 320) {
            buttons |= HOST_PAD_CROSS;
            if (s_poll_counter == 160) {
                fprintf(stderr, "[NEW-GAME-TRACE] Auto-input: CROSS injected for NEW GAME (frame %u)\n", (unsigned)s_poll_counter);
            }
        }
    }

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
     * - Threshold strictly against HOST_PAD_STICK_DEADZONE (12000).
     */
    if (s_raw_axis_x <= -HOST_PAD_STICK_DEADZONE) buttons |= HOST_PAD_LEFT;
    else if (s_raw_axis_x >= HOST_PAD_STICK_DEADZONE) buttons |= HOST_PAD_RIGHT;
    if (s_raw_axis_y <= -HOST_PAD_STICK_DEADZONE) buttons |= HOST_PAD_UP;
    else if (s_raw_axis_y >= HOST_PAD_STICK_DEADZONE) buttons |= HOST_PAD_DOWN;

    /* Map -32768..32767 to 0..255 with strict 128 deadzone centering */
    if (s_raw_axis_x <= -HOST_PAD_STICK_DEADZONE || s_raw_axis_x >= HOST_PAD_STICK_DEADZONE) {
        s_analog_lx = (uint8_t)(((int32_t)s_raw_axis_x + 32768) >> 8);
    } else {
        s_analog_lx = 128;
    }
    if (s_raw_axis_y <= -HOST_PAD_STICK_DEADZONE || s_raw_axis_y >= HOST_PAD_STICK_DEADZONE) {
        s_analog_ly = (uint8_t)(((int32_t)s_raw_axis_y + 32768) >> 8);
    } else {
        s_analog_ly = 128;
    }

    /* Update cached results */
    s_held_buttons = buttons;
    s_pad_word     = (uint16_t)~buttons; /* Active-low PS1 pad word */
    s_hotkeys      = hotkeys;

    if (s_pad_direct_buf[0]) {
        PspPadPacket pkt;
        PspInput_GetPacket(&pkt);
        memcpy(s_pad_direct_buf[0], &pkt, sizeof(pkt));
    }
    if (s_pad_direct_buf[1]) {
        s_pad_direct_buf[1][0] = 0xFF; /* Port 2 disconnected */
        s_pad_direct_buf[1][1] = 0xFF;
    }
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

void PadInitDirect(unsigned char *pad1, unsigned char *pad2) {
    s_pad_direct_buf[0] = pad1;
    s_pad_direct_buf[1] = pad2;
    PspInput_Init();
    PspInput_Poll();
}

void PadStartCom(void) {}

int PadGetState(int port) {
    if (port == 0) {
        PspInput_Poll();
        return 6; /* PadStateStable */
    }
    return 0;
}

int PadInfoMode(int port, int term, int offs) {
    (void)port; (void)term; (void)offs;
    return 4; /* Standard controller */
}

int PadSetAct(int port, unsigned char *act, int len) {
    (void)port; (void)act; (void)len;
    return 1;
}

int PadSetActAlign(int port, const unsigned char *align) {
    (void)port; (void)align;
    return 1;
}

int PadSetMainMode(int socket, int mode, int lock) {
    (void)socket; (void)mode; (void)lock;
    return 1;
}

