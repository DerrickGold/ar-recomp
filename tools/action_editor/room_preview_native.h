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
int RoomPreview_SetReceivers(int mask, int player_x, int player_y, int enemy_x, int enemy_y);
int RoomPreview_Configure(unsigned size);
int RoomPreview_ConfigureEffects(unsigned size);
void RoomPreview_Reset(void);
unsigned RoomPreview_Width(void);
unsigned RoomPreview_Height(void);
int RoomPreview_CameraX(void);
int RoomPreview_SetView(float distance, float yaw, float pitch, int aspect_mode, int dynamic_camera);
unsigned RoomPreview_Measure(int x, int y, uint32_t frame, int extra, int vertical,
    int width, int height, float zoom, float yaw, float pitch, int skybox, int pixels, unsigned bg);
int RoomPreview_Render(int x, int y, uint32_t frame, int extra, int vertical,
    int width, int height, float distance, float yaw, float pitch, int skybox, int pixel_aspect);
#endif
