#ifndef AR_RUN_RETENTION_H
#define AR_RUN_RETENTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Install the ownership marker and keep it locked for the life of the run. */
FILE *RunRetention_Claim(const char *directory);
bool RunRetention_AutomaticPath(const char *root, const char *timestamp, char *path,
                                size_t capacity, FILE **owner);
/* Only consider marked timestamped runs. A locked, linked, or preserved run
 * stays untouched. Call at launch and teardown, outside gameplay. */
void RunRetention_Prune(const char *root, size_t keep, uint64_t byte_budget,
                        time_t cutoff);
/* An explicitly preserved destination can also be nested inside runs/. */
bool RunRetention_Preserve(const char *directory);

#endif
