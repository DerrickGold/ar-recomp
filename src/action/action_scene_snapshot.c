#include "action_scene_snapshot.h"

#include <string.h>
#include "actraiser_game.h"
#include "deterministic_hash.h"

enum { kMagic = 0x43535241u, kWords = 28, kChecksumWord = 26 };

static uint32_t Read32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
      (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void Write32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static int32_t Signed32(uint32_t v) {
  return v <= INT32_MAX ? (int32_t)v : -1 - (int32_t)(UINT32_MAX - v);
}
static uint32_t Checksum(const uint8_t *bytes, size_t size) {
  uint32_t h = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  for (size_t i = 0; i < size; ++i)
    h = DeterministicHash_Fnv1a32Byte(h,
        i >= kChecksumWord * 4 && i < kChecksumWord * 4 + 4 ? 0 : bytes[i]);
  return h;
}
static size_t PayloadSize(const uint32_t h[kWords]) {
  return kActionSceneSnapshotHeaderBytes + kActionRoomSceneCharacterBytes +
      kActionRoomSceneExtraCharacterBytes + kActionRoomScenePaletteBytes +
      2 * kActionRoomSceneMetatileBytes + h[20] + h[24] +
      kActionRoomSceneVideoProfileBytes + kActionRoomSceneRasterWaveformBytes +
      kActionRoomSceneRasterMosaicWaveWindowBytes + kActionRoomSceneRasterWorkspaceBytes;
}
static bool Coordinate(uint32_t v) {
  const int32_t n = Signed32(v);
  return n >= -32768 && n <= 65535;
}
static bool HeaderValid(const uint32_t h[kWords]) {
  if (h[0] != kMagic || h[1] != kActionSceneSnapshotVersion || h[3] > 2 ||
      h[4] > UINT8_MAX || h[5] > UINT8_MAX ||
      !ActRaiser_IsActionMap((uint8_t)h[4], (uint8_t)h[5]) ||
      (h[6] & ~511u) || h[7] > UINT8_MAX || h[8] > kActionRoomRaster_DualBgOpposedWaves ||
      h[9] > UINT16_MAX || !Coordinate(h[10]) || !Coordinate(h[11]) ||
      !Coordinate(h[12]) || (h[14] != UINT32_MAX && h[14] > 255) ||
      (h[15] != UINT32_MAX && h[15] > 3) || (h[16] & ~15u) ||
      h[17] > UINT16_MAX || h[27] != 0) return false;
  for (unsigned bg = 0; bg < 2; ++bg) {
    const uint32_t *b = h + 18 + bg * 4;
    const unsigned max_pages = kActionRoomSceneMaxMapBytes / kActionRoomSceneMapPageBytes;
    if (b[0] > max_pages || b[1] > max_pages || b[2] > kActionRoomSceneMaxMapBytes ||
        (b[3] & ~3u)) return false;
    if (b[3] & 2u) {
      if (!b[0] || !b[1] || b[2] != b[0] * b[1] * 256) return false;
    } else if (b[0] || b[1] || b[2]) return false;
  }
  return h[2] == PayloadSize(h) && h[2] <= kActionSceneSnapshotMaxBytes;
}

bool ActionSceneSnapshot_Encode(const ActionSceneSnapshot *s,
                                uint8_t *bytes, size_t capacity, size_t *size) {
  if (size) *size = 0;
  if (!s || !bytes || !size) return false;
  const ActionRoomScene *r = &s->scene;
  const ActionRoomSceneFrameRequest *f = &s->frame;
  if (f->bgsc_override_mask > 3) return false;
  uint32_t h[kWords] = {kMagic, kActionSceneSnapshotVersion, 0, s->terrain_profile,
    r->group, r->map,
    r->have_character_bank[0] | r->have_character_bank[1] << 1 |
    r->have_extra_characters << 2 | r->have_palette << 3 |
    r->have_video_profile << 4 | r->have_raster_waveform << 5 |
    r->have_raster_mosaic_wave_window << 6 | r->have_raster_workspace << 7 |
    r->have_raster_entry_camera_x << 8,
    r->video_profile_index, r->raster_effect, r->raster_entry_camera_x,
    (uint32_t)f->camera_x, (uint32_t)f->camera_y, (uint32_t)f->raster_camera_x,
    f->game_frame, (uint32_t)f->animation_phase, (uint32_t)f->page_phase,
    f->have_raster_camera_x | (uint32_t)f->bgsc_override_mask << 1 |
    f->raster_entry_frame << 3,
    f->bgsc_override[0] | (uint32_t)f->bgsc_override[1] << 8};
  for (unsigned bg = 0; bg < 2; ++bg) {
    const ActionRoomSceneBg *b = &r->bg[bg];
    if (b->map_size > kActionRoomSceneMaxMapBytes) return false;
    h[18 + bg * 4] = b->pages_wide;
    h[19 + bg * 4] = b->pages_high;
    h[20 + bg * 4] = (uint32_t)b->map_size;
    h[21 + bg * 4] = b->have_metatiles | b->have_map << 1;
  }
  h[2] = (uint32_t)PayloadSize(h);
  if (!HeaderValid(h) || capacity < h[2]) return false;
  for (unsigned i = 0; i < kWords; ++i) Write32(bytes + i * 4, h[i]);
  uint8_t *p = bytes + kActionSceneSnapshotHeaderBytes;
#define PUT(field, count) do { memcpy(p, (field), (count)); p += (count); } while (0)
  PUT(r->characters, sizeof(r->characters));
  PUT(r->extra_characters, sizeof(r->extra_characters));
  PUT(r->palette, sizeof(r->palette));
  for (unsigned bg = 0; bg < 2; ++bg) {
    PUT(r->bg[bg].metatiles, sizeof(r->bg[bg].metatiles));
    PUT(r->bg[bg].map, r->bg[bg].map_size);
  }
  PUT(r->video_profile, sizeof(r->video_profile));
  PUT(r->raster_waveform, sizeof(r->raster_waveform));
  PUT(r->raster_mosaic_wave_window, sizeof(r->raster_mosaic_wave_window));
  PUT(r->raster_workspace, sizeof(r->raster_workspace));
#undef PUT
  Write32(bytes + kChecksumWord * 4, Checksum(bytes, h[2]));
  *size = h[2];
  return true;
}

bool ActionSceneSnapshot_Decode(const uint8_t *bytes, size_t size,
                                ActionSceneSnapshot *s) {
  if (!bytes || !s || size < kActionSceneSnapshotHeaderBytes ||
      size > kActionSceneSnapshotMaxBytes) return false;
  uint32_t h[kWords];
  for (unsigned i = 0; i < kWords; ++i) h[i] = Read32(bytes + i * 4);
  if (!HeaderValid(h) || size != h[2] || h[kChecksumWord] != Checksum(bytes, size))
    return false;
  /* Every bound and length has been checked before publishing any output. */
  memset(s, 0, sizeof(*s));
  ActionRoomScene *r = &s->scene;
  r->group = (uint8_t)h[4];
  r->map = (uint8_t)h[5];
  s->terrain_profile = h[3];
  bool *flags[] = {&r->have_character_bank[0], &r->have_character_bank[1],
    &r->have_extra_characters, &r->have_palette, &r->have_video_profile,
    &r->have_raster_waveform, &r->have_raster_mosaic_wave_window,
    &r->have_raster_workspace, &r->have_raster_entry_camera_x};
  for (unsigned i = 0; i < 9; ++i) *flags[i] = (h[6] & (1u << i)) != 0;
  r->video_profile_index = (uint8_t)h[7];
  r->raster_effect = (ActionRoomRasterEffect)h[8];
  r->raster_entry_camera_x = (uint16_t)h[9];
  s->frame = (ActionRoomSceneFrameRequest){
    .camera_x = Signed32(h[10]), .camera_y = Signed32(h[11]),
    .raster_camera_x = Signed32(h[12]), .game_frame = h[13],
    .animation_phase = Signed32(h[14]), .page_phase = Signed32(h[15]),
    .have_raster_camera_x = (h[16] & 1) != 0,
    .bgsc_override_mask = (uint8_t)((h[16] >> 1) & 3),
    .raster_entry_frame = (h[16] & 8) != 0,
    .bgsc_override = {(uint8_t)h[17], (uint8_t)(h[17] >> 8)},
  };
  const uint8_t *p = bytes + kActionSceneSnapshotHeaderBytes;
#define GET(field, count) do { memcpy((field), p, (count)); p += (count); } while (0)
  GET(r->characters, sizeof(r->characters));
  GET(r->extra_characters, sizeof(r->extra_characters));
  GET(r->palette, sizeof(r->palette));
  for (unsigned bg = 0; bg < 2; ++bg) {
    ActionRoomSceneBg *b = &r->bg[bg];
    b->pages_wide = (uint8_t)h[18 + bg * 4];
    b->pages_high = (uint8_t)h[19 + bg * 4];
    b->map_size = h[20 + bg * 4];
    b->have_metatiles = (h[21 + bg * 4] & 1) != 0;
    b->have_map = (h[21 + bg * 4] & 2) != 0;
    GET(b->metatiles, sizeof(b->metatiles));
    GET(b->map, b->map_size);
  }
  GET(r->video_profile, sizeof(r->video_profile));
  GET(r->raster_waveform, sizeof(r->raster_waveform));
  GET(r->raster_mosaic_wave_window, sizeof(r->raster_mosaic_wave_window));
  GET(r->raster_workspace, sizeof(r->raster_workspace));
#undef GET
  return true;
}
