#ifndef AR_ACTRAISER_AR2_MASTER_INTERNAL_H
#define AR_ACTRAISER_AR2_MASTER_INTERNAL_H

#include "actraiser/actraiser_ar2_master.h"

/* Presentation borrows validated assets for the session. Reset releases that
 * reference before the controller frees the pack. Update runs at the start of
 * an action-sprite pass; drawing and NMI consume its prepared frame. */
bool Ar2MasterPresentation_Init(const Ar2MasterAssets *assets, char *error, size_t error_size);
void Ar2MasterPresentation_Reset(void);
void Ar2MasterPresentation_Update(const CpuState *cpu, bool enabled, int controlled_frame);

#endif
