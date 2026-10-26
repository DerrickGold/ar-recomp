#ifndef AR_AR2_CONTENT_H
#define AR_AR2_CONTENT_H
/* Optional, immutable world-pack lifetime. Assets are never read from an
 * arbitrary module path supplied by a pack; native code ships with the app. */
#include "actraiser2/ar2_world_atlas.h"
void Ar2Content_Load(void);
void Ar2Content_Close(void);
const Ar2WorldAtlas *Ar2Content_Atlas(void);
const uint8_t *Ar2Content_Rom(size_t *size);
bool Ar2Content_Playable(const Ar2WorldDestination *destination);
#endif
