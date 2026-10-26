#ifndef AR_AR2_PRESENTATION_H
#define AR_AR2_PRESENTATION_H
/* AR2 surfaces presented by the application's existing native/diorama device.
 * HUD ownership is semantic and independent of BG3's raster scenery. */
#include "actraiser2/imported_game_api.h"
bool Ar2Presentation_Open(ArRenderDevice *device);
bool Ar2Presentation_Draw(const ArImportedGameFrame *frame, uint64_t now_ns,
                          ArRenderRectI *viewport);
void Ar2Presentation_Close(void);
#endif
