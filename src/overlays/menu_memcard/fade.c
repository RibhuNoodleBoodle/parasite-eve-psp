#include "fade_externs.h"
#include "menu_memcard_fade.h"
#include "menu_memcard_menu.h"
#include "menu_memcard_node.h"

void MenuCard_InitFadeForNode(MenuMemcardFadeState *state) {
    MenuMemcardListNode *node = ((MenuMemcardListNode *)g_MemcardActiveNodeHead);
    int kind = 7;

    state->displayTop = 0xB4;
    state->onStep = 0;
    state->phase = 0;
    state->velocity = 0;
    while (node != 0) {
        if (node->kind == kind) {
            break;
        }
        node = node->next;
    }
    node->fadeLevel = 0x100;
}

s32 MenuCard_UpdateFadeCounter(MenuMemcardFadeState *state) {
    s32 shifted;
    s32 result;

    state->displayTop = state->velocity + state->increment;
    shifted = state->increment << 4;
#if defined(__psp__) || defined(ENABLE_PSP)
    {
        s32 nextLevel = (shifted >= 0) ? (0x100 - shifted) : (shifted + 0x100);
        state->level = nextLevel;
        state->increment += (state->increment != 0);
        if (state->onStep != 0 && state->increment == state->callbackAt) {
            state->onStep(state);
        }
        if (state->increment < 16) return 1;
        state->finished = 1;
        result = 1;
    }
#else
    if (shifted >= 0) {
        result = 0x100 - shifted;
    } else {
        {
            s32 nextLevel = shifted + 0x100;
            asm volatile("" : : "r"(nextLevel));
            state->level = nextLevel;
        }
        state->increment += (state->increment != 0);
        if (state->onStep != 0 && state->increment == state->callbackAt) {
            state->onStep(state);
        }
        if (state->increment < 16) return 1;
        state->finished = 1;
        result = 1;
    }
#endif
    return result;
}

void MenuCard_AdvanceFade(MenuMemcardFadeState *state) {
    s32 level = state->level;

    if ((level <= 0 && state->velocity < 0) ||
        (level >= 0x100 && state->velocity > 0)) {
        state->velocity = 0;
    }
    state->level += state->velocity;
}

void MenuCard_UpdateFadeDisplay(MenuMemcardFadeState *state) {
    {
        s32 velocity = state->velocity;
        s32 increase = 0;
        s32 level;

        if (velocity != 0) {
            increase = velocity < 0x54;
        }
        level = state->level;
        velocity += increase;
        state->velocity = velocity;
        if (level < 0x100) {
            level += state->increment << 3;
        }
        state->level = level;
        if (level == 0 || level == 0x100) {
            state->increment = 0;
        }
    }
    {
        s32 current = state->velocity;
        s32 top = 0x3C - current;

        if (top >= 0) {
            top = current + 0x50;
        } else {
            top = 0x8C;
        }
        state->displayTop = top;
    }
    {
        s32 current = state->velocity;
        s32 delta = 0x54 - current;
        s32 bottom;

        /* Keep the floor constant in the branch delay slot. */
                if (delta >= 0x18) {
            bottom = delta;
        } else {
            bottom = 0x18;
        }
        state->displayBottom = bottom;
    }
}

void MenuCard_DecreaseFade(MenuMemcardFadeState *state) {
    s32 level = state->level - 0x10;

    if (level < 0) {
        level = 0;
    }
    state->level = level;
    if (level == 0) {
        state->increment = 0;
        state->velocity = 0;
    }
}

void MenuCard_CreateFadeImageNode(void) {
    MenuCard_CreateImageNode(2);
}

void MenuCard_AttachFadePayload(void) {
    MenuMemcardListNode *node;
    int kind;

    node = ((MenuMemcardListNode *)g_MemcardActiveNodeHead);
    kind = 2;
    while (node != 0) {
        if (node->kind == kind) {
            break;
        }
        node = node->next;
    }

    node->payload = &g_MenuCardStepFadeToImagesEntry;
}

void MenuCard_CreateImageGroup(void);

void MenuCard_StepFadeToImages(MenuMemcardFadeState *state) {
    int step_value;

    step_value = state->level - 0x10;
    state->level = step_value;

    if (step_value == 0x80) {
        MenuCard_CreateImageGroup();
    }

    if (state->level == 0) {
        state->finished = 1;
    }
}
