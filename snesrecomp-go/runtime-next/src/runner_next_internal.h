#pragma once

#include <stdbool.h>

#include "runner_next.h"

typedef struct Snes Snes;
typedef SrResult SrRunnerCpuStateProvider(
    Snes *snes, SrCpuStateSnapshot *out_state);
typedef SrResult SrRunnerPpuObjRasterProvider(
    Snes *snes, const SrPpuObjRasterRequest *request,
    SrPpuObjRasterResult *out_result);

/* Temporary bridge for in-tree consumers while compatibility globals are
 * migrated. None of these functions exposes an internal layout through the
 * public ABI. */
SrRunnerHandle *sr_runner_handle(Snes *snes);
void sr_runner_set_cpu_state_provider(
    Snes *snes, SrRunnerCpuStateProvider *provider,
    const void *component_handle);
void sr_runner_set_ppu_obj_raster_provider(
    Snes *snes, SrRunnerPpuObjRasterProvider *provider);
void sr_runner_bind_ppu_services(Snes *snes, bool enabled);
void sr_runner_note_tick(Snes *snes);
void sr_runner_note_reset(Snes *snes);
void sr_runner_note_load(Snes *snes);
void sr_runner_note_mutation(Snes *snes);
