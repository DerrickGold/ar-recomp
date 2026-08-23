#include "action_room_scene.h"

#include <string.h>

#include "action/action_bg_metatile.h"
#include "actraiser_game.h"
#include "quintet_lzss.h"

enum {
  kAssetScriptBase = 0x05 * 0x8000,
  kAssetScriptHeaderBytes = 3,
  kAssetScriptEnd = 0x06 * 0x8000,
  kVideoProfileBase = 0x02 * 0x8000 + 0x093E,
  kRasterWaveformBase = 0x02 * 0x8000 + 0x16D4,
  kRasterMosaicWaveWindowBase = kRasterWaveformBase +
      kActionRoomSceneRasterWaveformBytes,
  kCharacterBankBytes = 0x2000,
  kCharacterVramBytes = kCharacterBankBytes * 2,
  kExtraCharacterUploadDestination = 0x6000,
};

static bool Decompress(const uint8_t *rom, size_t rom_size, size_t offset,
                       uint8_t *out, size_t expected_size) {
  if (!rom || !out || offset > rom_size || rom_size - offset < 2)
    return false;
  return QuintetLzss_DecompressAsset(
      rom + offset, rom_size - offset, out, expected_size, NULL);
}

static uint32_t Read24(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
      ((uint32_t)bytes[2] << 16);
}

static int HighestBit(uint8_t command) {
  for (int bit = 7; bit >= 0; bit--)
    if (command & (1u << bit)) return bit;
  return -1;
}

static int OperandBytesForBit(int bit) {
  static const uint8_t kOperandBytes[8] = {6, 5, 3, 1, 4, 7, 6, 6};
  return bit >= 0 && bit < 8 ? kOperandBytes[bit] : 0;
}

static bool ApplyCommand(ActionRoomScene *scene,
                         const uint8_t *rom, size_t rom_size,
                         int bit, const uint8_t *ops) {
  if (!scene || !rom || !ops) return false;
  if (bit == 7) {
    if (ops[1] <= ops[0]) return false;
    const size_t bytes = (size_t)(ops[1] - ops[0]) << 9;
    const size_t destination = (size_t)ops[2] << 9;
    const size_t source = Read24(ops + 3);
    if (bytes != kCharacterBankBytes)
      return true;
    /* `$02:B28E` expands every compressed character upload through
     * `$7E:6000-$7FFF` before copying it to VRAM. The persistent raster
     * builders later reuse `$6000/$6800/$7000`; several write only the first
     * of Mode-2 HDMA's two data bytes, intentionally inheriting the other
     * byte from this last character upload. Preserve that workspace even for
     * VRAM destinations the stable BG1/BG2 compositor does not consume. */
    if (!Decompress(rom, rom_size, source, scene->raster_workspace, bytes))
      return false;
    scene->have_raster_workspace = true;
    if (destination < kCharacterVramBytes) {
      if (bytes > kCharacterVramBytes - destination)
        return false;
      memcpy(scene->characters + destination, scene->raster_workspace, bytes);
      scene->have_character_bank[destination / kCharacterBankBytes] = true;
    } else if (destination == kExtraCharacterUploadDestination) {
      memcpy(scene->extra_characters, scene->raster_workspace, bytes);
      scene->have_extra_characters = true;
    }
    return true;
  }
  if (bit == 6) {
    if (ops[1] < ops[0]) return false;
    const size_t bytes = (size_t)(ops[1] - ops[0]) * 2;
    const size_t destination = (size_t)ops[2] * 2;
    const size_t source = Read24(ops + 3) + (size_t)ops[0] * 2;
    if (destination >= kActionRoomScenePaletteBytes) return true;
    if (!bytes || bytes > kActionRoomScenePaletteBytes - destination ||
        source > rom_size || bytes > rom_size - source)
      return false;
    memcpy(scene->palette + destination, rom + source, bytes);
    scene->have_palette = true;
    return true;
  }
  if (bit == 5) {
    if (ops[3] != 1 && ops[3] != 2) return true;
    ActionRoomSceneBg *bg = &scene->bg[ops[3] - 1];
    if (!Decompress(rom, rom_size, Read24(ops + 4), bg->metatiles,
                    kActionRoomSceneMetatileBytes))
      return false;
    bg->have_metatiles = true;
    return true;
  }
  if (bit == 4) {
    if (ops[0] != 1 && ops[0] != 2) return true;
    ActionRoomSceneBg *bg = &scene->bg[ops[0] - 1];
    const size_t source = Read24(ops + 1);
    if (source > rom_size || rom_size - source < 4) return false;
    const size_t pages = (size_t)rom[source] * rom[source + 1];
    if (!pages || pages >
            kActionRoomSceneMaxMapBytes / kActionRoomSceneMapPageBytes)
      return false;
    bg->pages_wide = rom[source];
    bg->pages_high = rom[source + 1];
    bg->map_size = pages * kActionRoomSceneMapPageBytes;
    if (!Decompress(rom, rom_size, source + 2, bg->map, bg->map_size))
      return false;
    bg->have_map = true;
    return true;
  }
  if (bit == 3) {
    scene->video_profile_index = ops[0];
    scene->have_video_profile = true;
  }
  return true;
}

static ActionRoomRasterEffect RasterEffectForRoom(uint8_t group,
                                                  uint8_t map) {
  if (group == 0x01 && map == 0x04)
    return kActionRoomRaster_Bg2WaveWithFrame;
  if (group == 0x02 && map == 0x01)
    return kActionRoomRaster_Bg2LowerPerspective;
  if (group == 0x02 && (map == 0x02 || map == 0x03))
    return kActionRoomRaster_Bg2VerticalRipple;
  if (group == 0x04 && map == 0x05)
    return kActionRoomRaster_Bg2MosaicWave;
  if (group == 0x04 && map == 0x01)
    return kActionRoomRaster_Bg2LayeredParallax;
  if (group == 0x06 && (map == 0x01 || map == 0x05))
    return kActionRoomRaster_Bg2ParallaxPerspective;
  if (group == 0x06 && map == 0x08)
    return kActionRoomRaster_Bg2AcceleratingWave;
  if (group == 0x07 && map == 0x01)
    return kActionRoomRaster_Bg2OpposingBandMotion;
  if (group == 0x07 && map >= 0x02 && map <= 0x07)
    return kActionRoomRaster_Bg2CameraParallaxBands;
  if (group == 0x07 && map == 0x08)
    return kActionRoomRaster_DualBgOpposedWaves;
  return kActionRoomRaster_None;
}

static bool RasterEntryCameraForRoom(uint8_t group, uint8_t map,
                                     uint16_t *camera_x) {
  if (!camera_x) return false;
  if (group == 0x06 && map == 0x05) {
    *camera_x = 0x01c0;
    return true;
  }
  if (group == 0x07 && map >= 0x02 && map <= 0x07) {
    static const uint16_t kDeathHeimRematchEntryCamera[6] = {
      0x0010, 0x0000, 0x0000, 0x00d0, 0x0000, 0x0090,
    };
    *camera_x = kDeathHeimRematchEntryCamera[map - 0x02];
    return true;
  }
  return false;
}

bool ActionRoomScene_Load(ActionRoomScene *scene,
                          const uint8_t *rom, size_t rom_size,
                          uint8_t group, uint8_t map) {
  if (!scene) return false;
  memset(scene, 0, sizeof(*scene));
  scene->group = group;
  scene->map = map;
  scene->raster_effect = RasterEffectForRoom(group, map);
  scene->have_raster_entry_camera_x = RasterEntryCameraForRoom(
      group, map, &scene->raster_entry_camera_x);
  if (!rom || !ActRaiser_IsActionMap(group, map) ||
      rom_size <= kAssetScriptBase + kAssetScriptHeaderBytes)
    return false;

  size_t cursor = kAssetScriptBase + kAssetScriptHeaderBytes;
  const size_t end = rom_size < kAssetScriptEnd ? rom_size : kAssetScriptEnd;
  bool found = false;
  while (cursor + 3 <= end) {
    const uint8_t entry_group = rom[cursor++];
    const uint8_t entry_map = rom[cursor++];
    const bool apply = entry_group == group && entry_map <= map;
    while (cursor < end) {
      const uint8_t command = rom[cursor++];
      if (!command) break;
      const int bit = HighestBit(command);
      const int operand_bytes = OperandBytesForBit(bit);
      if (!operand_bytes || (size_t)operand_bytes > end - cursor) return false;
      if (apply && !ApplyCommand(scene, rom, rom_size, bit, rom + cursor))
        return false;
      cursor += (size_t)operand_bytes;
    }
    if (cursor > end) return false;
    if (entry_group == group && entry_map == map) found = true;
    if (entry_group > group || (entry_group == group && entry_map >= map))
      break;
  }
  if (!found) return false;
  if (kRasterWaveformBase > rom_size ||
      kActionRoomSceneRasterWaveformBytes >
          rom_size - kRasterWaveformBase)
    return false;
  memcpy(scene->raster_waveform, rom + kRasterWaveformBase,
         kActionRoomSceneRasterWaveformBytes);
  scene->have_raster_waveform = true;
  if (kRasterMosaicWaveWindowBase > rom_size ||
      kActionRoomSceneRasterMosaicWaveWindowBytes >
          rom_size - kRasterMosaicWaveWindowBase)
    return false;
  memcpy(scene->raster_mosaic_wave_window,
         rom + kRasterMosaicWaveWindowBase,
         kActionRoomSceneRasterMosaicWaveWindowBytes);
  scene->have_raster_mosaic_wave_window = true;
  if (scene->have_video_profile) {
    const size_t offset = kVideoProfileBase +
        (size_t)scene->video_profile_index * kActionRoomSceneVideoProfileBytes;
    if (offset > rom_size ||
        kActionRoomSceneVideoProfileBytes > rom_size - offset)
      return false;
    memcpy(scene->video_profile, rom + offset,
           kActionRoomSceneVideoProfileBytes);
  }
  return true;
}

static const ActionRoomSceneBg *GetBg(const ActionRoomScene *scene,
                                      uint8_t bg_layer) {
  return scene && bg_layer >= 1 && bg_layer <= kActionRoomSceneBgCount
      ? &scene->bg[bg_layer - 1] : NULL;
}

bool ActionRoomScene_HasBackground(const ActionRoomScene *scene,
                                   uint8_t bg_layer) {
  const ActionRoomSceneBg *bg = GetBg(scene, bg_layer);
  return bg && bg->have_map && bg->have_metatiles && bg->map_size &&
      bg->pages_wide && bg->pages_high;
}

unsigned ActionRoomScene_TileWidth(const ActionRoomScene *scene,
                                   uint8_t bg_layer) {
  const ActionRoomSceneBg *bg = GetBg(scene, bg_layer);
  return ActionRoomScene_HasBackground(scene, bg_layer)
      ? (unsigned)bg->pages_wide * 32u : 0;
}

unsigned ActionRoomScene_TileHeight(const ActionRoomScene *scene,
                                    uint8_t bg_layer) {
  const ActionRoomSceneBg *bg = GetBg(scene, bg_layer);
  return ActionRoomScene_HasBackground(scene, bg_layer)
      ? (unsigned)bg->pages_high * 32u : 0;
}

size_t ActionRoomScene_TileCount(const ActionRoomScene *scene,
                                 uint8_t bg_layer) {
  return (size_t)ActionRoomScene_TileWidth(scene, bg_layer) *
      ActionRoomScene_TileHeight(scene, bg_layer);
}

uint16_t ActionRoomScene_BgAttributes(const ActionRoomScene *scene,
                                      uint8_t bg_layer) {
  if (!GetBg(scene, bg_layer)) return 0;
  uint16_t attributes = (uint16_t)(bg_layer == 1
      ? kActionRoomSceneBg1AttributeByte : kActionRoomSceneBg2AttributeByte)
      << 8;
  if (scene->have_video_profile &&
      (scene->video_profile[4] & (1u << (bg_layer - 1))))
    attributes |= 0x2000;
  return attributes;
}

bool ActionRoomScene_LookupTile(const ActionRoomScene *scene,
                                uint8_t bg_layer,
                                unsigned tile_x, unsigned tile_y,
                                uint16_t *entry, uint8_t *metatile) {
  const ActionRoomSceneBg *bg = GetBg(scene, bg_layer);
  const unsigned tile_width = ActionRoomScene_TileWidth(scene, bg_layer);
  const unsigned tile_height = ActionRoomScene_TileHeight(scene, bg_layer);
  if (!bg || !entry || tile_x >= tile_width || tile_y >= tile_height)
    return false;
  const size_t page = (size_t)(tile_y >> 5) * bg->pages_wide +
      (tile_x >> 5);
  const size_t in_page = (size_t)((tile_y >> 1) & 15u) * 16u +
      ((tile_x >> 1) & 15u);
  const size_t map_offset = page * kActionRoomSceneMapPageBytes + in_page;
  if (map_offset >= bg->map_size) return false;
  const uint8_t id = bg->map[map_offset];
  const unsigned quadrant = ((tile_y & 1u) << 1) | (tile_x & 1u);
  const uint8_t *source = bg->metatiles + (size_t)id * 8 + quadrant * 2;
  const uint16_t definition =
      (uint16_t)(source[1] | ((uint16_t)source[0] << 8));
  *entry = ActionBg_ComposeTilemapWord(
      definition, kActionRoomSceneTileWordMask,
      ActionRoomScene_BgAttributes(scene, bg_layer));
  if (metatile) *metatile = id;
  return true;
}

bool ActionRoomScene_ExpandBg(const ActionRoomScene *scene,
                              uint8_t bg_layer,
                              uint16_t *entries, size_t entry_count) {
  const unsigned width = ActionRoomScene_TileWidth(scene, bg_layer);
  const unsigned height = ActionRoomScene_TileHeight(scene, bg_layer);
  const size_t needed = (size_t)width * height;
  if (!entries || !needed || entry_count < needed) return false;
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++)
      if (!ActionRoomScene_LookupTile(
              scene, bg_layer, x, y, entries + (size_t)y * width + x, NULL))
        return false;
  return true;
}

unsigned ActionRoomScene_CharacterAnimationPhaseCount(
    const ActionRoomScene *scene) {
  return scene && scene->have_video_profile
      ? scene->video_profile[23] & 0x0Fu : 0;
}

unsigned ActionRoomScene_CharacterAnimationStride(
    const ActionRoomScene *scene) {
  return scene && scene->have_video_profile
      ? ((scene->video_profile[23] >> 4) & 7u) << 7 : 0;
}

unsigned ActionRoomScene_CharacterAnimationCadence(
    const ActionRoomScene *scene) {
  return scene && scene->have_video_profile
      ? scene->video_profile[24] & 0x7Fu : 0;
}

uint16_t ActionRoomScene_CharacterAnimationTarget(
    const ActionRoomScene *scene) {
  return scene && scene->have_video_profile && (scene->video_profile[23] & 0x80)
      ? 0x1000 : 0x0000;
}

bool ActionRoomScene_CharacterAnimationContinues(
    const ActionRoomScene *scene) {
  return scene && scene->have_video_profile &&
      (scene->video_profile[24] & 0x80) != 0;
}

bool ActionRoomScene_HasCharacterAnimation(const ActionRoomScene *scene) {
  const unsigned count =
      ActionRoomScene_CharacterAnimationPhaseCount(scene);
  const unsigned stride = ActionRoomScene_CharacterAnimationStride(scene);
  const unsigned cadence = ActionRoomScene_CharacterAnimationCadence(scene);
  return count && stride && cadence &&
      (size_t)count * stride <= kActionRoomSceneAnimationWindowBytes;
}

unsigned ActionRoomScene_ResolveCharacterAnimationPhase(
    const ActionRoomScene *scene, uint32_t game_frame, int explicit_phase) {
  const unsigned count =
      ActionRoomScene_CharacterAnimationPhaseCount(scene);
  if (!ActionRoomScene_HasCharacterAnimation(scene) || !count) return 0;
  if (explicit_phase >= 0) return (unsigned)explicit_phase % count;
  const unsigned cadence = ActionRoomScene_CharacterAnimationCadence(scene);
  return (game_frame / cadence) & (count - 1u);
}

bool ActionRoomScene_BuildCharacters(const ActionRoomScene *scene,
                                     uint32_t game_frame,
                                     int explicit_phase,
                                     uint8_t *characters,
                                     size_t character_bytes) {
  if (!scene || !characters ||
      character_bytes < kActionRoomSceneCharacterBytes ||
      !scene->have_character_bank[0] || !scene->have_character_bank[1])
    return false;
  memcpy(characters, scene->characters, kActionRoomSceneCharacterBytes);
  if (!ActionRoomScene_HasCharacterAnimation(scene)) return true;
  const size_t target =
      (size_t)ActionRoomScene_CharacterAnimationTarget(scene) * 2;
  const size_t stride = ActionRoomScene_CharacterAnimationStride(scene);
  const size_t phase = ActionRoomScene_ResolveCharacterAnimationPhase(
      scene, game_frame, explicit_phase);
  const size_t source = target + phase * stride;
  if (target > kActionRoomSceneCharacterBytes ||
      stride > kActionRoomSceneCharacterBytes - target ||
      source > kActionRoomSceneCharacterBytes ||
      stride > kActionRoomSceneCharacterBytes - source)
    return false;
  memcpy(characters + target, scene->characters + source, stride);
  return true;
}

bool ActionRoomScene_HasBg2PageCycle(const ActionRoomScene *scene) {
  return scene && scene->have_video_profile &&
      scene->group == 0x04 && (scene->map == 0x02 || scene->map == 0x03) &&
      scene->bg[1].have_map && scene->bg[1].pages_wide == 2 &&
      scene->bg[1].pages_high == 2 && scene->video_profile[19] != 0;
}

unsigned ActionRoomScene_ResolveBg2PagePhase(
    const ActionRoomScene *scene, uint32_t game_frame, int explicit_phase) {
  if (!ActionRoomScene_HasBg2PageCycle(scene)) return 0;
  return explicit_phase >= 0 ? (unsigned)explicit_phase & 3u
                             : (game_frame / 5u) & 3u;
}

unsigned ActionRoomScene_Bg2PageIndex(const ActionRoomScene *scene,
                                      uint32_t game_frame,
                                      int explicit_phase) {
  static const uint8_t kNativePageOrder[4] = {1, 2, 3, 0};
  return kNativePageOrder[ActionRoomScene_ResolveBg2PagePhase(
      scene, game_frame, explicit_phase)];
}

static uint16_t ResolveParallax(uint32_t camera, uint8_t ratio,
                                unsigned extent, unsigned viewport) {
  const unsigned numerator = ratio >> 4;
  const unsigned denominator = ratio & 0x0f;
  uint32_t result = denominator ? camera * numerator / denominator : 0;
  if (extent >= 0x300 && result + viewport >= extent)
    result = extent - viewport;
  return (uint16_t)result & 0x03ff;
}

typedef struct RasterWriter16 {
  uint16_t *values;
  unsigned line;
  uint16_t held;
} RasterWriter16;

static void RasterWrite16(RasterWriter16 *writer, unsigned count,
                          uint16_t value) {
  if (!writer || !count) return;
  writer->held = value & 0x03ff;
  while (count-- && writer->line < kActionRoomSceneFrameHeight)
    writer->values[writer->line++] = writer->held;
}

static void RasterFinish16(RasterWriter16 *writer) {
  if (!writer) return;
  while (writer->line < kActionRoomSceneFrameHeight)
    writer->values[writer->line++] = writer->held;
}

static uint16_t Swap16(uint16_t value) {
  return (uint16_t)((value << 8) | (value >> 8));
}

/* HDMA mode 2 writes two successive table bytes to the same scroll register.
 * After both SNES latch writes, the resolved 10-bit scroll is simply the
 * first byte followed by the low two bits of the second byte. Some original
 * builders only replace the first data byte and leave the second byte in the
 * shared character-decompression workspace. */
static uint16_t ResolveMode2Bytes(uint8_t first, uint8_t second) {
  return (uint16_t)(first | ((uint16_t)second << 8)) & 0x03ff;
}

static uint16_t ResolveInheritedMode2(const ActionRoomScene *scene,
                                      size_t second_byte_offset,
                                      uint8_t first) {
  const uint8_t second = scene->have_raster_workspace &&
      second_byte_offset < kActionRoomSceneRasterWorkspaceBytes
      ? scene->raster_workspace[second_byte_offset] : 0;
  return ResolveMode2Bytes(first, second);
}

static void BuildRasterScroll(const ActionRoomScene *scene,
                              const ActionRoomSceneFrameRequest *request,
                              ActionRoomSceneFrameState *state) {
  /* The table scanned during game frame N was built by the preceding main-loop
   * update while $88 still held N-1. Character/page animation uses the current
   * visible frame, but persistent HDMA therefore has this intentional one-tick
   * phase lag. The live pre-scanline oracle pins it. */
  const uint16_t frame = (uint16_t)(request->game_frame - 1u);
  uint16_t camera_x = (uint16_t)(request->have_raster_camera_x
      ? request->raster_camera_x : request->camera_x);
  if (request->raster_entry_frame && scene->have_raster_entry_camera_x)
    camera_x = scene->raster_entry_camera_x;
  RasterWriter16 bg1 = {
    .values = state->bg_hscroll[0],
    .held = state->bg_hscroll[0][0],
  };
  RasterWriter16 bg2 = {
    .values = state->bg_hscroll[1],
    .held = state->bg_hscroll[1][0],
  };
  RasterWriter16 bg2v = {
    .values = state->bg_vscroll[1],
    .held = state->bg_vscroll[1][0],
  };

  switch (scene->raster_effect) {
    case kActionRoomRaster_Bg2WaveWithFrame: {
      uint8_t phase = (uint8_t)(frame >> 1);
      for (unsigned i = 0; i < 0x6f; i++) {
        const uint8_t value = (uint8_t)(
            scene->raster_waveform[phase++] + (uint8_t)frame);
        RasterWrite16(&bg2, 2, ResolveInheritedMode2(
            scene, (size_t)i * 3u + 2u, value));
      }
      RasterFinish16(&bg2);
      break;
    }
    case kActionRoomRaster_Bg2LowerPerspective: {
      RasterWrite16(&bg2, 0x7f, 0);
      const uint16_t step = (uint16_t)((frame + camera_x) << 1);
      uint16_t value = step;
      for (unsigned i = 0; i < 96; i++) {
        /* The 16-bit STA begins on the count byte; the following 8-bit count
         * store replaces its low byte. HDMA therefore sees value>>8 followed
         * by the inherited byte at entry+2. */
        RasterWrite16(&bg2, 1, ResolveInheritedMode2(
            scene, 3u + (size_t)i * 3u + 2u, (uint8_t)(value >> 8)));
        value = (uint16_t)(value + step);
      }
      RasterFinish16(&bg2);
      break;
    }
    case kActionRoomRaster_Bg2VerticalRipple: {
      RasterWrite16(&bg2v, 0x7f,
                    ResolveInheritedMode2(scene, 2u, 0));
      uint8_t value = (uint8_t)frame;
      if (((uint8_t)(frame >> 8)) & 1) value ^= 0xff;
      value = (value >> 4) & 0x0f;
      for (unsigned i = 0; i < 32; i++) {
        RasterWrite16(&bg2v, 1, ResolveInheritedMode2(
            scene, 3u + (size_t)i * 3u + 2u, value));
        value = (uint8_t)(value - 5);
      }
      RasterWrite16(&bg2v, 1, ResolveInheritedMode2(
          scene, 3u + 32u * 3u + 2u, 0));
      RasterFinish16(&bg2v);
      break;
    }
    case kActionRoomRaster_Bg2MosaicWave: {
      if (request->raster_entry_frame) {
        memset(state->mosaic, 0x02, sizeof(state->mosaic));
        break;
      }
      /* The native routine loads `$88` with 8-bit A before shifting. The high
       * byte of the 16-bit game clock therefore never contributes to phase. */
      uint8_t phase = (uint8_t)frame;
      phase >>= 2;
      unsigned line = 0;
      uint8_t held = 0;
      for (unsigned i = 0; i < 0x70; i++) {
        /* `$02:9382` stores the phase to DP `$00` in 8-bit A mode, then
         * reloads it as a 16-bit X index. DP `$01` remains one throughout
         * action mode, selecting `$02:97D4+phase` rather than the nominal
         * `$02:96D4` waveform page. Preserve that original-ROM quirk in the
         * standalone scene model instead of depending on live scratch WRAM. */
        held = (uint8_t)(
            (scene->raster_mosaic_wave_window[phase++] << 4) & 0x10) |
            0x02;
        for (unsigned repeat = 0; repeat < 2; repeat++)
          if (line < kActionRoomSceneFrameHeight)
            state->mosaic[line++] = held;
      }
      while (line < kActionRoomSceneFrameHeight)
        state->mosaic[line++] = held;
      break;
    }
    case kActionRoomRaster_Bg2LayeredParallax: {
      uint16_t value = (uint16_t)((frame << 1) + (camera_x >> 1));
      RasterWrite16(&bg2, 0x3f, value);
      value >>= 1;
      RasterWrite16(&bg2, 0x10, value);
      value >>= 1;
      RasterWrite16(&bg2, 0x08, value);
      value >>= 1;
      RasterWrite16(&bg2, 0x08, value);
      RasterWrite16(&bg2, 0x10, 0);
      value = camera_x >> 4;
      RasterWrite16(&bg2, 0x08, value);
      RasterWrite16(&bg2, 0x10, camera_x >> 3);
      RasterWrite16(&bg2, 0x28, camera_x >> 2);
      RasterWrite16(&bg2, 0x50, camera_x >> 1);
      RasterFinish16(&bg2);
      break;
    }
    case kActionRoomRaster_Bg2ParallaxPerspective: {
      uint16_t value = (uint16_t)((frame << 2) + (camera_x >> 1));
      RasterWrite16(&bg2, 0x1e, value);
      value >>= 1;
      RasterWrite16(&bg2, 0x10, value);
      value >>= 1;
      RasterWrite16(&bg2, 0x10, value);
      value >>= 1;
      RasterWrite16(&bg2, 0x08, value);
      value >>= 1;
      RasterWrite16(&bg2, 0x08, value);
      RasterWrite16(&bg2, 0x70, camera_x >> 1);
      const uint16_t step = (uint16_t)((frame + camera_x) << 2);
      value = step;
      for (unsigned i = 0; i < 0x20; i++) {
        RasterWrite16(&bg2, 1, Swap16(value));
        value = (uint16_t)(value + step);
        value = (uint16_t)(value + step);
      }
      RasterFinish16(&bg2);
      break;
    }
    case kActionRoomRaster_Bg2AcceleratingWave: {
      uint8_t phase = (uint8_t)(frame >> 1);
      uint8_t step = 1;
      for (unsigned i = 0; i < 0x6f; i++) {
        const uint8_t source = phase;
        phase = (uint8_t)(phase + step++);
        RasterWrite16(&bg2, 2, ResolveInheritedMode2(
            scene, 0x0800u + (size_t)i * 3u + 2u,
            scene->raster_waveform[source]));
      }
      RasterFinish16(&bg2);
      break;
    }
    case kActionRoomRaster_Bg2OpposingBandMotion: {
      const uint16_t reverse = (uint16_t)(0u - frame);
      RasterWrite16(&bg2, 0x4f, 0);
      RasterWrite16(&bg2, 0x40, 0);
      RasterWrite16(&bg2, 0x10, reverse >> 2);
      RasterWrite16(&bg2, 0x10, reverse >> 1);
      RasterWrite16(&bg2, 0x10, reverse);
      RasterWrite16(&bg2, 0x08, (uint16_t)(reverse << 1));
      RasterWrite16(&bg2, 0x04,
                    (uint16_t)((reverse << 1) + reverse));
      RasterWrite16(&bg2, 0x08, (uint16_t)(reverse << 2));
      RasterWrite16(&bg2, 0x04, (uint16_t)(frame << 1));
      RasterWrite16(&bg2, 0x08, frame);
      RasterWrite16(&bg2, 0x10, frame >> 1);
      RasterFinish16(&bg2);
      break;
    }
    case kActionRoomRaster_Bg2CameraParallaxBands:
      RasterWrite16(&bg2, 0x4f,
                    ResolveInheritedMode2(scene, 0x1002u, 0));
      RasterWrite16(&bg2, 0x40, camera_x >> 2);
      RasterWrite16(&bg2, 0x60, camera_x >> 1);
      RasterFinish16(&bg2);
      break;
    case kActionRoomRaster_DualBgOpposedWaves: {
      uint8_t phase = (uint8_t)(frame >> 1);
      for (unsigned i = 0; i < 0x6f; i++) {
        const uint8_t wave = scene->raster_waveform[phase++];
        RasterWrite16(&bg2, 2, ResolveInheritedMode2(
            scene, (size_t)i * 3u + 2u,
            (uint8_t)(wave + (uint8_t)frame)));
        RasterWrite16(&bg1, 2, ResolveInheritedMode2(
            scene, 0x0800u + (size_t)i * 3u + 2u,
            (uint8_t)(0u - wave + 0x40)));
      }
      RasterFinish16(&bg1);
      RasterFinish16(&bg2);
      break;
    }
    case kActionRoomRaster_None:
    default:
      break;
  }
}

bool ActionRoomScene_BuildFrameState(
    const ActionRoomScene *scene,
    const ActionRoomSceneFrameRequest *request,
    ActionRoomSceneFrameState *state) {
  if (!scene || !request || !state || !scene->have_video_profile ||
      !scene->have_raster_waveform ||
      (scene->raster_effect == kActionRoomRaster_Bg2MosaicWave &&
       !scene->have_raster_mosaic_wave_window) || request->camera_x < 0 ||
      request->camera_y < 0)
    return false;
  memset(state, 0, sizeof(*state));
  state->camera_x = request->camera_x;
  state->camera_y = request->camera_y;
  state->game_frame = request->game_frame;
  state->screen_enabled[0] = scene->video_profile[0];
  state->screen_enabled[1] = scene->video_profile[1];
  state->screen_windowed[0] = scene->video_profile[0];
  state->screen_windowed[1] = scene->video_profile[1];
  state->cgwsel = scene->video_profile[2];
  state->cgadsub = scene->video_profile[3];
  state->bgsc[0] = (uint8_t)(0x60 | (scene->video_profile[5] & 3));
  state->bgsc[1] = (uint8_t)(0x70 | ((scene->video_profile[5] >> 2) & 3));
  /* The video profile is applied before the action background bootstrap has
   * finalized its resident tilemap topology. Every Death Heim rematch uses a
   * one-page BG2 at $7000, and the final arena likewise collapses its
   * provisional profile-$2E BG2 size after the first setup frame. */
  if (!request->raster_entry_frame &&
      (scene->raster_effect == kActionRoomRaster_Bg2CameraParallaxBands ||
       scene->raster_effect == kActionRoomRaster_DualBgOpposedWaves))
    state->bgsc[1] = 0x70;
  state->bgmode = scene->video_profile[6];
  state->brightness = 15;
  state->raster_effect = scene->raster_effect;
  state->animation_phase = (uint8_t)
      ActionRoomScene_ResolveCharacterAnimationPhase(
          scene, request->game_frame, request->animation_phase);
  state->bg2_page_phase = (uint8_t)ActionRoomScene_ResolveBg2PagePhase(
      scene, request->game_frame, request->page_phase);
  state->bg2_page_index = (uint8_t)ActionRoomScene_Bg2PageIndex(
      scene, request->game_frame, request->page_phase);
  if (ActionRoomScene_HasBg2PageCycle(scene))
    state->bgsc[1] = (uint8_t)(0x70 | (state->bg2_page_index << 2));
  for (unsigned bg = 0; bg < kActionRoomSceneBgCount; bg++)
    if (request->bgsc_override_mask & (1u << bg))
      state->bgsc[bg] = request->bgsc_override[bg];

  const uint16_t base_h[kActionRoomSceneBgCount] = {
    (uint16_t)request->camera_x & 0x03ff,
    ResolveParallax((uint32_t)request->camera_x, scene->video_profile[9],
                    ActionRoomScene_TileWidth(scene, 2) * 8u, 0x100),
  };
  const uint16_t base_v[kActionRoomSceneBgCount] = {
    (uint16_t)request->camera_y & 0x03ff,
    ResolveParallax((uint32_t)request->camera_y, scene->video_profile[10],
                    ActionRoomScene_TileHeight(scene, 2) * 8u, 0x0e0),
  };
  for (unsigned bg = 0; bg < kActionRoomSceneBgCount; bg++)
    for (unsigned line = 0; line < kActionRoomSceneFrameHeight; line++) {
      state->bg_hscroll[bg][line] = base_h[bg];
      state->bg_vscroll[bg][line] = base_v[bg];
    }
  BuildRasterScroll(scene, request, state);
  return true;
}

typedef struct NativePixel {
  uint8_t palette;
  uint8_t layer;
  uint8_t rank;
} NativePixel;

static unsigned WrapCoordinate(int coordinate, unsigned extent) {
  if (!extent) return 0;
  int result = coordinate % (int)extent;
  return (unsigned)(result < 0 ? result + (int)extent : result);
}

static bool LookupFrameTile(const ActionRoomScene *scene,
                            const ActionRoomSceneFrameState *state,
                            unsigned bg, unsigned pixel_x, unsigned pixel_y,
                            uint16_t *entry, unsigned *fine_x,
                            unsigned *fine_y) {
  unsigned width = ActionRoomScene_TileWidth(scene, (uint8_t)(bg + 1)) * 8u;
  unsigned height = ActionRoomScene_TileHeight(scene, (uint8_t)(bg + 1)) * 8u;
  if (!width || !height || !entry || !fine_x || !fine_y) return false;
  unsigned x = pixel_x;
  unsigned y = pixel_y;
  if (bg == 1 && ActionRoomScene_HasBg2PageCycle(scene)) {
    const unsigned page = state->bg2_page_index;
    x = (x & 0xffu) + (page & 1u) * 256u;
    y = (y & 0xffu) + (page >> 1) * 256u;
  } else {
    x = WrapCoordinate((int)x, width);
    y = WrapCoordinate((int)y, height);
  }
  *fine_x = x & 7u;
  *fine_y = y & 7u;
  return ActionRoomScene_LookupTile(
      scene, (uint8_t)(bg + 1), x >> 3, y >> 3, entry, NULL);
}

static uint8_t SampleCharacter(const ActionRoomScene *scene,
                               const uint8_t *characters, uint16_t entry,
                               unsigned fine_x, unsigned fine_y) {
  unsigned tile = entry & 0x03ffu;
  const uint8_t *source = characters;
  if (tile >= kActionRoomSceneCharacterBytes / 32u) {
    if (!scene->have_extra_characters || tile < 0x200 || tile >= 0x300)
      return 0;
    source = scene->extra_characters;
    tile -= 0x200;
  }
  if (entry & 0x4000) fine_x = 7u - fine_x;
  if (entry & 0x8000) fine_y = 7u - fine_y;
  const size_t address = (size_t)tile * 32u + fine_y * 2u;
  const unsigned bit = 7u - fine_x;
  return (uint8_t)(((source[address] >> bit) & 1u) |
      (((source[address + 1] >> bit) & 1u) << 1) |
      (((source[address + 16] >> bit) & 1u) << 2) |
      (((source[address + 17] >> bit) & 1u) << 3));
}

static NativePixel SampleNativeLayer(
    const ActionRoomScene *scene, const ActionRoomSceneFrameState *state,
    const uint8_t *characters, unsigned bg, unsigned screen_x,
    unsigned output_y) {
  NativePixel result = {0, 5, 0};
  const uint8_t mosaic = state->mosaic[output_y];
  const unsigned mosaic_size = (mosaic >> 4) + 1u;
  const bool mosaic_enabled = mosaic_size > 1 && (mosaic & (1u << bg));
  unsigned sample_x = screen_x;
  unsigned sample_line = output_y + 1u;
  if (mosaic_enabled) {
    sample_x -= sample_x % mosaic_size;
    sample_line -= sample_line % mosaic_size;
  }
  const unsigned source_x =
      (state->bg_hscroll[bg][output_y] + sample_x) & 0x03ffu;
  const unsigned source_y =
      (state->bg_vscroll[bg][output_y] + sample_line) & 0x03ffu;
  uint16_t entry;
  unsigned fine_x, fine_y;
  if (!LookupFrameTile(scene, state, bg, source_x, source_y,
                       &entry, &fine_x, &fine_y))
    return result;
  const uint8_t pixel = SampleCharacter(
      scene, characters, entry, fine_x, fine_y);
  if (!pixel) return result;
  result.palette = (uint8_t)(((entry >> 10) & 7u) * 16u + pixel);
  result.layer = (uint8_t)bg;
  const bool high = (entry & 0x2000) != 0;
  result.rank = (uint8_t)(bg == 0 ? (high ? 12 : 8)
                                  : (high ? 11 : 7));
  return result;
}

static NativePixel ResolveNativeScreen(uint8_t mask, NativePixel bg1,
                                       NativePixel bg2) {
  NativePixel result = {0, 5, 0};
  if ((mask & 1) && bg1.rank > result.rank) result = bg1;
  if ((mask & 2) && bg2.rank > result.rank) result = bg2;
  return result;
}

static uint16_t PaletteColor(const ActionRoomScene *scene, uint8_t index) {
  const size_t offset = (size_t)index * 2u;
  return (uint16_t)(scene->palette[offset] |
                    ((uint16_t)scene->palette[offset + 1] << 8));
}

static uint8_t ExpandComponent(unsigned value, bool half,
                               unsigned brightness) {
  if (half) value >>= 1;
  if (value > 31) value = 31;
  const unsigned expanded = (value << 3) | (value >> 2);
  return (uint8_t)(expanded * brightness / 15u);
}

static uint32_t CompositeNativePixel(const ActionRoomScene *scene,
                                     const ActionRoomSceneFrameState *state,
                                     NativePixel main, NativePixel sub) {
  const unsigned clip_mode = state->cgwsel >> 6 & 3u;
  const bool visible = clip_mode == 0 || clip_mode == 2;
  uint16_t color = PaletteColor(scene, main.palette);
  unsigned red = visible ? color & 31u : 0;
  unsigned green = visible ? (color >> 5) & 31u : 0;
  unsigned blue = visible ? (color >> 10) & 31u : 0;
  const unsigned prevent_mode = state->cgwsel >> 4 & 3u;
  const bool math_window = prevent_mode == 0 || prevent_mode == 2;
  const bool math = math_window &&
      (state->cgadsub & (1u << main.layer)) != 0;
  bool half = false;
  if (math) {
    const bool use_subscreen = (state->cgwsel & 2) != 0;
    const bool have_subscreen = use_subscreen && sub.palette != 0;
    const uint16_t second = have_subscreen
        ? PaletteColor(scene, sub.palette) : state->fixed_color;
    const unsigned red2 = second & 31u;
    const unsigned green2 = (second >> 5) & 31u;
    const unsigned blue2 = (second >> 10) & 31u;
    if (state->cgadsub & 0x80) {
      red = red >= red2 ? red - red2 : 0;
      green = green >= green2 ? green - green2 : 0;
      blue = blue >= blue2 ? blue - blue2 : 0;
    } else {
      red += red2;
      green += green2;
      blue += blue2;
    }
    half = (state->cgadsub & 0x40) != 0 &&
        (!use_subscreen || have_subscreen);
  }
  const uint8_t r = ExpandComponent(red, half, state->brightness);
  const uint8_t g = ExpandComponent(green, half, state->brightness);
  const uint8_t b = ExpandComponent(blue, half, state->brightness);
  return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

bool ActionRoomScene_RenderNativeFrame(
    const ActionRoomScene *scene,
    const ActionRoomSceneFrameState *state,
    uint32_t *argb, size_t pixel_count) {
  if (!scene || !state || !argb ||
      pixel_count < kActionRoomSceneFramePixels ||
      !scene->have_palette || !scene->have_character_bank[0] ||
      !scene->have_character_bank[1] || (state->bgmode & 7u) != 1 ||
      (state->cgwsel & 1u) != 0)
    return false;
  uint8_t characters[kActionRoomSceneCharacterBytes];
  if (!ActionRoomScene_BuildCharacters(
          scene, state->game_frame, state->animation_phase,
          characters, sizeof(characters)))
    return false;
  for (unsigned y = 0; y < kActionRoomSceneFrameHeight; y++)
    for (unsigned x = 0; x < kActionRoomSceneFrameWidth; x++) {
      const NativePixel bg1 = SampleNativeLayer(
          scene, state, characters, 0, x, y);
      const NativePixel bg2 = SampleNativeLayer(
          scene, state, characters, 1, x, y);
      const NativePixel main = ResolveNativeScreen(
          state->screen_enabled[0], bg1, bg2);
      const NativePixel sub = ResolveNativeScreen(
          state->screen_enabled[1], bg1, bg2);
      argb[(size_t)y * kActionRoomSceneFrameWidth + x] =
          CompositeNativePixel(scene, state, main, sub);
    }
  return true;
}
