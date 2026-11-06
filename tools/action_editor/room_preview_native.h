#ifndef AR_ROOM_PREVIEW_NATIVE_H
#define AR_ROOM_PREVIEW_NATIVE_H
#include "action/present_action_effects.h"
#include "diorama/diorama_render_options.h"
/* Optional native projection adapter for the same complete-room preview used
 * by the browser. NULL callbacks preserve the portable reference renderer. */
void RoomPreview_SetSourceProjection(PresentActionSourceDraw effects, DioramaSkyboxDraw skybox);
void RoomPreview_SetSourceEffectBatch(DioramaSourceEffectBatch batch);
void RoomPreview_SetProjectionOffsets(const ArRenderPointF *planes, ArRenderPointF skybox);
unsigned RoomPreview_SkyboxRoom(void);
int RoomPreview_LoadSkybox(unsigned size);
int RoomPreview_Load(unsigned size);
int RoomPreview_LoadPlayer(unsigned size);
/* Raw native .arobj packet, read from DioramaPreview_Input(). Room/terrain
 * mismatch leaves the previous packet intact. Native and stand-in actors
 * are mutually exclusive. Clearing restores the ordinary editor preview. */
int RoomPreview_LoadObjects(unsigned size);
void RoomPreview_ClearObjects(void);
/* int32[14]: group, room, terrain, native GF, camera X/Y, world origin X/Y,
 * capture width/height, prop/actor counts, extra X/top. Returns 0 if absent. */
unsigned RoomPreview_ObjectInfo(void);
int RoomPreview_SetReceivers(int mask, int player_x, int player_y, int enemy_x, int enemy_y);
int RoomPreview_ReceiverStyle(unsigned flipped, unsigned rim);
void RoomPreview_EnableEffects(int enabled);
/* Toggle the solid SNES backdrop independently of the BG2/skybox mode. */
void RoomPreview_ShowBackdrop(int enabled);
unsigned RoomPreview_EffectCount(void);
unsigned RoomPreview_EffectVertices(void);
int RoomPreview_Configure(unsigned size);
int RoomPreview_ConfigureEffects(unsigned size);
void RoomPreview_Reset(void);
unsigned RoomPreview_Width(void);
unsigned RoomPreview_Height(void);
int RoomPreview_CameraX(void);
int RoomPreview_Set3DMode(int mode);
int RoomPreview_SetView(float distance, float yaw, float pitch, int aspect_mode, int dynamic_camera);
/* Five floats per opening in DioramaPreview_Input(): centre, width, power,
 * edge offset and reach, all relative to the authored region. */
unsigned RoomPreview_CloudOpenings(uint32_t seed, unsigned count);
unsigned RoomPreview_Measure(int x, int y, uint32_t frame, int extra, int vertical,
    int width, int height, float zoom, float yaw, float pitch, int skybox, int pixels, unsigned bg);
int RoomPreview_Render(int x, int y, uint32_t frame, int extra, int vertical,
    int width, int height, float distance, float yaw, float pitch, int skybox, int pixel_aspect);
/* Native baseline for release comparisons. Output aspect determines horizontal
 * coverage; no Diorama projection, skybox, authored tile edits or effects. */
int RoomPreview_RenderFlat(int x, int y, uint32_t frame, int width, int height, int pixel_aspect);
#endif
