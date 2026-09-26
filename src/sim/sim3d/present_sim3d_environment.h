#ifndef AR_PRESENT_SIM3D_ENVIRONMENT_H
#define AR_PRESENT_SIM3D_ENVIRONMENT_H
/* Shared town/globe atmosphere: immutable cloud layers and directional shadow
 * light from the captured scene. Owns no textures or compositor state. */

struct FrameSlot;
enum { kSimCloudLayerCount = 3 };
typedef struct SimCloudLayer {
  float scale, offset_x, offset_y, weight, drift_x, drift_y;
} SimCloudLayer;
extern const SimCloudLayer kSimCloudLayers[kSimCloudLayerCount];
/* Shared angular conversion value preserves the town/globe light arithmetic. */
extern const float kPi;
void SimShadowLight(const struct FrameSlot *slot, float *light_x, float *light_y);
#endif
