#ifndef AR_EDITOR_ROOM_SCENE_H
#define AR_EDITOR_ROOM_SCENE_H
#include "action/action_scene_snapshot.h"
#include "action/action_environment_scene.h"
#include "action/action_effect_recipes.h"
#include "action/action_effect_preview.h"
#include "diorama/diorama.h"
#include "snesrecomp/runner/scene_renderer.h"

typedef struct EditorRoomScene EditorRoomScene;
EditorRoomScene *EditorRoomScene_Create(const ActionSceneSnapshot *assets);
void EditorRoomScene_SetEvent(EditorRoomScene *room,const ActionEffectPreviewEvent *event);
void EditorRoomScene_Destroy(EditorRoomScene *room);
/* Complete selected-room INI text; parser/terrain resolution are production C.
 * Bad edits leave the previous configuration intact. */
bool EditorRoomScene_Configure(EditorRoomScene *room, const char *text);
bool EditorRoomScene_Render(EditorRoomScene *room, int x, int y, uint32_t frame,
                            int extra_x, int vertical_budget);
const DioramaCapture *EditorRoomScene_Capture(const EditorRoomScene *room);
const DioramaScene *EditorRoomScene_Scene(const EditorRoomScene *room);
const SrSceneSurfaces *EditorRoomScene_Surfaces(const EditorRoomScene *room);
bool EditorRoomScene_ConfigureEffects(EditorRoomScene *room, const char *text, size_t size, unsigned *line);
const ActionSceneEffectFrame *EditorRoomScene_Effects(const EditorRoomScene *room);
const ActionEnvironmentScene *EditorRoomScene_Environment(const EditorRoomScene *room);
/* Stable CPU-surface digest for native/WASM regression checks. */
uint32_t EditorRoomScene_EffectHash(const EditorRoomScene *room);
uint32_t EditorRoomScene_Hash(const EditorRoomScene *room);
unsigned EditorRoomScene_Width(const EditorRoomScene *room);
unsigned EditorRoomScene_Height(const EditorRoomScene *room);
#endif
