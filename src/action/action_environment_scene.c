#include "action_environment_scene.h"
#include "action_environment_capture_internal.h"

static uint16_t Word(const uint8_t *p, bool big) {
  return big ? (uint16_t)((p[0] << 8) | p[1]) :
      (uint16_t)(p[0] | (p[1] << 8));
}

uint16_t ActionEnvironmentScene_Word(const ActionEnvironmentScene *s,
    unsigned bg, unsigned metatile, unsigned quadrant) {
  if (!s || bg > 1 || metatile > 255 || quadrant > 3 || !s->metatiles[bg]) return 0;
  return Word(s->metatiles[bg] + metatile * 8 + quadrant * 2, s->big_endian);
}

bool ActionEnvironmentScene_FromWram(ActionEnvironmentScene *out,
    const uint8_t *ram, size_t size, uint16_t clock) {
  if (!out || !ram || size < 0x06a0) return false;
  ActionEnvironmentScene s = {.group = Read8(ram,size,kActRaiserWram_MapGroup),
    .room = Read8(ram,size,kActRaiserWram_CurrentMap), .clock = clock};
  for (unsigned bg = 0; bg < 2; ++bg) {
    const unsigned offset = bg * kActRaiserBgLayerStateStride;
    const unsigned table = Read16(ram,size,kActRaiserWram_BgMetatileTable + offset);
    const uint16_t width = Read16(ram,size,kActRaiserWram_Bg1Width + offset);
    const uint16_t height = Read16(ram,size,kActRaiserWram_Bg1Height + offset);
    if (!ActionBgMapView_Init(&s.maps[bg],ram,size,width,height,
        Read16(ram,size,kActRaiserWram_BgMapPage + offset)))
      s.maps[bg] = (ActionBgMapView){.world_width = width, .world_height = height};
    if (table <= size && size - table >= 2048) s.metatiles[bg] = ram + table;
    s.word_mask[bg] = Read16(ram,size,0x54 + offset);
    s.attributes[bg] = (uint16_t)Read8(ram,size,0x6b + offset) << 8;
    s.camera_x[bg] = (int16_t)Read16(ram,size,kActRaiserWram_Bg1CameraX + offset);
    s.camera_y[bg] = (int16_t)Read16(ram,size,kActRaiserWram_Bg1CameraY + offset);
  }
  memcpy(s.collision,ram + 0x05a0,sizeof(s.collision));
  const unsigned end = 0x6003 + 3 * kActionBloodpoolWaterScrollRows;
  s.water_scroll_valid = size > end && ram[0x6000] == 127 &&
      !Read16(ram,size,0x6001) && !ram[end];
  if (s.water_scroll_valid) for (unsigned i = 0; i < kActionBloodpoolWaterScrollRows; ++i) {
    const unsigned at = 0x6003 + i * 3;
    if (ram[at] != 1) { s.water_scroll_valid = false; break; }
    s.water_scroll[i] = Read16(ram,size,at+1) & 1023;
  }
  *out = s;
  return true;
}


void ActionEnvironmentScene_Capture(const ActionEnvironmentScene *s,
    ActionSceneEffectFrame *frame) {
  if (!s || !frame || frame->decoration_overflow) return;
  CaptureFillmoreForestScene(s,frame);
  CaptureFillmoreCaveScene(s,frame);
  CaptureBloodpoolMarshScene(s,frame);
  CaptureBloodpoolCastleScene(s,frame);
}
