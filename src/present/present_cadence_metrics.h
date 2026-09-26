#ifndef AR_PRESENT_CADENCE_METRICS_H
#define AR_PRESENT_CADENCE_METRICS_H
/* PresentCadence metrics: counters the display loop (host/host_display.c)
 * keeps for diagnostic dumps: presents of a new game tick, re-presents of the
 * retained frame and their largest interpolation alpha, and loop iterations
 * that produced a frame but neither presented nor slept.
 * Phase: present. */

typedef struct PresentCadenceMetrics {
  unsigned long tick_present_count;
  unsigned long represent_count;
  float maximum_represent_alpha;
  unsigned long no_present_no_sleep_iteration_count;
} PresentCadenceMetrics;

PresentCadenceMetrics PresentCadence_GetMetrics(void);

#endif /* AR_PRESENT_CADENCE_METRICS_H */
