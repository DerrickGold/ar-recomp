#include "dev/action_object_snapshot_capture.h"

#include "actraiser/actraiser_action_bg.h"
#include "actraiser_game.h"
#include "app/session_fatal.h"
#include "diorama/diorama_object_snapshot.h"
#include "diorama/diorama_planes.h"
#include "present/present.h"
#include "present/presentation_surface.h"
#include "snesrecomp/support/utf8_fs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
  bool initialized, failed, have_last;
  const char *directory;
  unsigned from, to, every, serial, last_frame, last_room;
  uint8_t *packet;
} s_capture;

static bool Option(const char *name, unsigned fallback, unsigned *out) {
  const char *value = getenv(name);
  *out = fallback;
  if (!value || !*value) return true;
  errno = 0;
  char *end;
  const unsigned long number = strtoul(value, &end, 10);
  if (errno || end == value || *end || number > 65535u) return false;
  *out = (unsigned)number;
  return true;
}

static void Fail(const char *reason) {
  s_capture.failed = true;
  free(s_capture.packet);
  s_capture.packet = NULL;
  SessionFatal_Request("Editor native object capture failed: %s", reason);
}

static bool Initialize(void) {
  if (!s_capture.initialized) {
    s_capture.initialized = true;
    s_capture.directory = getenv("AR_EDITOR_OBJECT_CAPTURE_DIR");
    if (!s_capture.directory || !s_capture.directory[0]) return false;
    if (!Option("AR_EDITOR_OBJECT_CAPTURE_FROM", 0, &s_capture.from) ||
        !Option("AR_EDITOR_OBJECT_CAPTURE_TO", 65534, &s_capture.to) ||
        !Option("AR_EDITOR_OBJECT_CAPTURE_EVERY", 1, &s_capture.every) ||
        !s_capture.every || s_capture.from > s_capture.to) {
      Fail("invalid FROM/TO/EVERY native-frame range");
    } else if (!sr_path_is_directory(s_capture.directory)) {
      Fail("AR_EDITOR_OBJECT_CAPTURE_DIR must be an existing directory");
    } else {
      char index[4096];
      const int length = snprintf(index, sizeof(index), "%s/index.jsonl", s_capture.directory);
      if (length < 0 || (size_t)length >= sizeof(index))
        Fail("native object output path is too long");
      else if (sr_path_exists(index))
        Fail("output directory already contains an object capture index; use a fresh directory");
    }
  }
  return s_capture.directory && s_capture.directory[0] && !s_capture.failed;
}

bool ActionObjectSnapshotCapture_Enabled(void) { return Initialize(); }

void ActionObjectSnapshotCapture_Capture(const FrameSlot *slot) {
  if (!Initialize() || !slot || !slot->diorama_active) return;
  const unsigned frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
  if (frame == kActRaiserPowerOnGameFrame || frame < s_capture.from ||
      frame > s_capture.to || (frame - s_capture.from) % s_capture.every ||
      !ActRaiser_IsActionMapGroup(slot->diorama_map_group)) return;
  const unsigned room = (unsigned)slot->diorama_map_group * 256u + slot->diorama_map_number;
  if (s_capture.have_last && s_capture.last_frame == frame && s_capture.last_room == room) return;

  const int width = slot->snes_width + slot->obj_apron * 2;
  const int height = slot->snes_height + slot->ws_extra_top + slot->ws_extra_bottom;
  if (width <= 0 || width > kDioramaObjectSnapshotWidth || height <= 0 ||
      height > kDioramaObjectSnapshotHeight || slot->ws_extra < 0 ||
      slot->ws_extra_top < 0 || slot->obj_apron < 0) {
    Fail("invalid native capture geometry");
    return;
  }
  DioramaObjectSnapshotView view = {
    .map_group = slot->diorama_map_group, .map_number = slot->diorama_map_number,
    .terrain_profile = ActRaiserActionBg_TerrainProfile(),
    .layer_section = slot->diorama_layer_section, .game_frame = frame,
    .camera_x = slot->bg1_camera_x, .camera_y = slot->bg1_camera_y,
    .world_x = slot->bg1_camera_x - slot->ws_extra - slot->obj_apron,
    .world_y = slot->bg1_camera_y - slot->ws_extra_top,
    .extra_x = slot->ws_extra, .extra_top = slot->ws_extra_top,
    .obj_apron = slot->obj_apron, .width = width, .height = height,
    .owner_tags = slot->diorama_obj_owner_tags, .voxel_sprites = slot->voxel_sprites,
    .actor_count = slot->action_scene_effects.actor_count,
  };
  if (view.actor_count > kActionEffectActorMax) {
    Fail("native actor count exceeds the capture contract");
    return;
  }
  memcpy(view.actors, slot->action_scene_effects.actors,
      view.actor_count * sizeof(view.actors[0]));
  for (unsigned p = 0; p < 4; ++p) {
    const unsigned plane = DioramaPlaneForObjectPriority(p);
    if (slot->diorama_plane_additive_mask & (UINT32_C(1) << plane))
      view.additive_priority_mask |= (uint8_t)(1u << p);
    if (!(slot->diorama_plane_content_mask & (UINT32_C(1) << plane))) continue;
    const SrPpuSurfaceView *obj = &slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][p];
    if (!PresentationSurface_Holds(obj, width, height)) {
      Fail("a nonempty native OBJ band has no bounded source pixels");
      return;
    }
    view.obj[p] = (const uint32_t *)obj->data;
    view.obj_pitch[p] = (size_t)obj->pitch_bytes / sizeof(uint32_t);
    const SrPpuSurfaceView *attributes = &slot->diorama_obj_attributes[p];
    if (attributes->data) {
      if (!PresentationSurface_Holds(attributes, width, height)) {
        Fail("native OBJ attributes do not match their pixel band");
        return;
      }
      view.attributes[p] = (const uint32_t *)attributes->data;
      view.attribute_pitch[p] = (size_t)attributes->pitch_bytes / sizeof(uint32_t);
    }
  }
  if (view.voxel_sprites.count) {
    const SrPpuSurfaceView *atlas = &slot->voxel_sprite_atlas;
    if (!PresentationSurface_Holds(atlas, kVoxelSpriteAtlasWidth,
        ((int)view.voxel_sprites.count + 3) / 4 * kVoxelSpriteCellHeight)) {
      Fail("modeled native props have no matching bounded atlas");
      return;
    }
    view.atlas = (const uint32_t *)atlas->data;
    view.atlas_pitch = (size_t)atlas->pitch_bytes / sizeof(uint32_t);
    view.atlas_height = atlas->height_pixels;
  }
  if (!s_capture.packet) s_capture.packet = malloc(kDioramaObjectSnapshotCapacity);
  size_t size = 0;
  if (!s_capture.packet || !DioramaObjectSnapshot_Encode(&view, s_capture.packet,
      kDioramaObjectSnapshotCapacity, &size)) {
    Fail("native object packet could not be encoded");
    return;
  }
  char name[80], path[4096];
  snprintf(name, sizeof(name), "%02x%02x-gf%05u-%06u.arobj",
      view.map_group, view.map_number, frame, s_capture.serial);
  const int length = snprintf(path, sizeof(path), "%s/%s", s_capture.directory, name);
  if (length < 0 || (size_t)length >= sizeof(path)) {
    Fail("native object output path is too long");
    return;
  }
  if (sr_path_exists(path)) {
    Fail("native object packet already exists; use a fresh output directory");
    return;
  }
  FILE *file = sr_fopen(path, "wb");
  bool ok = file && fwrite(s_capture.packet, 1, size, file) == size;
  if (file && fclose(file)) ok = false;
  if (!ok) {
    sr_remove(path);
    Fail("native object packet could not be written");
    return;
  }
  const int index_length = snprintf(path, sizeof(path), "%s/index.jsonl", s_capture.directory);
  if (index_length < 0 || (size_t)index_length >= sizeof(path)) {
    Fail("native object index path is too long");
    return;
  }
  file = sr_fopen(path, "a");
  ok = file && fprintf(file,
      "{\"file\":\"%s\",\"room\":\"%02x%02x\",\"terrain_profile\":%u,"
      "\"layer_section\":%u,\"gf\":%u,\"camera\":[%d,%d],\"world_origin\":[%d,%d],"
      "\"size\":[%u,%u],\"actors\":%u,\"voxel_components\":%u,\"bytes\":%zu}\n",
      name, view.map_group, view.map_number, view.terrain_profile, view.layer_section,
      frame, view.camera_x, view.camera_y, view.world_x, view.world_y, view.width,
      view.height, view.actor_count, view.voxel_sprites.count, size) > 0;
  if (file && fclose(file)) ok = false;
  if (!ok) {
    Fail("native object index could not be written");
    return;
  }
  s_capture.have_last = true;
  s_capture.last_frame = frame;
  s_capture.last_room = room;
  ++s_capture.serial;
  fprintf(stderr, "[editor-object-capture] room=%04x gf=%u camera=%d,%d actors=%u props=%u bytes=%zu\n",
      room, frame, view.camera_x, view.camera_y, view.actor_count, view.voxel_sprites.count, size);
}
