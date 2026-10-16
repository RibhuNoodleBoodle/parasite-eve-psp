#ifndef PE1_CD_POLL_H
#define PE1_CD_POLL_H

#include "common.h"
#include "pe1/cdrom.h"
#include "pe1/game_state.h"

/* Polls the pending sector read; a finished or failed read (0 or -1)
 * clears the read-busy game state flags. Boot loaders that spin on it
 * inline this body; cdrom/scene_assets.c holds the out-of-line copy. */
#ifndef PE1_NO_INLINE_CDROM_POLL
#if defined(__GNUC__) && !defined(__GNUC_GNU_INLINE__)
static inline int CdRom_PollReady(void) {
#else
extern inline int CdRom_PollReady(void) {
#endif
    int scratch;
    int status;
    Pe1GameState *state;

    status = DsReadSync(&scratch);
    if ((unsigned int)(status + 1) < 2U) {
        state = &g_GameState;
        state->flags &= 0xFEFFBFFF;
    }
    return status;
}
#endif

#endif /* PE1_CD_POLL_H */
