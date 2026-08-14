#ifndef ACTRAISER_ACTION_METATILE_H
#define ACTRAISER_ACTION_METATILE_H

#include "cpu_state.h"

/* Whole-body HLEs for the action BG column/row metatile expanders. */
RecompReturn ActRaiser_ExpandActionBgMetatileColumn(CpuState *cpu);
RecompReturn ActRaiser_ExpandActionBgMetatileRow(CpuState *cpu);

#endif /* ACTRAISER_ACTION_METATILE_H */
