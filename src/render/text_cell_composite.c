#include "render/text_cell_composite.h"

#include <stdbool.h>
#include <string.h>

typedef struct AxisRun {
  int start;
  int length;
} AxisRun;

static bool InCyclicInterval(unsigned value, unsigned start,
                             unsigned length, unsigned period) {
  if (length >= period) return true;
  const unsigned end = start + length;
  return end <= period ? value >= start && value < end
                       : value >= start || value < end - period;
}

static size_t BuildAxisRuns(unsigned region_start, unsigned region_length,
                            unsigned period, unsigned scroll,
                            unsigned fetch_phase, unsigned visible,
                            AxisRun runs[2]) {
  size_t count = 0;
  bool active = false;
  for (unsigned screen = 0; screen < visible; ++screen) {
    const unsigned world = (screen + scroll + fetch_phase) % period;
    const bool inside = InCyclicInterval(
        world, region_start, region_length, period);
    if (inside && !active) {
      if (count >= 2) return 0;
      runs[count] = (AxisRun){(int)screen, 1};
      active = true;
    } else if (inside) {
      ++runs[count].length;
    } else if (active) {
      ++count;
      active = false;
    }
  }
  if (active) ++count;
  return count;
}

size_t ArTextCellComposite_ProjectRegion(
    ArTextCellRegion region, unsigned map_width_tiles,
    unsigned map_height_tiles, uint16_t h_scroll, uint16_t v_scroll,
    unsigned visible_width, unsigned visible_height,
    ArRenderRectI projected[kArTextCellMaximumProjectedRegions]) {
  if (!projected || !region.columns || !region.rows ||
      !map_width_tiles || !map_height_tiles ||
      map_width_tiles > 64 || map_height_tiles > 64 ||
      (unsigned)region.column + region.columns > map_width_tiles ||
      (unsigned)region.row + region.rows > map_height_tiles ||
      !visible_width || !visible_height || visible_width > 512 ||
      visible_height > 512)
    return 0;
  const unsigned map_width = map_width_tiles * 8u;
  const unsigned map_height = map_height_tiles * 8u;
  AxisRun horizontal[2] = {{0}}, vertical[2] = {{0}};
  const size_t horizontal_count = BuildAxisRuns(
      (unsigned)region.column * 8u, (unsigned)region.columns * 8u,
      map_width, h_scroll % map_width, 0, visible_width, horizontal);
  const size_t vertical_count = BuildAxisRuns(
      (unsigned)region.row * 8u, (unsigned)region.rows * 8u,
      map_height, v_scroll % map_height, 1, visible_height, vertical);
  if (!horizontal_count || !vertical_count) return 0;
  size_t count = 0;
  for (size_t y = 0; y < vertical_count; ++y) {
    for (size_t x = 0; x < horizontal_count; ++x) {
      projected[count++] = (ArRenderRectI){
          horizontal[x].start, vertical[y].start,
          horizontal[x].length, vertical[y].length,
      };
    }
  }
  return count;
}

static bool Intersect(ArRenderRectI a, ArRenderRectI b,
                      ArRenderRectI *intersection) {
  const int left = a.x > b.x ? a.x : b.x;
  const int top = a.y > b.y ? a.y : b.y;
  const int a_right = a.x + a.w;
  const int b_right = b.x + b.w;
  const int right = a_right < b_right ? a_right : b_right;
  const int a_bottom = a.y + a.h;
  const int b_bottom = b.y + b.h;
  const int bottom = a_bottom < b_bottom ? a_bottom : b_bottom;
  if (right <= left || bottom <= top) return false;
  if (intersection)
    *intersection = (ArRenderRectI){left, top, right - left, bottom - top};
  return true;
}

static int ProjectEdge(int edge, int source_start, int source_length,
                       int destination_start, int destination_length) {
  const int relative = edge - source_start;
  const int64_t scaled = (int64_t)relative * destination_length;
  return destination_start +
      (int)((scaled + (scaled >= 0 ? source_length / 2
                                  : -source_length / 2)) / source_length);
}

static HudPresentationChunk SliceChunk(
    const HudPresentationChunk *chunk, ArRenderRectI screen) {
  HudPresentationChunk piece = *chunk;
  const int source_left = ProjectEdge(
      screen.x, chunk->screen_source.x, chunk->screen_source.w,
      chunk->texture_source.x, chunk->texture_source.w);
  const int source_right = ProjectEdge(
      screen.x + screen.w, chunk->screen_source.x, chunk->screen_source.w,
      chunk->texture_source.x, chunk->texture_source.w);
  const int source_top = ProjectEdge(
      screen.y, chunk->screen_source.y, chunk->screen_source.h,
      chunk->texture_source.y, chunk->texture_source.h);
  const int source_bottom = ProjectEdge(
      screen.y + screen.h, chunk->screen_source.y, chunk->screen_source.h,
      chunk->texture_source.y, chunk->texture_source.h);
  const int output_left = ProjectEdge(
      screen.x, chunk->screen_source.x, chunk->screen_source.w,
      chunk->output_destination.x, chunk->output_destination.w);
  const int output_right = ProjectEdge(
      screen.x + screen.w, chunk->screen_source.x, chunk->screen_source.w,
      chunk->output_destination.x, chunk->output_destination.w);
  const int output_top = ProjectEdge(
      screen.y, chunk->screen_source.y, chunk->screen_source.h,
      chunk->output_destination.y, chunk->output_destination.h);
  const int output_bottom = ProjectEdge(
      screen.y + screen.h, chunk->screen_source.y, chunk->screen_source.h,
      chunk->output_destination.y, chunk->output_destination.h);
  piece.screen_source = screen;
  piece.texture_source = (ArRenderRectI){
      source_left, source_top, source_right - source_left,
      source_bottom - source_top};
  piece.output_destination = (ArRenderRectI){
      output_left, output_top, output_right - output_left,
      output_bottom - output_top};
  return piece;
}

static size_t SubtractOne(const HudPresentationChunk *piece,
                          ArRenderRectI mask,
                          HudPresentationChunk output[4]) {
  ArRenderRectI cut;
  if (!Intersect(piece->screen_source, mask, &cut)) {
    output[0] = *piece;
    return 1;
  }
  const ArRenderRectI whole = piece->screen_source;
  size_t count = 0;
  const ArRenderRectI candidates[4] = {
    {whole.x, whole.y, whole.w, cut.y - whole.y},
    {whole.x, cut.y + cut.h, whole.w,
     whole.y + whole.h - (cut.y + cut.h)},
    {whole.x, cut.y, cut.x - whole.x, cut.h},
    {cut.x + cut.w, cut.y,
     whole.x + whole.w - (cut.x + cut.w), cut.h},
  };
  for (size_t index = 0; index < 4; ++index) {
    if (candidates[index].w > 0 && candidates[index].h > 0)
      output[count++] = SliceChunk(piece, candidates[index]);
  }
  return count;
}

size_t ArTextCellComposite_SubtractMasks(
    const HudPresentationChunk *chunk,
    const ArRenderRectI *masks, size_t mask_count,
    HudPresentationChunk *pieces, size_t piece_capacity) {
  if (!chunk || !pieces || !piece_capacity ||
      chunk->screen_source.w <= 0 || chunk->screen_source.h <= 0 ||
      (mask_count && !masks))
    return SIZE_MAX;
  pieces[0] = *chunk;
  size_t piece_count = 1;
  for (size_t mask_index = 0; mask_index < mask_count; ++mask_index) {
    HudPresentationChunk next[kArTextCellMaximumChunkPieces];
    size_t next_count = 0;
    for (size_t piece_index = 0; piece_index < piece_count; ++piece_index) {
      HudPresentationChunk split[4];
      const size_t split_count =
          SubtractOne(&pieces[piece_index], masks[mask_index], split);
      if (next_count + split_count > piece_capacity ||
          next_count + split_count > kArTextCellMaximumChunkPieces)
        return SIZE_MAX;
      memcpy(&next[next_count], split,
             split_count * sizeof(split[0]));
      next_count += split_count;
    }
    memcpy(pieces, next, next_count * sizeof(next[0]));
    piece_count = next_count;
  }
  return piece_count;
}

bool ArTextCellComposite_ProjectToOutput(
    const HudPresentationChunk *chunk, ArRenderRectI screen_region,
    ArRenderRectI *output_region) {
  if (!chunk || !output_region || chunk->screen_source.w <= 0 ||
      chunk->screen_source.h <= 0 || chunk->output_destination.w <= 0 ||
      chunk->output_destination.h <= 0)
    return false;
  ArRenderRectI visible;
  if (!Intersect(chunk->screen_source, screen_region, &visible)) return false;
  *output_region = SliceChunk(chunk, visible).output_destination;
  return output_region->w > 0 && output_region->h > 0;
}
