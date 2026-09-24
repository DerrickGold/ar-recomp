#ifndef AR_REGIONAL_POSES_H
#define AR_REGIONAL_POSES_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum ArRegionalPoseRule {
  kArRegionalPose_AitosShared,
  kArRegionalPose_AitosHumanoid,
  kArRegionalPose_Count,
} ArRegionalPoseRule;
typedef struct ArRegionalPosePolicy { ArRegionalSource source[kArRegionalPose_Count]; } ArRegionalPosePolicy;
typedef struct ArRegionalPoseDescriptor { const char *key; uint16_t enabled[kArRegionalSource_Count]; } ArRegionalPoseDescriptor;
const ArRegionalPoseDescriptor *ArRegionalPoses_Descriptor(unsigned rule);
bool ArRegionalPoses_Resolve(const ArRegionalPosePolicy *policy,uint8_t *snapshot);
/* Changes only the selected visual ordinal. Timing, motion and extents remain
 * native. Caller verifies the measured source row and compatible composition. */
bool ArRegionalPoses_Visual(uint8_t snapshot,unsigned state,unsigned row,uint8_t native,uint8_t *visual);
#endif
