#ifndef AR_CHURCH_LIGHTING_H
#define AR_CHURCH_LIGHTING_H
/* Static church light visibility and diffuse shading. Shadow maps are built
 * from the altar's actual faces and round proxies for the native columns. */
#include "render/render_device.h"

typedef struct ChurchPoint {
  float x, y, z;
} ChurchPoint;

typedef enum ChurchLightSource {
  kChurchLight_Window,
  kChurchLight_Door,
  kChurchLight_Count,
} ChurchLightSource;

/* Shared center and spread for the visible rays, dust and diffuse pool. */
ChurchPoint ChurchLighting_WindowPoint(float across, float along);
void ChurchLighting_Reset(void);
bool ChurchLighting_Ready(bool columns);
void ChurchLighting_Begin(bool columns);
void ChurchLighting_AddQuad(const ChurchPoint points[4]);
void ChurchLighting_End(void);
float ChurchLighting_Visibility(ChurchLightSource source, ChurchPoint p, ChurchPoint normal);
ArRenderColorF ChurchLighting_Shade(ChurchPoint p, ChurchPoint normal, ArRenderColorF color,
                                    float ambient_occlusion);
/* Gentle wrapped light for already-shaded native art; never changes alpha. */
ArRenderColorF ChurchLighting_ActorTint(ChurchPoint p, float across);
#endif
