#ifndef AR_REGIONAL_COLLISION_H
#define AR_REGIONAL_COLLISION_H
#include "regional_source.h"
#include <stdbool.h>
#include <stdint.h>
enum { kArRegionalCollision_Kasandora, kArRegionalCollision_Arrow, kArRegionalCollision_Count };
typedef struct ArRegionalCollisionPolicy { ArRegionalSource source[kArRegionalCollision_Count]; } ArRegionalCollisionPolicy;
typedef uint8_t ArRegionalCollisionSnapshot;
typedef struct ArRegionalCollisionDescriptor { const char *key; uint16_t japanese[kArRegionalSource_Count]; } ArRegionalCollisionDescriptor;
/* Signed distances in native composition order, before facing is applied.
 * Artwork/parts, damage values and movement are independently owned. */
typedef struct ArRegionalCollisionExtents { int16_t left,right,top,bottom; } ArRegionalCollisionExtents;
const ArRegionalCollisionDescriptor *ArRegionalCollision_Descriptor(unsigned rule);
bool ArRegionalCollision_Init(ArRegionalCollisionPolicy *policy,ArRegionalSource source);
bool ArRegionalCollision_Resolve(const ArRegionalCollisionPolicy *policy,ArRegionalCollisionSnapshot *snapshot);
bool ArRegionalCollision_GroupSource(const ArRegionalCollisionPolicy *policy,ArRegionalSource *source);
/* Only known US headers are replaced; incompatible donor assets fail closed.
 * Visual IDs are semantic poses within the proven family, not ROM pointers. */
bool ArRegionalCollision_Pose(ArRegionalCollisionSnapshot snapshot,unsigned family,unsigned visual,
    const ArRegionalCollisionExtents *native,ArRegionalCollisionExtents *out);
#endif
