#include "snesrecomp/game/cpu.h"
#include <stdio.h>

/* Former generated miss-only formatting. The caller retains the environment
 * presence check so disabled logging makes no outlined call. Keep the messages
 * byte-identical; these are not missing-body traps and do not change the
 * generic return following a failed local guard. */
NOINLINE void sr_rts_dispatch_width(const CpuState *cpu, uint32 site, uint16 target) {
    fprintf(stderr, "[rts_dispatch_width] site=$%06X popped target=$%04X: runtime m=%d x=%d has no decoded body -> generic return (refused wrong-width goto)\n",
            (unsigned)site, (unsigned)target, (int)cpu->m_flag, (int)cpu->x_flag);
}

NOINLINE void sr_rts_dispatch_miss(const CpuState *cpu, uint32 site, uint16 target) {
    fprintf(stderr, "[rts_dispatch_miss] site=$%06X popped target=$%04X (UNREGISTERED -> generic return; add to rts_dispatch) S=$%04X m=%d x=%d\n",
            (unsigned)site, (unsigned)target, (unsigned)cpu->S, (int)cpu->m_flag, (int)cpu->x_flag);
}
