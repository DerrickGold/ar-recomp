#ifndef SNESRECOMP_PAIRED_TAIL_INTERNAL_H
#define SNESRECOMP_PAIRED_TAIL_INTERNAL_H

/* Opaque, transient execution tracking; not part of a saved machine state. */
typedef struct PairedTailDriver PairedTailDriver;
extern PairedTailDriver *g_sr_paired_tail_driver;

#endif
