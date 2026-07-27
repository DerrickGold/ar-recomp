#ifndef DIORAMA_LAYER_ORDER_H
#define DIORAMA_LAYER_ORDER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "diorama_planes.h"

/* Per-room diorama layer overrides (F3/F4 from the 2026-07-26 on-device
 * handback).
 *
 * The problem: a stage's layers do not always want the diorama's default paint
 * order. Fillmore Act 2 renders its water band BEHIND the rock path, when it
 * should be in front. Which layer belongs in front is a property of what the
 * stage is drawing, so it cannot be derived — it has to be authored per room.
 *
 * The room key is ($18, $19) — the game's own map-group and map-number bytes.
 * $19 already indexes rooms WITHIN a group: Death Heim is group $07 with maps
 * $01..$08 (hub, six bosses, final — see actraiser_game.h), which is where the
 * distinction was first observed while mapping level warps. So no new
 * identifier is needed and no ROM tracing is required.
 *
 * Three knobs per plane:
 *   - `order`  where the plane sits in the paint sequence, back to front.
 *   - `z`      the plane's depth in the 3D projection.
 *   - `alpha`  the plane's opacity, 0..255. Lets a room compensate for a
 *              translucency the capture did not reproduce.
 *
 * IMPORTANT — why `order` is SEPARATE from `z`, which an earlier revision of
 * this module got wrong. diorama.c's paint order is the literal order of the
 * kDioramaLayers table and nothing sorts by z (no qsort in diorama.c; the file
 * notes at :1106 that SDL_RenderGeometry has no depth test). Crucially the two
 * do NOT agree today: Bg2Hi (z=0.21) is painted at slot 7, AFTER Bg1 (z=0.50)
 * at slot 5. So "sort by ascending z when an override is active" is not a
 * refinement of the default order — it is a DIFFERENT order, and it reshuffled
 * five planes even for an edit that changed nothing.
 *
 * `z` also feeds the depth-of-field radius: DofRadiusForLayer(layer->z) against
 * a focal plane hardcoded to BG1's 0.50 (diorama.c:153, :1371). Moving a plane's
 * z to reorder it would therefore silently change how blurred it is — pushing
 * BG2 from 0.20 to 0.52 drops its blur below the cutoff and the water would
 * turn sharp. Keeping the two keys distinct lets a room reorder without
 * disturbing focus, or change focus without reordering.
 *
 * Everything here is pure: no SDL, no globals, no file I/O beyond a caller-
 * supplied line. That keeps the arithmetic and the manifest grammar testable
 * without a ROM or a renderer (precedent: diorama_scroll_math.c,
 * actraiser_ws_gap.c, host_display_pacing.c).
 */

enum {
  /* A room is identified by one (group, map) byte pair. */
  kDioramaRoomOverrideMax = 64,
  kDioramaLayerAlphaOpaque = 255,
};

/* One plane's override within a room. Each knob has its own `set` flag so a
 * room can author exactly one of them: `set_order` without `set_z` reorders
 * without touching depth-of-field, and vice versa. That also makes export /
 * re-import lossless, and makes an edit that changes nothing genuinely change
 * nothing. */
typedef struct DioramaPlaneOverride {
  bool set_order;
  int order;        /* paint slot, back (0) to front; ties keep table order */
  bool set_z;
  float z;
  bool set_alpha;
  uint8_t alpha;
} DioramaPlaneOverride;

typedef struct DioramaRoomOverride {
  bool used;
  uint8_t map_group;   /* $18 */
  uint8_t map_number;  /* $19 */
  DioramaPlaneOverride planes[kDioramaPlane_Count];
} DioramaRoomOverride;

typedef struct DioramaLayerOrderTable {
  DioramaRoomOverride rooms[kDioramaRoomOverrideMax];
  int count;
} DioramaLayerOrderTable;

/* One resolved plane, ready for the caller to draw. */
typedef struct DioramaResolvedLayer {
  int plane;
  float z;
  uint8_t alpha;
} DioramaResolvedLayer;

/* Find a room, or NULL. Pure lookup, no insertion. */
const DioramaRoomOverride *DioramaLayerOrder_Find(
    const DioramaLayerOrderTable *table, uint8_t map_group, uint8_t map_number);

/* Find a room or create it. Returns NULL only when the table is full, which is
 * reported rather than silently dropping an edit. */
DioramaRoomOverride *DioramaLayerOrder_FindOrAdd(
    DioramaLayerOrderTable *table, uint8_t map_group, uint8_t map_number);

/* True when the room has at least one authored plane. A room whose every
 * override was reset is NOT active, so it behaves exactly as if it had never
 * been authored — that is what makes "Reset room" a true undo. */
bool DioramaLayerOrder_RoomIsActive(const DioramaRoomOverride *room);

/* Drop every override for a room (the editor's "Reset room"). */
void DioramaLayerOrder_ResetRoom(DioramaLayerOrderTable *table,
                                 uint8_t map_group, uint8_t map_number);

/* Resolve the draw list for a room.
 *
 * `defaults` / `default_count` are the built-in planes in their built-in table
 * order, with their built-in z. On no override the output is the defaults
 * verbatim, in the same order, with alpha 255 — so an inactive table is
 * bit-identical to today's behaviour.
 *
 * With an override active, planes carrying an authored `order` are placed at
 * that slot and everything else keeps its built-in relative position. Planes
 * with no authored `order` are NEVER moved, so an override that only sets z or
 * alpha leaves the paint sequence byte-identical to the default. The sort is
 * stable, so equal keys keep built-in table order and the four OBJ planes never
 * reshuffle among themselves. Returns the number written, never more than
 * `capacity`.
 */
int DioramaLayerOrder_Resolve(const DioramaLayerOrderTable *table,
                              uint8_t map_group, uint8_t map_number,
                              const DioramaResolvedLayer *defaults,
                              int default_count,
                              DioramaResolvedLayer *out, int capacity);

/* The plane's manifest token ("bg1", "bg2hi", "obj2", ...), or NULL if the
 * plane index is not one the diorama draws. Stable across versions: these
 * strings are the manifest's grammar, so renaming one breaks authored files. */
const char *DioramaLayerOrder_PlaneToken(int plane);

/* Inverse of PlaneToken. Returns -1 on an unknown token. */
int DioramaLayerOrder_PlaneFromToken(const char *token);

/* Parse one manifest body line into `room`, e.g. "bg1 = z:0.55 alpha:255".
 * Returns false on a malformed line; `*out_error` (optional) gets a short
 * reason for the log. Whitespace-tolerant; a line may set z, alpha, or both. */
bool DioramaLayerOrder_ParseLine(DioramaRoomOverride *room, const char *line,
                                 const char **out_error);

/* Parse a section header of the form "layers:GG:MM" (hex, as the WRAM bytes
 * read). Returns false if it is not one of ours or is malformed. */
bool DioramaLayerOrder_ParseSection(const char *section, uint8_t *out_group,
                                    uint8_t *out_map);

/* Render a room as manifest text into `buffer`. Returns the number of bytes
 * that WOULD be written (like snprintf), so a caller can detect truncation.
 * Emits nothing for an inactive room. */
size_t DioramaLayerOrder_FormatRoom(const DioramaRoomOverride *room,
                                    char *buffer, size_t size);

#endif /* DIORAMA_LAYER_ORDER_H */
