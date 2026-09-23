#ifndef SNESRECOMP_PAIRED_TAIL_INTERNAL_H
#define SNESRECOMP_PAIRED_TAIL_INTERNAL_H

/* Opaque, transient execution tracking; not part of a saved machine state. */
typedef struct PairedTailDriver PairedTailDriver;
extern PairedTailDriver *g_sr_paired_tail_driver;
/* Owner token while an adopted transfer unwinds through dead inner drivers.
 * Dispatch loops propagate the pending request; only this driver consumes it.
 * Cleared together with the other transient execution state on abandonment. */
extern PairedTailDriver *g_sr_paired_tail_owner;
/* Only the generated wrapper that issued an HLE branch may drive it locally.
 * An escaped child's tail must retain its nonlocal-return meaning. */
int sr_take_hle_tail_request(int activation_depth);

#endif
