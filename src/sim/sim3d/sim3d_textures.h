#ifndef AR_SIM3D_TEXTURES_H
#define AR_SIM3D_TEXTURES_H
/* SIM capture textures, their exact upload mirrors and availability. CPU
 * capture remains in sim3d.c; scene drawing is in present_sim3d.c. */
#include "render/render_device.h"
typedef struct FrameSlot FrameSlot;
struct SettingDesc;

/* Boot/shutdown. Enabled features report fatal allocation failures at boot.
 * Destroy before recreating; target/device resets only invalidate uploads. */
void Sim3DTextures_Create(ArRenderDevice *device);
void Sim3DTextures_Destroy(ArRenderDevice *device);
void Sim3DTextures_ResetUploads(void);
/* Called before releasing producer pixels, including the atlas/flat buffers
 * owned by SIM capture. Retained presentations sample GPU handles only. */
void Sim3DTextures_Upload(ArRenderDevice *device, const FrameSlot *slot);
/* Borrowed handles: callers may sample them but must not destroy them. */
ArRenderTexture Sim3DTextures_Atlas(void);
ArRenderTexture Sim3DTextures_Layer(int plane);
ArRenderTexture Sim3DTextures_Flat(void);
bool Sim3DTextures_Ready(void);
bool Sim3DTextures_BillboardsReady(void);
bool Sim3DTextures_ValidateSetting(const struct SettingDesc *desc);
#endif
