#ifndef AR_ACTION_ENVIRONMENT_CAPTURE_INTERNAL_H
#define AR_ACTION_ENVIRONMENT_CAPTURE_INTERNAL_H
/* Private bounded capture operations. Each stage validates its map dimensions
 * and art witnesses before emitting records; capture never mutates WRAM. */
#include "action/action_effects.h"
#include "action/action_environment_scene.h"
#include "actraiser/actraiser_room_profiles.h"
#include "actraiser_game.h"
#include "action/action_bg_world.h"
#include <string.h>

static inline uint16_t Read16(const uint8_t *wram, size_t wram_size,
                       size_t address) {
  if (!wram || address + 1 >= wram_size) return 0;
  return (uint16_t)(wram[address] | ((uint16_t)wram[address + 1] << 8));
}

static inline uint8_t Read8(const uint8_t *wram, size_t wram_size, size_t address) {
  if (!wram || address >= wram_size) return 0;
  return wram[address];
}

static inline bool SceneDecorationAppend(ActionSceneEffectFrame *dst,
                                  const ActionEffectInstance *effect) {
  if (!dst || !effect) return false;
  if (dst->decoration_count >= kActionSceneDecorationMaxInstances) {
    dst->decoration_overflow = 1;
    return false;
  }
  dst->decorations[dst->decoration_count++] = *effect;
  if (effect->flags & kActionEffectFlag_Visible)
    dst->decoration_visible_count++;
  return true;
}

bool IsFillmoreForest(const uint8_t *wram, size_t size);
bool IsBloodpoolMarsh(const uint8_t *wram, size_t size);
unsigned FillmoreCaveRoom(const uint8_t *wram, size_t size);
unsigned BloodpoolCastleRoom(const uint8_t *wram, size_t size);
void CaptureFillmoreForest(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *wram, size_t size);
void CaptureFillmoreCave(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *wram, size_t size);
void CaptureBloodpoolMarsh(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *wram, size_t size);
void CaptureBloodpoolCastle(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *wram, size_t size);

void CaptureFillmoreForestScene(const ActionEnvironmentScene *, ActionSceneEffectFrame *);
void CaptureFillmoreCaveScene(const ActionEnvironmentScene *, ActionSceneEffectFrame *);
void CaptureBloodpoolMarshScene(const ActionEnvironmentScene *, ActionSceneEffectFrame *);
void CaptureBloodpoolCastleScene(const ActionEnvironmentScene *, ActionSceneEffectFrame *);

#endif
