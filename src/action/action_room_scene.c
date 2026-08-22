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
    if (destination < kCharacterVramBytes) {
      if (bytes > kCharacterVramBytes - destination ||
          !Decompress(rom, rom_size, source,
                      scene->characters + destination, bytes))
        return false;
      scene->have_character_bank[destination / kCharacterBankBytes] = true;
    } else if (destination == kExtraCharacterUploadDestination) {
      if (!Decompress(rom, rom_size, source, scene->extra_characters, bytes))
        return false;
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

static ActionRoomRasterPreset RasterPresetForRoom(uint8_t group,
                                                   uint8_t map) {
  if (group == 0x01 && map == 0x04) return kActionRoomRaster_R1;
  if (group == 0x02 && map == 0x01) return kActionRoomRaster_R2;
  if (group == 0x02 && (map == 0x02 || map == 0x03))
    return kActionRoomRaster_R3;
  if (group == 0x04 && map == 0x05) return kActionRoomRaster_R4;
  if (group == 0x04 && map == 0x01) return kActionRoomRaster_R5;
  if (group == 0x06 && (map == 0x01 || map == 0x05))
    return kActionRoomRaster_R6;
  if (group == 0x06 && map == 0x08) return kActionRoomRaster_R7;
  if (group == 0x07 && map == 0x01) return kActionRoomRaster_R8;
  if (group == 0x07 && map >= 0x02 && map <= 0x07)
    return kActionRoomRaster_R9;
  if (group == 0x07 && map == 0x08) return kActionRoomRaster_R10;
  return kActionRoomRaster_None;
}

bool ActionRoomScene_Load(ActionRoomScene *scene,
                          const uint8_t *rom, size_t rom_size,
                          uint8_t group, uint8_t map) {
  if (!scene) return false;
  memset(scene, 0, sizeof(*scene));
  scene->group = group;
  scene->map = map;
  scene->raster_preset = RasterPresetForRoom(group, map);
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
