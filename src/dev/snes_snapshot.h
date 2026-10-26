#ifndef AR_SNES_SNAPSHOT_H
#define AR_SNES_SNAPSHOT_H
/* Shared diagnostic format for either game's private runner. The caller
 * owns the runner and must keep execution stopped for the whole write. */
#include "snesrecomp/runner.h"
#include <stdbool.h>
bool SnesSnapshot_Write(const char *prefix, SrRunnerHandle *runner, const SnesRunnerApi *api);
#endif
