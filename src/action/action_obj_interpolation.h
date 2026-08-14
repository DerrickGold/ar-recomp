#ifndef ACTION_OBJ_INTERPOLATION_H
#define ACTION_OBJ_INTERPOLATION_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Ppu Ppu;

enum {
  kActionObjInterpolationMaxParts = 256,
  kActionObjInterpolationAtlasWidth = 1024,
  kActionObjInterpolationAtlasHeight = 1024,
  kActionObjInterpolationSyntheticSlot = 0xff,
};

/* One current-frame sprite component. Motion is keyed by the action object
 * record and its common screen anchor, not by the OAM slot: ActRaiser's
 * sequential OAM allocator can move every later component when an earlier
 * actor changes composition. Components without an action identity remain
 * drawable but deliberately snap to their current position. */
typedef struct ActionObjInterpolationPart {
  int16_t x, y;
  int16_t anchor_x, anchor_y;
  uint16_t object_address;
  uint16_t object_signature;
  uint16_t tile_attr;
  uint16_t atlas_x, atlas_y;
  uint8_t size;
  uint8_t priority;
  uint8_t oam_slot;
} ActionObjInterpolationPart;

typedef struct ActionObjInterpolationFrame {
  bool valid;
  uint64_t timestamp_ns;
  uint16_t part_count;
  uint16_t atlas_used_width, atlas_used_height;
  ActionObjInterpolationPart parts[kActionObjInterpolationMaxParts];
} ActionObjInterpolationFrame;

extern uint32_t g_action_obj_interpolation_atlas_pixels[
    kActionObjInterpolationAtlasWidth *
    kActionObjInterpolationAtlasHeight];

/* Game-thread emitter channel. Begin is called once before the action object
 * scan. Accepted OAM components and apron-only synthetic components publish
 * the same object anchor, so the renderer can move a multipart actor as one
 * unit even when its visual composition changes between ticks. */
void ActionObjInterpolation_BeginFrame(bool enabled);
void ActionObjInterpolation_RecordOamPart(
    uint8_t slot, uint16_t object_address, uint16_t object_signature,
    int screen_anchor_x, int screen_anchor_y);
void ActionObjInterpolation_RecordSyntheticPart(
    int x, int y, uint16_t tile_attr, uint8_t size,
    uint16_t object_address, uint16_t object_signature,
    int screen_anchor_x, int screen_anchor_y);

/* Resolve current OAM plus the synthetic apron channel into a packed atlas.
 * The four captured OBJ priority planes are used as an exact visibility mask:
 * every atlas pixel must correspond to a pixel scanout actually retained.
 * Any unmatched plane pixel fails the frame closed to the existing raster
 * planes rather than presenting an incomplete reconstruction. */
bool ActionObjInterpolation_BuildFrame(
    Ppu *ppu, ActionObjInterpolationFrame *out,
    const uint8_t *priority_pixels[4], uint8_t priority_content_mask,
    int surface_width, int surface_height,
    int screen_x_origin, int screen_y_origin,
    uint8_t excluded_oam_first, uint8_t excluded_oam_count,
    uint64_t timestamp_ns);

/* Resolve one current component's delayed position from its object's prior
 * common anchor. Current artwork and component-local offsets are retained;
 * only the object's screen-space displacement is interpolated. A missing or
 * ambiguous identity and an implausibly large displacement snap to current. */
void ActionObjInterpolation_PartPosition(
    const ActionObjInterpolationFrame *previous,
    const ActionObjInterpolationPart *current,
    float pair_phase, int maximum_delta,
    float *out_x, float *out_y);

#endif  /* ACTION_OBJ_INTERPOLATION_H */
