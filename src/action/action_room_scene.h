#ifndef ACTION_ROOM_SCENE_H
#define ACTION_ROOM_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Immutable, ROM-derived action-room presentation state. This is the shared
 * arbitrary-room authority used by tooling and game-side validation; it does
 * not execute gameplay or mutate emulated memory. */
enum {
  kActionRoomSceneBgCount = 2,
  kActionRoomSceneCharacterBytes = 0x4000,
  kActionRoomSceneExtraCharacterBytes = 0x2000,
  kActionRoomScenePaletteBytes = 0x0100,
  kActionRoomSceneMetatileBytes = 0x0800,
  kActionRoomSceneMapPageBytes = 0x0100,
  kActionRoomSceneMaxMapBytes = 16 * 4 * kActionRoomSceneMapPageBytes,
  kActionRoomSceneVideoProfileBytes = 28,
  kActionRoomSceneAnimationWindowBytes = 0x1000,
  kActionRoomSceneTileWordMask = 0xECFF,
  kActionRoomSceneBg1AttributeByte = 0x10,
  kActionRoomSceneBg2AttributeByte = 0x01,
};

typedef enum ActionRoomRasterPreset {
  kActionRoomRaster_None = 0,
  kActionRoomRaster_R1,
  kActionRoomRaster_R2,
  kActionRoomRaster_R3,
  kActionRoomRaster_R4,
  kActionRoomRaster_R5,
  kActionRoomRaster_R6,
  kActionRoomRaster_R7,
  kActionRoomRaster_R8,
  kActionRoomRaster_R9,
  kActionRoomRaster_R10,
} ActionRoomRasterPreset;

typedef struct ActionRoomSceneBg {
  uint8_t metatiles[kActionRoomSceneMetatileBytes];
  uint8_t map[kActionRoomSceneMaxMapBytes];
  size_t map_size;
  uint8_t pages_wide;
  uint8_t pages_high;
  bool have_metatiles;
  bool have_map;
} ActionRoomSceneBg;

typedef struct ActionRoomScene {
  uint8_t group;
  uint8_t map;
  uint8_t characters[kActionRoomSceneCharacterBytes];
  uint8_t extra_characters[kActionRoomSceneExtraCharacterBytes];
  uint8_t palette[kActionRoomScenePaletteBytes];
  ActionRoomSceneBg bg[kActionRoomSceneBgCount];
  uint8_t video_profile[kActionRoomSceneVideoProfileBytes];
  uint8_t video_profile_index;
  ActionRoomRasterPreset raster_preset;
  bool have_character_bank[2];
  bool have_extra_characters;
  bool have_palette;
  bool have_video_profile;
} ActionRoomScene;

/* Replays the cumulative action asset script through the selected room and
 * resolves its command-3 video profile. A legacy/synthetic graphics script
 * without command 3 may still load, but have_video_profile remains false. */
bool ActionRoomScene_Load(ActionRoomScene *scene,
                          const uint8_t *rom, size_t rom_size,
                          uint8_t group, uint8_t map);

bool ActionRoomScene_HasBackground(const ActionRoomScene *scene,
                                   uint8_t bg_layer);
unsigned ActionRoomScene_TileWidth(const ActionRoomScene *scene,
                                   uint8_t bg_layer);
unsigned ActionRoomScene_TileHeight(const ActionRoomScene *scene,
                                    uint8_t bg_layer);
size_t ActionRoomScene_TileCount(const ActionRoomScene *scene,
                                 uint8_t bg_layer);

/* Profile common priority is merged with the permanent action character-bank
 * attribute. The returned value is already in the 16-bit tile-word domain. */
uint16_t ActionRoomScene_BgAttributes(const ActionRoomScene *scene,
                                      uint8_t bg_layer);

/* Exact finite-world tile lookup/expansion. Coordinates are 8x8 tile cells.
 * The optional metatile result names the containing 16x16 map cell. */
bool ActionRoomScene_LookupTile(const ActionRoomScene *scene,
                                uint8_t bg_layer,
                                unsigned tile_x, unsigned tile_y,
                                uint16_t *entry, uint8_t *metatile);
bool ActionRoomScene_ExpandBg(const ActionRoomScene *scene,
                              uint8_t bg_layer,
                              uint16_t *entries, size_t entry_count);

/* Character animation is reconstructed from the room's own 4 KiB snapshot
 * window. explicit_phase >= 0 is editor-controlled; -1 derives a deterministic
 * phase from game_frame with an origin of zero. */
bool ActionRoomScene_HasCharacterAnimation(const ActionRoomScene *scene);
unsigned ActionRoomScene_CharacterAnimationPhaseCount(
    const ActionRoomScene *scene);
unsigned ActionRoomScene_CharacterAnimationStride(
    const ActionRoomScene *scene);
unsigned ActionRoomScene_CharacterAnimationCadence(
    const ActionRoomScene *scene);
uint16_t ActionRoomScene_CharacterAnimationTarget(
    const ActionRoomScene *scene);
bool ActionRoomScene_CharacterAnimationContinues(
    const ActionRoomScene *scene);
unsigned ActionRoomScene_ResolveCharacterAnimationPhase(
    const ActionRoomScene *scene, uint32_t game_frame, int explicit_phase);
bool ActionRoomScene_BuildCharacters(const ActionRoomScene *scene,
                                     uint32_t game_frame,
                                     int explicit_phase,
                                     uint8_t *characters,
                                     size_t character_bytes);

/* Only Aitos 0402/0403 use the four-page BG2 cycle. Phase zero follows the
 * native $04,$08,$0C,$00 sequence; editor callers should set it explicitly. */
bool ActionRoomScene_HasBg2PageCycle(const ActionRoomScene *scene);
unsigned ActionRoomScene_ResolveBg2PagePhase(
    const ActionRoomScene *scene, uint32_t game_frame, int explicit_phase);
unsigned ActionRoomScene_Bg2PageIndex(const ActionRoomScene *scene,
                                      uint32_t game_frame,
                                      int explicit_phase);

#endif  /* ACTION_ROOM_SCENE_H */
