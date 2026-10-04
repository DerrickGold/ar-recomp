#ifndef AR_EDITOR_ROOM_SCENE_H
#define AR_EDITOR_ROOM_SCENE_H
#include "action/action_scene_snapshot.h"
#include "action/action_environment_scene.h"
#include "action/action_effect_recipes.h"
#include "action/action_effect_preview.h"
#include "diorama/diorama.h"
#include "snesrecomp/runner/scene_renderer.h"

typedef struct EditorRoomScene EditorRoomScene;
/* Stable default source inventory, independent of the preview camera or edits. */
typedef struct EditorRoomEffectSource {
  ActionEffectInstance effect;
  float map_scale_x, map_scale_y;
} EditorRoomEffectSource;
unsigned EditorRoomScene_DefaultSourceCount(EditorRoomScene *room);
const EditorRoomEffectSource *EditorRoomScene_DefaultSource(EditorRoomScene *room, unsigned index);
/* Semantic source/receiver regions, never camera-window aggregate origins.
 * Coordinates belong to the source's BG layer. Linked terrain guides edit the
 * common field definition; they are not independent runtime emitters. */
unsigned EditorRoomScene_DefaultGuideCount(EditorRoomScene *room, unsigned index);
bool EditorRoomScene_DefaultGuide(EditorRoomScene *room, unsigned index, unsigned guide,
                                  ArRenderRectF *bounds);
EditorRoomScene *EditorRoomScene_Create(const ActionSceneSnapshot *assets);
void EditorRoomScene_SetEvent(EditorRoomScene *room,const ActionEffectPreviewEvent *event);
void EditorRoomScene_Destroy(EditorRoomScene *room);
/* Complete selected-room INI text; parser/terrain resolution are production C.
 * Bad edits leave the previous configuration intact. */
bool EditorRoomScene_Configure(EditorRoomScene *room, const char *text);
/* Query the production planner before/after saved overrides at this camera. */
bool EditorRoomScene_BgPolicy(const EditorRoomScene *room, int x, int y,
                              uint32_t frame, bool defaults, ActionBgPlan *out, int camera_x[2]);
bool EditorRoomScene_Render(EditorRoomScene *room, int x, int y, uint32_t frame,
                            int extra_x, int vertical_budget);
const DioramaCapture *EditorRoomScene_Capture(const EditorRoomScene *room);
const DioramaScene *EditorRoomScene_Scene(const EditorRoomScene *room);
const SrSceneSurfaces *EditorRoomScene_Surfaces(const EditorRoomScene *room);
bool EditorRoomScene_ConfigureEffects(EditorRoomScene *room, const char *text, size_t size, unsigned *line);
const ActionSceneEffectFrame *EditorRoomScene_Effects(const EditorRoomScene *room);
const ActionEnvironmentScene *EditorRoomScene_Environment(const EditorRoomScene *room);
const ActionAtmosphereField *EditorRoomScene_AtmosphereField(const EditorRoomScene *room);
bool EditorRoomScene_PreviewBinding(EditorRoomScene *,bool enabled,uint32_t source,int x,int y,unsigned start);
const ActionSurfaceField *EditorRoomScene_SurfaceField(const EditorRoomScene *,unsigned index);
const ActionProjectileField *EditorRoomScene_ProjectileField(const EditorRoomScene *,unsigned index);
const ActionArcField *EditorRoomScene_ArcField(const EditorRoomScene *,unsigned index);
const ActionGlowField *EditorRoomScene_GlowField(const EditorRoomScene *);
const ActionCastleField *EditorRoomScene_CastleField(const EditorRoomScene *);
const ActionMarshField *EditorRoomScene_MarshField(const EditorRoomScene *);
const ActionMoonField *EditorRoomScene_MoonField(const EditorRoomScene *room);
const ActionWaterField *EditorRoomScene_WaterField(const EditorRoomScene *room);
double EditorRoomScene_WaterFieldMapScale(const EditorRoomScene *room,unsigned axis);
const ActionRayField *EditorRoomScene_RayField(const EditorRoomScene *room);
double EditorRoomScene_RayFieldMapScale(const EditorRoomScene *room, unsigned axis);
/* Stable CPU-surface digest for native/WASM regression checks. */
uint32_t EditorRoomScene_EffectHash(const EditorRoomScene *room);
uint32_t EditorRoomScene_Hash(const EditorRoomScene *room);
unsigned EditorRoomScene_Width(const EditorRoomScene *room);
unsigned EditorRoomScene_Height(const EditorRoomScene *room);
#endif
