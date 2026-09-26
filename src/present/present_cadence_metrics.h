#ifndef AR_PRESENT_CADENCE_METRICS_H
#define AR_PRESENT_CADENCE_METRICS_H

typedef struct PresentCadenceMetrics {
  unsigned long tick_present_count;
  unsigned long represent_count;
  float maximum_represent_alpha;
  unsigned long no_present_no_sleep_iteration_count;
} PresentCadenceMetrics;

PresentCadenceMetrics PresentCadence_GetMetrics(void);

#endif /* AR_PRESENT_CADENCE_METRICS_H */
