/* CC1_FLAGS: -fno-cse-skip-blocks */

#include "common.h"
#include "pe1/boot_disc_check.h"
#include "pe1/boot_display.h"
#include "pe1/psyq_gpu.h"
#include "pe1/signed_rect.h"

#include "menu_memcard_image.h"
#include "menu_memcard_screen.h"
#include "page_flip_externs.h"

typedef PsxSignedRect Rect;

void ResetGraph(int);

void MenuCard_FlipScreenPages(void) {
    int i;
    int page;
    int y;
    MemcardScreenBuffer *screen;
    Rect main_rect;

    for (i = 0; i < 2; i++) {
        page = g_MemcardScreenPage == 0;
        g_MemcardCurrentScreen = g_MemcardScreenBuffers[page];
        g_MemcardScreenPage = page;
        main_rect.x = 0;
        y = 0x14;
        if (page == 0) y = 0x104;
        main_rect.w = 0x1E0;
        main_rect.h = 0xCC;
        main_rect.y = y;
        LoadImage(&main_rect, &((u8 *)&g_MemcardImageOffsetsView4)[g_MemcardImageOffsets[0]]);
        DrawSync(0);

        screen = g_MemcardCurrentScreen;
        if (screen->overlay.w > 0) {
            RECT clipped;
            clipped = screen->overlay;
            clipped.x = (clipped.x * 3) >> 1;
            if (g_MemcardScreenPage == 0) clipped.y = (u16)clipped.y + 0xF0;
            clipped.w = (clipped.w * 3) >> 1;
            LoadImage(&clipped, g_MemcardCurrentScreen->overlayImage);
        }

#if defined(__psp__) || defined(ENABLE_PSP)
        PutDrawEnv(&g_MemcardCurrentScreen->draw);
        PutDispEnv(&g_MemcardCurrentScreen->disp);
        VSync(0);
        ResetGraph(1);
#else
        VSync(0);
        ResetGraph(1);
        PutDrawEnv(&g_MemcardCurrentScreen->draw);
        PutDispEnv(&g_MemcardCurrentScreen->disp);
#endif
    }
}
