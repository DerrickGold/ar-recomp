#ifndef AR_REGIONAL_SOURCE_H
#define AR_REGIONAL_SOURCE_H

/* Semantic rule provenance, not language, display refresh, or donor presence.
 * Persist stable keys rather than these process-local enum ordinals. */
typedef enum ArRegionalSource {
  kArRegionalSource_US,
  kArRegionalSource_Japan,
  kArRegionalSource_Europe,
  kArRegionalSource_Count,
} ArRegionalSource;

#endif
