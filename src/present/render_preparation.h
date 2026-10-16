#ifndef AR_RENDER_PREPARATION_H
#define AR_RENDER_PREPARATION_H

#include "render/render_capabilities.h"
#include "render/render_device.h"

/* Owner-thread boot/reset work. Probes only scratch resources, never game
 * state. False means caller render state could not be safely established or
 * restored; individual unsupported features are reported in the mask. */
bool RenderPreparation_Prepare(ArRenderDevice *device, RenderFeatureMask *supported);

/* Builds pipelines for optional features the player has switched on (Diorama
 * 3D, frame interpolation) that are not built yet. Preparation calls it, and
 * the frame loop calls it every iteration so a feature switched on in the
 * settings menu compiles there, while the game is paused, instead of on the
 * first frame that uses it. Two flag checks when nothing is pending. */
void RenderPreparation_PrepareSelectedFeatures(ArRenderDevice *device);

#endif
