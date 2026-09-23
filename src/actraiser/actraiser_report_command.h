#ifndef ACTRAISER_REPORT_COMMAND_H
#define ACTRAISER_REPORT_COMMAND_H

#include "snesrecomp/game/cpu.h"
#include <stdbool.h>

/* Four SIM commands only, not the Palace menu or the reports' own lifecycle.
 * Caller captures keep_open when the command is accepted. */
bool ActRaiserReportCommand_Entry(const CpuState *cpu, unsigned action);
RecompReturn ActRaiserReportCommand_Run(CpuState *cpu, unsigned action, bool keep_open);

#endif
