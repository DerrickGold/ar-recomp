/* Stage-owned environmental capture. Phase: capture; read-only WRAM. */
#include "action/action_environment_capture_internal.h"

bool IsFillmoreForest(const uint8_t *wram, size_t wram_size) {
  return wram && wram_size > kActRaiserWram_Bg2Height + 1 &&
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Fillmore &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) == 1 &&
      Read16(wram, wram_size, kActRaiserWram_Bg1Width) == 4096 &&
      Read16(wram, wram_size, kActRaiserWram_Bg1Height) == 768 &&
      Read16(wram, wram_size, kActRaiserWram_Bg2Width) == 2304 &&
      Read16(wram, wram_size, kActRaiserWram_Bg2Height) == 512;
}

void CaptureFillmoreForestScene(const ActionEnvironmentScene *scene, ActionSceneEffectFrame *dst) {
  if (scene->group != kActRaiserMapGroup_Fillmore || scene->room != 1) return;
  ActionRayField_Capture(scene,dst,ActionRayField_Bundled(),0x46000000u);
}

void CaptureFillmoreForest(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *ram, size_t size) {
  ActionEnvironmentScene scene;
  if (ActionEnvironmentScene_FromWram(&scene,ram,size,observer->scene_clock))
    CaptureFillmoreForestScene(&scene,dst);
}
