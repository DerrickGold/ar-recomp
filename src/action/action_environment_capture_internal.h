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

typedef struct SceneBgScanBounds {
  unsigned x0, y0;
  unsigned x1, y1;  /* exclusive */
} SceneBgScanBounds;

static inline bool SceneBgScanBounds_InitWindow(
    SceneBgScanBounds *bounds, const ActionBgMapView *map,
    int camera_x, int camera_y, int margin_x, int margin_y,
    bool include_partial_cells) {
  if (!bounds || !map || !map->world_width || !map->world_height ||
      margin_x < 0 || margin_y < 0)
    return false;
  int min_x = camera_x - margin_x;
  int min_y = camera_y - margin_y;
  int max_x = camera_x + kActRaiserAuthenticWidth + margin_x;
  int max_y = camera_y + kActRaiserAuthenticHeight + margin_y;
  if (max_x < 0 || max_y < 0 || min_x >= (int)map->world_width ||
      min_y >= (int)map->world_height)
    return false;
  if (min_x < 0) min_x = 0;
  if (min_y < 0) min_y = 0;
  if (max_x >= (int)map->world_width) max_x = (int)map->world_width - 1;
  if (max_y >= (int)map->world_height) max_y = (int)map->world_height - 1;
  const unsigned cell = kActionBgMetatilePixels;
  const unsigned align_bias = include_partial_cells ? 0u : cell - 1u;
  bounds->x0 = ((unsigned)min_x + align_bias) / cell * cell;
  bounds->y0 = ((unsigned)min_y + align_bias) / cell * cell;
  bounds->x1 = ((unsigned)max_x / cell + 1u) * cell;
  bounds->y1 = ((unsigned)max_y / cell + 1u) * cell;
  if (bounds->x1 > map->world_width) bounds->x1 = map->world_width;
  if (bounds->y1 > map->world_height) bounds->y1 = map->world_height;
  return bounds->x0 < bounds->x1 && bounds->y0 < bounds->y1;
}

static inline bool SceneBgScanBounds_Init(SceneBgScanBounds *bounds,
                                   const ActionBgMapView *map,
                                   bool camera_bounded,
                                   int camera_x, int camera_y) {
  if (!bounds || !map || !map->world_width || !map->world_height)
    return false;
  if (!camera_bounded) {
    *bounds = (SceneBgScanBounds){
      .x1 = map->world_width,
      .y1 = map->world_height,
    };
    return true;
  }
  return SceneBgScanBounds_InitWindow(
      bounds, map, camera_x, camera_y,
      kActRaiserAuthenticWidth, kActRaiserAuthenticWidth, false);
}

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
    const uint8_t *wram, size_t size,bool native_moon,bool native_marsh);
void CaptureBloodpoolCastle(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *wram, size_t size,bool native_castle);

void CaptureFillmoreForestScene(const ActionEnvironmentScene *, ActionSceneEffectFrame *);
void CaptureFillmoreCaveScene(const ActionEnvironmentScene *, ActionSceneEffectFrame *);
void CaptureBloodpoolMarshScene(const ActionEnvironmentScene *, ActionSceneEffectFrame *);
void CaptureBloodpoolCastleScene(const ActionEnvironmentScene *, ActionSceneEffectFrame *);

void CaptureFillmoreCaveFiltered(ActionEffectObserver *observer,ActionSceneEffectFrame *dst,
    const uint8_t *ram,size_t size,bool native_water, bool native_atmosphere);

#endif
