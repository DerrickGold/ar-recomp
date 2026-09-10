#include "sim_background_mountain_mesh.h"

#include "sim_background_mountain_silhouette.h"
#include "sim_world_map.h"

static const uint8_t kMountainFullIntensity[4] = {255, 255, 255, 255};

void SimBackgroundMountainMesh_PlanePoint(
    float source_x, float source_y, float baseline,
    const SimBackgroundMountainRelief *relief,
    float offset_x, float offset_y,
    float *local_x, float *local_y, float *local_z) {
  float rise = baseline - source_y;
  *local_x = source_x + offset_x;
  *local_y = baseline - rise * relief->face_depth_scale + offset_y;
  *local_z = rise * relief->face_height_scale;
}

void SimBackgroundMountainMesh_StackTile(
    const SimBackgroundMountainMeshContext *context,
    float baseline_left, float baseline_right,
    float maximum_rise_left, float maximum_rise_right,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, uint8_t flags) {
  if (!context || !context->relief || !context->relief->stack_layer_count ||
      !context->emit || context->atlas_pixels <= 0) return;
  bool mirror_x =
      (flags & kSimBackgroundMountainCapTile_MirrorX) != 0;
  float x0 = destination_cell_x * kSimTownCellPixels;
  float y0 = destination_cell_y * kSimTownCellPixels;
  float x1 = x0 + kSimTownCellPixels;
  float y1 = y0 + kSimTownCellPixels;
  float u0 = source_cell_x * kSimTownCellPixels /
      (float)context->atlas_pixels;
  float v0 = source_cell_y * kSimTownCellPixels /
      (float)context->atlas_pixels;
  float u1 = (source_cell_x + 1) * kSimTownCellPixels /
      (float)context->atlas_pixels;
  float v1 = (source_cell_y + 1) * kSimTownCellPixels /
      (float)context->atlas_pixels;
  float source_x[4] = {x0, x1, x1, x0};
  float source_y[4] = {y0, y0, y1, y1};
  /* Every layer retains the exact front silhouette orientation. Flipping only
   * the rear copy moves off-centre tip pixels across their tile and turns one
   * peak into two visible horns, so the mapping does not vary by layer. */
  const SimBackgroundMountainMeshUV uv[4] = {
    {mirror_x ? u1 : u0, v0},
    {mirror_x ? u0 : u1, v0},
    {mirror_x ? u0 : u1, v1},
    {mirror_x ? u1 : u0, v1},
  };
  /* The displacement of the rearmost copy. Every nearer layer is a fixed
   * fraction of it, so the taper is resolved once per corner instead of once
   * per corner per layer. */
  const SimBackgroundMountainRelief *relief = context->relief;
  float corner_baseline[4], rearmost_depth[4];
  for (int point = 0; point < 4; point++) {
    bool left = point == 0 || point == 3;
    corner_baseline[point] = left ? baseline_left : baseline_right;
    float rise = corner_baseline[point] - source_y[point];
    /* The relief module states the displacement as a signed northward offset,
     * which is its magnitude along whichever way the camera says is back.
     * Negating recovers that magnitude; the direction supplies the sign, so a
     * zero-yaw camera lands on exactly the old northward value. */
    rearmost_depth[point] = -SimBackgroundMountainRelief_StackOffsetY(
        relief, (uint8_t)(relief->stack_layer_count - 1), rise,
        left ? maximum_rise_left : maximum_rise_right);
  }
  float layers = (float)(relief->stack_layer_count - 1);
  /* Emit rear copies first for coherent material batching. Visibility is
   * resolved per fragment by the shared D32 target, so this loop order is not
   * a correctness dependency. */
  for (int layer = relief->stack_layer_count - 1; layer >= 0; layer--) {
    float fraction = layers > 0.0f ? (float)layer / layers : 0.0f;
    float local_x[4], local_y[4], local_z[4];
    for (int point = 0; point < 4; point++) {
      float depth = rearmost_depth[point] * fraction;
      SimBackgroundMountainMesh_PlanePoint(
          source_x[point], source_y[point], corner_baseline[point], relief,
          depth * context->stack_direction.x,
          depth * context->stack_direction.y,
          &local_x[point], &local_y[point], &local_z[point]);
      local_z[point] *= context->height_scale;
    }
    context->emit(context->user, local_x, local_y, local_z, uv,
                  kMountainFullIntensity, kMountainFullIntensity);
  }
}

/* Each relief layer is a flat inclined plane with the authentic art painted on
 * it: its cross-section is a straight line from the ground at the front up and
 * back to the ridge. That reads as a mountain only from the canonical camera.
 * The wedge between the plane and the ground is empty and open at the sides,
 * so any other angle shows a tilted board whose upper half hangs in the air -
 * and no amount of extra stack layers changes that, because the layers are
 * parallel copies of the same plane and thicken it along the ground rather
 * than filling underneath it.
 *
 * This closes the silhouette: wherever a mountain cell has no neighbour, a
 * quad drops from that cell's edge of the plane straight down to z=0, giving
 * the mass a visible side and a footprint. It follows the outline in both
 * axes, stretches the tile's own art down the wall, and ramps its shading
 * from the face's own value to a shaded base. */
enum {
  /* How far into the tile the wall's ground edge samples. The mountain tiles
   * carry a one-to-two pixel dither margin along their outline that is both
   * grass-coloured and partly transparent, so a wall sampled from the outline
   * alone shows green see-through streaks. Stretching the tile's own interior
   * across the quad is what makes it read as rock. */
  kMountainSkirtStretchPixels = 8,
  /* A wall needs more than a couple of rows of outline to be worth standing. */
  kMountainSkirtMinimumRows = 2,
};
/* Shading down the wall. The mountain face is drawn fully lit, so a wall at a
 * single darker value meets it in a hard tonal step, and any pixel of
 * misalignment at that junction then reads as a shelf rather than as an edge.
 * Ramping from almost the face's own value at the top to a shaded base makes
 * the join continuous and lets the wall still read as a side. */
static const uint8_t kMountainSkirtTopBrightness = 240;
static const uint8_t kMountainSkirtBaseBrightness = 150;
/* The wall stands a fraction of a pixel proud of the art and starts a hair
 * above the face it meets. Both are below one source pixel, and they close
 * the sub-pixel seam the fitted line cannot follow exactly - the outline is
 * jagged row to row and a single quad can only be straight. */
static const float kMountainSkirtOutwardMargin = 0.5f;
static const float kMountainSkirtOverlapPixels = 0.35f;
/* How far a row's outline may sit inside the fitted line before that row is
 * treated as dither fringe rather than part of the wall. */
static const float kMountainSkirtStraightTolerance = 1.5f;
/* Where a tile's art meets the open side of its cell. Derived from the
 * immutable silhouette table, so it is resolved once per tile/side and reused
 * for every frame, object and town. */
typedef struct MountainSkirtProfile {
  bool resolved;
  bool present;
  uint8_t first_row, last_row;
  /* Where the wall stands, fitted to the tile's whole outline and then pushed
   * outward until no row of art lies outside it. Taking the two end rows
   * alone slants the line inward at a base tile, whose last row is the
   * dithered fringe several pixels in, and the mountain then overhangs its
   * own wall by three or four pixels. */
  float wall_first, wall_last;
  /* Columns the quad samples: the outline for the top edge so it joins the
   * mountain without a seam, the interior for the ground edge. */
  uint8_t outer_first, outer_last;
  uint8_t inner_first, inner_last;
} MountainSkirtProfile;

/* Resolved from the compile-time silhouette table, so an entry is the same
 * value whenever it is computed and never needs invalidating - not on a town
 * change, not on Reset. It is filled lazily from the present thread, which is
 * the only thread that renders; a second renderer thread would need this
 * built up front instead. */
static MountainSkirtProfile g_skirt_profiles[256][2];

static bool MountainSilhouetteOpaque(uint8_t tile, int column, int row) {
  bool opaque = false;
  if (!SimBackgroundMountainSilhouette_Lookup(tile, column, row, &opaque))
    return true;  /* Unknown tiles are treated as solid elsewhere too. */
  return opaque;
}

/* Outermost and innermost opaque columns of one row, scanning from `edge`
 * toward the far side of the tile. */
static bool MountainRowRun(uint8_t tile, int row, int edge, int step,
                           int *outer, int *inner) {
  int first = -1, last = -1;
  for (int at = 0; at < kSimTownCellPixels; at++) {
    int column = edge + at * step;
    if (!MountainSilhouetteOpaque(tile, column, row)) continue;
    if (first < 0) first = column;
    last = column;
  }
  if (first < 0) return false;
  int reach = first + step * kMountainSkirtStretchPixels;
  /* Never sample past the run: beyond it is the tile's other outline. */
  if ((step > 0 && reach > last) || (step < 0 && reach < last)) reach = last;
  /* The fringe rows where a mountain's foot meets grass are dithered, not
   * solid runs, so the column that far in can still be a hole. Back off to
   * the nearest opaque one rather than punching the wall through. */
  while (reach != first && !MountainSilhouetteOpaque(tile, reach, row))
    reach -= step;
  *outer = first;
  *inner = reach;
  return true;
}

/* Least-squares fit of the outline over a row span, as column = intercept +
 * slope * (row - first_row). */
static void MountainFitOutline(uint8_t tile, int edge, int step,
                               int first_row, int last_row,
                               float *slope, float *intercept) {
  float rows = 0.0f, sum_row = 0.0f, sum_column = 0.0f;
  float sum_row_column = 0.0f, sum_row_row = 0.0f;
  for (int row = first_row; row <= last_row; row++) {
    int outer, inner;
    if (!MountainRowRun(tile, row, edge, step, &outer, &inner)) continue;
    float offset = (float)(row - first_row);
    rows += 1.0f;
    sum_row += offset;
    sum_column += (float)outer;
    sum_row_column += offset * (float)outer;
    sum_row_row += offset * offset;
  }
  if (rows <= 0.0f) {
    *slope = 0.0f;
    *intercept = 0.0f;
    return;
  }
  float denominator = rows * sum_row_row - sum_row * sum_row;
  *slope = denominator > 0.0001f
      ? (rows * sum_row_column - sum_row * sum_column) / denominator : 0.0f;
  *intercept = (sum_column - *slope * sum_row) / rows;
}

static const MountainSkirtProfile *MountainSkirtProfileFor(uint8_t tile,
                                                           bool right_edge) {
  MountainSkirtProfile *profile = &g_skirt_profiles[tile][right_edge ? 1 : 0];
  if (profile->resolved) return profile;
  profile->resolved = true;
  int step = right_edge ? -1 : 1;
  int edge = right_edge ? kSimTownCellPixels - 1 : 0;

  int first_row = -1, last_row = -1;
  for (int row = 0; row < kSimTownCellPixels; row++) {
    int outer, inner;
    if (!MountainRowRun(tile, row, edge, step, &outer, &inner)) continue;
    if (first_row < 0) first_row = row;
    last_row = row;
  }
  if (first_row < 0 || last_row - first_row < kMountainSkirtMinimumRows)
    return profile;

  /* A mountain's foot dissolves into the lawn over two or three dithered
   * rows whose outline sits several pixels inside the rest of the wall.
   * Following them would either drag the wall into the mountain, leaving the
   * art overhanging it, or leave the wall protruding past the art once it is
   * biased back out. Ending the wall where the outline stops being straight
   * avoids both, and costs nothing: those rows are within a pixel of the
   * ground already. A genuine diagonal deviates from its own fit by nothing,
   * so it is never trimmed. */
  float slope, intercept;
  while (last_row - first_row > kMountainSkirtMinimumRows) {
    MountainFitOutline(tile, edge, step, first_row, last_row,
                       &slope, &intercept);
    int outer, inner;
    if (!MountainRowRun(tile, last_row, edge, step, &outer, &inner)) break;
    float line = intercept + slope * (float)(last_row - first_row);
    float deviation = (float)step * ((float)outer - line);
    if (deviation <= kMountainSkirtStraightTolerance) break;
    last_row--;
  }
  MountainFitOutline(tile, edge, step, first_row, last_row,
                     &slope, &intercept);

  /* Push the fitted line outward until no row of art lies outside it. */
  float bias = 0.0f;
  for (int row = first_row; row <= last_row; row++) {
    int outer, inner;
    if (!MountainRowRun(tile, row, edge, step, &outer, &inner)) continue;
    float line = intercept + slope * (float)(row - first_row);
    float outside = (float)step * (line - (float)outer);
    if (outside > bias) bias = outside;
  }

  int outer_first, inner_first, outer_last, inner_last;
  if (!MountainRowRun(tile, first_row, edge, step,
                      &outer_first, &inner_first) ||
      !MountainRowRun(tile, last_row, edge, step, &outer_last, &inner_last))
    return profile;

  float span = (float)(last_row - first_row);
  profile->present = true;
  profile->first_row = (uint8_t)first_row;
  profile->last_row = (uint8_t)last_row;
  profile->wall_first = intercept - (float)step * bias;
  profile->wall_last = intercept + slope * span - (float)step * bias;
  profile->outer_first = (uint8_t)outer_first;
  profile->outer_last = (uint8_t)outer_last;
  profile->inner_first = (uint8_t)inner_first;
  profile->inner_last = (uint8_t)inner_last;
  return profile;
}

/* Displacement of the outermost stack copy on one side of the mountain. The
 * relief stack recedes along the camera's away axis, which under yaw has a
 * sideways component - up to 2.8px at Ultra - so a wall standing at the front
 * copy is overhung by the rear ones. Zero when the stack fans the other way,
 * because then the front copy already is the outermost. */
static void MountainStackOutwardOffset(
    const SimBackgroundMountainRelief *relief,
    SimBackgroundMountainMeshDirection direction, float rise, float maximum_rise,
    bool right_edge, float *offset_x, float *offset_y) {
  *offset_x = 0.0f;
  *offset_y = 0.0f;
  if (relief->stack_layer_count < 2) return;
  float magnitude = -SimBackgroundMountainRelief_StackOffsetY(
      relief, (uint8_t)(relief->stack_layer_count - 1), rise, maximum_rise);
  float sideways = magnitude * direction.x;
  if ((right_edge ? sideways : -sideways) <= 0.0f) return;
  *offset_x = sideways;
  *offset_y = magnitude * direction.y;
}

void SimBackgroundMountainMesh_SkirtTile(
    const SimBackgroundMountainMeshContext *context,
    float baseline, float maximum_rise,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, uint8_t source_tile,
    bool right_edge) {
  if (!context || !context->relief || !context->relief->stack_layer_count ||
      !context->emit || context->atlas_pixels <= 0) return;
  const MountainSkirtProfile *profile =
      MountainSkirtProfileFor(source_tile, right_edge);
  if (!profile->present) return;

  float outward = right_edge ? kMountainSkirtOutwardMargin
                             : -kMountainSkirtOutwardMargin;
  float cell_x = destination_cell_x * (float)kSimTownCellPixels +
      outward;
  float cell_top = destination_cell_y * (float)kSimTownCellPixels;
  /* The wall's top edge is the outline itself, so it slants with a shoulder
   * tile's diagonal instead of standing at the cell boundary beside it. */
  float north_x = cell_x + profile->wall_first;
  float south_x = cell_x + profile->wall_last;
  float y0 = cell_top + (float)profile->first_row;
  float y1 = cell_top + (float)profile->last_row + 1.0f;
  float north_offset_x, north_offset_y, south_offset_x, south_offset_y;
  MountainStackOutwardOffset(context->relief, context->stack_direction,
                             baseline - y0, maximum_rise, right_edge,
                             &north_offset_x, &north_offset_y);
  MountainStackOutwardOffset(context->relief, context->stack_direction,
                             baseline - y1, maximum_rise, right_edge,
                             &south_offset_x, &south_offset_y);
  float north_wall_x, north_y, north_z, south_wall_x, south_y, south_z;
  SimBackgroundMountainMesh_PlanePoint(north_x, y0, baseline, context->relief,
                     north_offset_x, north_offset_y,
                     &north_wall_x, &north_y, &north_z);
  SimBackgroundMountainMesh_PlanePoint(south_x, y1, baseline, context->relief,
                     south_offset_x, south_offset_y,
                     &south_wall_x, &south_y, &south_z);
  const float overlap = kMountainSkirtOverlapPixels * context->height_scale;
  north_z = north_z * context->height_scale + overlap;
  south_z = south_z * context->height_scale + overlap;
  /* Art already sitting on the ground has no wedge to close. */
  if (north_z <= overlap) return;

  float local_x[4] = {
    north_wall_x, south_wall_x, south_wall_x, north_wall_x,
  };
  float local_y[4] = {north_y, south_y, south_y, north_y};
  float local_z[4] = {north_z, south_z, 0.0f, 0.0f};
  float cell_u = source_cell_x * (float)kSimTownCellPixels;
  float cell_v = source_cell_y * (float)kSimTownCellPixels;
  /* Stretched down the wall rather than smeared along it: the top edge takes
   * the outline so it joins the mountain without a seam, and the ground edge
   * takes the interior, so the quad is the tile's own rock in the town's own
   * palette. */
  float v0 = (cell_v + profile->first_row + 0.5f) /
      (float)context->atlas_pixels;
  float v1 = (cell_v + profile->last_row + 0.5f) /
      (float)context->atlas_pixels;
  SimBackgroundMountainMeshUV uv[4] = {
    {(cell_u + profile->outer_first + 0.5f) / context->atlas_pixels, v0},
    {(cell_u + profile->outer_last + 0.5f) / context->atlas_pixels, v1},
    {(cell_u + profile->inner_last + 0.5f) / context->atlas_pixels, v1},
    {(cell_u + profile->inner_first + 0.5f) / context->atlas_pixels, v0},
  };
  /* Points 0 and 1 are the top edge against the mountain; 2 and 3 stand on
   * the ground. */
  const uint8_t brightness[4] = {
    kMountainSkirtTopBrightness, kMountainSkirtTopBrightness,
    kMountainSkirtBaseBrightness, kMountainSkirtBaseBrightness,
  };
  context->emit(context->user, local_x, local_y, local_z, uv,
                brightness, kMountainFullIntensity);
}
