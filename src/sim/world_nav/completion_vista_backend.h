#ifndef AR_COMPLETION_VISTA_BACKEND_H
#define AR_COMPLETION_VISTA_BACKEND_H
#include "render/render_device.h"
#include "present/presentation_outcome.h"

/* B-only material stages. Parameters are immutable presentation facts;
 * shader storage and native renderer state stay in the platform adapter. */
enum { kCompletionVistaLightCount = 13 };
/* Values match sun_horizon_stage.w in completion_vista.frag.glsl. */
typedef enum CompletionVistaStage {
  kCompletionVistaStage_Sky = 0,
  kCompletionVistaStage_Ocean = 1,
  kCompletionVistaStage_Shafts = 2,
  kCompletionVistaStage_Reflection = 3,
  kCompletionVistaStage_Rim = 4,
  kCompletionVistaStage_Count
} CompletionVistaStage;
typedef struct CompletionVistaLight {
  float source[3], target[3], cone_slope, strength;
} CompletionVistaLight;
typedef struct CompletionVistaParams {
  float eye[3], radius;
  float right[3], right_scale, up[3], up_scale, forward[3], time;
  ArRenderPointF sun, player;
  ArRenderPointF waterline;
  unsigned detail_flags;
  CompletionVistaLight lights[kCompletionVistaLightCount];
  float horizon, brightness;
  CompletionVistaStage stage;
  bool waves, pixel_water;
  ArRenderTexture water_pattern;
  ArRenderTexture native_art;
  int width, height;
} CompletionVistaParams;
bool CompletionVistaBackend_IsAvailable(ArRenderDevice *device);
PresentationOutcome CompletionVistaBackend_Draw(ArRenderDevice *device, ArRenderRectI viewport,
                                                const CompletionVistaParams *params);
void CompletionVistaBackend_Reset(ArRenderDevice *device);
#endif
