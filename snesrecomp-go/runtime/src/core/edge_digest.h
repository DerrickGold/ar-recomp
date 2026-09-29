#ifndef SNESRECOMP_EDGE_DIGEST_H
#define SNESRECOMP_EDGE_DIGEST_H

typedef struct Snes Snes;

/* Execution-sequence digest for A/B comparison of two builds of one game.
 *
 * While enabled it folds, in the order the runner reports them, every executed
 * generated block, dynamic dispatch, interrupt, runtime error and frame
 * boundary into a running SHA-256, and writes one checkpoint line per host
 * frame. Two builds that execute the same control-flow edges from the same
 * inputs write identical files; the first differing line names the first
 * frame whose edge sequence diverged. It is evidence about execution order,
 * complementary to state dumps and source hashes, not a substitute for them.
 *
 * SNESRECOMP_EDGE_DIGEST=<path>, read at the first runner bind, enables it
 * for the published runner. While it is unset nothing subscribes, so no
 * per-event cost is paid. */
int sr_edge_digest_open(const char *path);
void sr_edge_digest_close(void);
void sr_edge_digest_bind_runner(Snes *runner, int enabled);

#endif
