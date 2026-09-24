#include "regional/action/regional_hazards.h"
#include <stddef.h>

typedef struct HazardStream { const uint8_t (*boxes)[5]; unsigned count; } HazardStream;
typedef struct HazardRoom { uint16_t scene; HazardStream streams[kArRegionalSource_Count]; } HazardRoom;
#include "regional/action/regional_hazards_data.inc"

static const ArRegionalHazardDescriptor kDescriptor = {"terrain_hazards", {0,1,2}};
const ArRegionalHazardDescriptor *ArRegionalHazards_Descriptor(void) { return &kDescriptor; }
bool ArRegionalHazards_Resolve(ArRegionalSource source, uint8_t *profile) {
  if (!profile || (unsigned)source >= kArRegionalSource_Count) return false;
  *profile = (uint8_t)kDescriptor.profile[source];
  return true;
}

bool ArRegionalHazards_Copy(uint8_t profile, uint16_t scene, ArRegionalHazards *out) {
  if (!out || profile >= kArRegionalSource_Count) return false;
  /* Once per room, never on the contact/frame path. */
  for (unsigned i = 0; i < sizeof(kRooms)/sizeof(kRooms[0]); ++i) {
    if (kRooms[i].scene != scene) continue;
    const HazardStream *stream = &kRooms[i].streams[profile];
    if (stream->count > kArRegionalHazards_Max) return false;
    ArRegionalHazards next = {.count = stream->count};
    for (unsigned j = 0; j < stream->count; ++j) {
      const uint8_t *b = stream->boxes[j];
      next.boxes[j] = (ArRegionalHazardBox){
        (uint16_t)(16u*b[0]-4u), (uint16_t)(16u*(uint8_t)(b[1]-b[0])+24u),
        (uint16_t)(16u*b[2]-16u), (uint16_t)(16u*(uint8_t)(b[3]-b[2])+48u), b[4]};
    }
    *out = next;
    return true;
  }
  return false;
}
