#ifndef DIORAMA_LAYER_ORDER_H
#define DIORAMA_LAYER_ORDER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "diorama_planes.h"

/* Per-room diorama layer overrides.
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
 * notes at each SDL_RenderGeometry call that it has no depth test). Crucially the two
 * do not agree: Bg2Hi (z=0.21) is painted at slot 7, AFTER Bg1 (z=0.50)
 * at slot 5. So "sort by ascending z when an override is active" is not a
 * refinement of the default order — it is a DIFFERENT order, and it reshuffled
 * five planes even for an edit that changed nothing.
 *
 * `z` also feeds the depth-of-field radius: DofRadiusForLayer(layer->z) against
 * a focal plane hardcoded to BG1's 0.50 (kDofFocalZ in diorama.c). Moving a plane's
 * z to reorder it would therefore silently change how blurred it is — pushing
 * BG2 from 0.20 to 0.52 drops its blur below the cutoff and the water would
 * turn sharp. Keeping the two keys distinct lets a room reorder without
 * disturbing focus, or change focus without reordering.
 *
 * Everything here is pure: no SDL, no globals, no file I/O beyond a caller-
 * supplied line. That keeps the arithmetic and the manifest grammar testable
 * without a ROM or a renderer (precedent: actraiser_ws_gap.c and
 * host_display_pacing.c).
 */

enum {
  /* A room override is identified by (group, map, section). */
  kDioramaRoomOverrideMax = 64,
  kDioramaLayerAlphaOpaque = 255,
  /* Hard cap on stack copies per plane. Each copy is one more RenderGeometry
   * call over the whole layer, so this bounds the worst case an authored file
   * can impose on a frame: 10 planes x this, on top of the base pass. Chosen so
   * a fully-authored room stays well inside a 60 Hz budget rather than because
   * more copies stop looking better. */
  kDioramaStackMax = 8,
  kDioramaStackCopiesDefault = 3,
  /* A voxel fill needs many more slices than a stack, because its whole point is
   * to read as SOLID rather than as distinguishable layers -- and solidity is a
   * function of how close consecutive slices land on screen. Kept as its own cap
   * so authoring a voxel cannot silently raise the budget for ordinary stacks. */
  kDioramaVoxelMax = 24,
  kDioramaVoxelCopiesDefault = 12,
  /* Sparse presentation-only cell regions for one action BG. The editor
   * coalesces painted cells into horizontal rectangles, so ordinary brush
   * work costs one span per affected metatile row rather than one record per
   * cell. The largest shipped room is 128 metatile rows high; 512 leaves room
   * for several disjoint passes while keeping DioramaLayerOrderTable a small,
   * fixed-value object that existing reset/save code can memset safely. */
  kDioramaVirtualCellSpanMax = 512,
  kDioramaVirtualBandCount = 3,
};

/* Optional authored subsection of a room. The base room always resolves first;
 * a non-default section then refines only the knobs it explicitly authors.
 * This lets one decoded map carry distinct presentation areas without making
 * the game invent another room byte. Tokens are part of diorama-layers.ini's
 * stable grammar. */
typedef enum DioramaLayerSection {
  kDioramaLayerSection_Room = 0,
  kDioramaLayerSection_AitosWaterfall,
  kDioramaLayerSection_Count,
} DioramaLayerSection;

const char *DioramaLayerOrder_SectionToken(int section);
int DioramaLayerOrder_SectionFromToken(const char *token);

/* Pixel source for a resolved layer. Captured is the ordinary current-frame
 * plane. Every other valid value encodes one action room and one of its two BG
 * planes. Uniform eight-room slots make the identity stable in manifests; the
 * helpers reject the unused slots in shorter acts. */
typedef enum DioramaLayerSource {
  kDioramaLayerSource_Captured = 0,
  kDioramaLayerSource_ActionBgFirst,
  kDioramaLayerSource_Count =
      kDioramaLayerSource_ActionBgFirst + 7 * 8 * 2,
} DioramaLayerSource;

/* Compatibility name for the first named source shipped before the catalogue
 * was generalized. New manifests format it as `rom-04-01-bg2`; the parser also
 * accepts the historical `aitos-sky` token. */
enum {
  kDioramaLayerSource_AitosSky =
      kDioramaLayerSource_ActionBgFirst + ((4 - 1) * 8 + (1 - 1)) * 2 + 1,
};

const char *DioramaLayerOrder_SourceToken(int source);
int DioramaLayerOrder_SourceFromToken(const char *token);
bool DioramaLayerOrder_SourceIsValid(int source);
int DioramaLayerOrder_ActionBgSource(uint8_t map_group, uint8_t map_number,
                                     uint8_t bg_layer);
bool DioramaLayerOrder_DecodeActionBgSource(int source,
                                            uint8_t *out_map_group,
                                            uint8_t *out_map_number,
                                            uint8_t *out_bg_layer);
int DioramaLayerOrder_NextSource(int source, int direction);

/* The depth strategies a plane can use, as ONE enumeration.
 *
 * This exists for the layer editor: the point of the editor is to cycle a room's
 * plane through the available strategies and watch the result, which needs a
 * single ordered list with names -- not four independent keys the UI has to know
 * to clear in the right combinations. The manifest keys stay as they are (they are
 * more expressive, since a plane can carry a rake AND a thickness), so this is the
 * editor's view of the same data, resolved from whichever keys are authored.
 *
 * Ordered cheapest-first so cycling forward from Flat escalates cost gradually,
 * and so a room half-tuned by an author who ran out of budget stops somewhere
 * sensible. Voxel is last because it is the only one that can cost 24 draws. */
typedef enum DioramaDepthStrategy {
  kDioramaDepth_Flat = 0,   /* no depth: a parallel sheet, as every room shipped */
  kDioramaDepth_Rake,       /* tilt the plane linearly */
  kDioramaDepth_Bow,        /* tilt it on a curve -- eases in, no shear at the top */
  kDioramaDepth_Thick,      /* extrude a near face from the quad's bottom edge */
  kDioramaDepth_Stack,      /* parallel repeats, faded: several things in depth */
  kDioramaDepth_Voxel,      /* dense unfaded repeats: one solid object */
  kDioramaDepth_StrategyCount,
} DioramaDepthStrategy;

/* Strategy name for the editor row and logs ("flat", "rake", ...). Never NULL. */
const char *DioramaLayerOrder_StrategyName(DioramaDepthStrategy strategy);

/* Which way a stack lays its copies, relative to the plane's own depth. Higher z
 * is NEARER the camera in this projection (the backdrop is z=0.00, the HUD z=0.95),
 * so Forward means toward the viewer.
 *
 * Forward is the default because the reported case needs it: Fillmore act 2's
 * water sits BEHIND the rock path (z=0.21 vs 0.50), so the gap to fill is between
 * the water and the camera. Backward exists for the mirror case -- a foreground
 * layer that should recede into the scene behind it -- and Both spreads the fill
 * either side of the plane, for something the plane sits in the MIDDLE of, like a
 * cloud bank or a dust volume. */
typedef enum DioramaStackDirection {
  kDioramaStack_Forward = 0,   /* z .. z + depth  (toward the camera) */
  kDioramaStack_Backward = 1,  /* z - depth .. z  (away from the camera) */
  kDioramaStack_Both = 2,      /* z - depth/2 .. z + depth/2 */
  kDioramaStack_DirectionCount,
} DioramaStackDirection;

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
  bool set_source;
  uint8_t source;     /* DioramaLayerSource; Backdrop record → skybox */
  /* RAKE — the layer stops being parallel to the screen and tilts in depth: its
   * TOP edge keeps `z`, its BOTTOM edge sits at `z + rake`. Positive rakes the
   * bottom toward the camera.
   *
   * This exists because two parallel planes at different depths leave a visible
   * VOID between them once the diorama camera tilts — you see past the near
   * plane's bottom edge into the gap. Fillmore act 2 is the reported case: the
   * water is Bg2Hi at z=0.21 and the rock path is Bg1 at z=0.50, so the water
   * appears to float behind a hole. Raking the water forward until its near edge
   * meets the rock's depth closes the gap, and for a water surface viewed from
   * an angle it is also the physically right shape -- a surface receding into
   * the distance rather than a billboard.
   *
   * Free-form rather than "snap to the next layer": the converging target is a
   * judgement call per room, and the editor can offer the snap as a preset
   * without the data model hard-coding it. */
  bool set_rake;
  float rake;
  /* BOW — a rake on a curve instead of a straight tilt.
   *
   * Same endpoints as a rake: the top edge keeps `z`, the bottom edge lands at
   * `z + bow`. What differs is everything in between. A rake's depth is linear in
   * t, so dz/dt is CONSTANT: every row is displaced in depth, including the ones
   * near the top, which is why a raked layer's parallax reads uniformly
   * exaggerated. A bow is quadratic, so dz/dt is 0 at the top and greatest at the
   * bottom -- the layer keeps its original depth behaviour where it meets the sky
   * and only bends forward near the fold, which is where it actually needs to
   * reach the layer in front.
   *
   * Worth trying wherever a rake was the right idea but read as too much: it
   * closes the same gap with the same number, concentrating the distortion where
   * the geometry demands it instead of spreading it over the whole plane. Still a
   * tilt, so it still spans a depth range and still has more than one parallax
   * rate -- if that is unacceptable at all, the shape wanted is a stack. */
  bool set_bow;
  float bow;
  /* THICKNESS — the solid-volume alternative to a rake: extrude the plane's
   * BOTTOM edge forward from `z` to `z + thickness`, so the layer reads as a
   * block with a near face rather than an infinitely thin sheet.
   *
   * Kept distinct from `rake` because they are different shapes, not two
   * spellings of one, and a room may want both (they compose: the skirt starts
   * at the raked bottom edge, so the fold does not tear).
   *
   * Which to reach for: a rake tilts the WHOLE plane, so its art stretches in
   * depth and it stops being parallel to the screen -- right for a water surface
   * receding into the distance, wrong for a rock face whose front should stay
   * square to the camera. A thickness leaves the plane untouched and adds
   * geometry below it, so the art is unchanged and only the fold is new.
   *
   * The skirt is textured with the plane's bottom source row repeated, shaded
   * darker with depth. That is the honest limit of extruding a flat 2D capture:
   * there is no side-face art to sample, so the row already at the fold is what
   * continues, and the gradient is what makes the fold legible rather than a
   * smear. */
  bool set_thickness;
  float thickness;
  /* STACK — fill the depth gap by REPEATING the layer at intermediate depths,
   * instead of tilting it (rake) or extruding a side face from it (thickness).
   *
   * `stack` is how deep to fill, in the same z units as `rake`: copies are laid
   * from `z` toward `z + stack`. `stack_copies` is how many, 1..kDioramaStackMax.
   *
   * Why a third shape rather than a variant of the other two. A rake tilts the
   * plane, which puts its own rows at DIFFERENT depths -- so the layer picks up
   * two different parallax rates inside itself and shears as the camera moves,
   * over-exaggerating that layer's parallax. That is the reported problem with
   * the rake, and it is intrinsic to tilting rather than a tuning error: the
   * perspective divide is per-vertex, so any plane spanning a depth range has a
   * depth-dependent scale across its own surface. A stack never tilts anything.
   * Every copy stays exactly parallel to the screen, so every copy has ONE
   * parallax rate, and the layer as a whole keeps the flat, poster-like motion
   * the diorama is built on -- it just occupies depth instead of being a sheet.
   *
   * The trade, stated honestly: copies are discrete, so the gap is filled by
   * layered slices rather than continuous solid, and gaps remain BETWEEN slices.
   * More copies close them at a linear cost in draw calls. This reads well for
   * foliage, crowds, rain, cloud banks and rubble -- anything whose real-world
   * form is many similar things at different depths -- and poorly for a surface
   * that should be continuous, which is what thickness and rake are for.
   *
   * Copies fade and darken with depth so the stack reads as receding volume
   * rather than as a smear of identical sprites. */
  bool set_stack;
  float stack;
  /* Copy count. Authoring this directly pins an EXACT number of slices, which is
   * what you want when the look depends on the count itself (four distinct
   * cloud banks, say). Mutually informative with `stack_density` below: an
   * explicit count always wins, since it is the more specific instruction. */
  bool set_stack_copies;
  int stack_copies;
  /* Copies per unit of depth, as a fraction of the fill. Density rather than an
   * absolute count because slice SPACING is what the eye judges, and a fixed
   * count gives different spacing in every room: `copies:4` across a 0.29 gap
   * spaces slices 0.097 apart, but across a 0.10 gap only 0.033. Authoring a
   * density instead keeps the visual result consistent as the gap changes, and
   * lets one value be reused across rooms.
   *
   * Resolved to a count and clamped to kDioramaStackMax, so a large density on a
   * deep fill cannot silently blow the per-frame draw budget. */
  bool set_stack_density;
  float stack_density;
  bool set_stack_direction;
  int stack_direction;   /* DioramaStackDirection */
  /* VOXEL — extrude the layer through depth by repeating it densely with NO
   * fade, so it reads as one solid object rather than as separate slices.
   *
   * Mechanically a stack: parallel copies, so it inherits the stack's freedom
   * from the rake's shear. What differs is intent, and therefore two settings --
   * the falloff is off and the slice count is much higher (kDioramaVoxelMax).
   *
   * Why it is not just `thick` with more work. A thickness hangs its skirt from
   * the QUAD's bottom edge, so it draws a straight band across the layer's whole
   * width even where the art is transparent -- right for a cliff that spans the
   * layer, wrong for a rock island with sky either side. A voxel's copies carry
   * the layer's OWN alpha (a captured BG is mostly transparent with art islands;
   * palette index 0 stays transparent, see ppu.c), so every island extrudes
   * itself and the silhouette is respected for free. That is the case thick
   * cannot express.
   *
   * Why it is not just `stack` with density cranked up: it can be authored that
   * way, and that is precisely the point -- but the stack's cap and its fade are
   * tuned for reading as depth LAYERS. Naming the solid intent separately means
   * the defaults are right without every room restating them, and a reader can
   * tell "several things at different depths" from "one object with volume".
   *
   * COST: the most expensive shape here, up to kDioramaVoxelMax draws of the
   * whole layer. Use it on one plane in a room, not on all of them. */
  bool set_voxel;
  float voxel;
  bool set_voxel_copies;
  int voxel_copies;
} DioramaPlaneOverride;

/* Presentation-only depth classification for one action background.
 *
 * This is intentionally kept beside the ordinary room plane overrides: both
 * are authored in the same [layers:GG:MM] section and must survive the same
 * comment-preserving merge. It does not change the authentic SNES tile word.
 * The later renderer consumes the resolved band only while splitting captured
 * diorama surfaces, which is what keeps flat presentation byte-identical.
 *
 * Band 0 is a new far surface, band 1 is the BG's ordinary priority-0 plane,
 * and band 2 is its existing priority-1 plane. Cell spans override metatile
 * rules; the tile word's priority bit is the fallback. */
typedef struct DioramaVirtualCellSpan {
  uint16_t x0, y0;
  uint16_t x1, y1;  /* inclusive */
  uint8_t band;
} DioramaVirtualCellSpan;

typedef struct DioramaVirtualLayerOverride {
  bool set_z;
  float z;
  bool set_order;
  int order;
  bool set_alpha;
  uint8_t alpha;
  /* One presence bit per metatile id. A separate bitset lets zero-initialized
   * rooms mean "no rules" even though band 0 is a valid authored value. */
  uint8_t metatile_set[32];
  uint8_t metatile_bands[256];
  uint16_t cell_span_count;
  DioramaVirtualCellSpan cell_spans[kDioramaVirtualCellSpanMax];
} DioramaVirtualLayerOverride;

typedef struct DioramaRoomOverride {
  bool used;
  uint8_t map_group;   /* $18 */
  uint8_t map_number;  /* $19 */
  uint8_t section;     /* DioramaLayerSection; Room is the base override */
  DioramaPlaneOverride planes[kDioramaPlane_Count];
  DioramaVirtualLayerOverride virtual_layers[2]; /* BG1, BG2 */
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
  uint8_t source;     /* DioramaLayerSource */
  float rake;       /* bottom edge sits at z + rake; 0 = parallel */
  float bow;        /* same, but eased quadratically; 0 = no curve */
  float thickness;  /* extrude the bottom edge forward to z + thickness; 0 = flat */
  float stack;      /* fill depth by repeating the layer to z + stack; 0 = off */
  int stack_copies; /* resolved repeat count, 1..kDioramaStackMax */
  int stack_direction; /* DioramaStackDirection */
  /* Voxel: a solid extrusion. Resolves onto the same stack fields plus this flag,
   * because the geometry is identical -- only the fade and the cap differ. */
  bool stack_solid;
} DioramaResolvedLayer;

/* The skybox source is authored on the Backdrop record so the manifest and
 * editor do not need a second room-scoped object. It is deliberately resolved
 * independently of that plane's alpha/visibility: those fields control the
 * residual in-box Backdrop geometry, while `source` controls the surrounding
 * skybox whenever the global skybox mode is enabled. */
int DioramaLayerOrder_SkyboxSource(const DioramaResolvedLayer *layers,
                                   int count);

/* Which strategy a resolved plane is using, for the editor's label and for
 * diagnostics. Derived, not stored: whichever authored key is most specific wins,
 * in the same precedence the renderer applies. Flat when none is authored. */
DioramaDepthStrategy DioramaLayerOrder_StrategyOf(
    const DioramaResolvedLayer *layer);

/* Resolve a copy count from an authored density and fill depth.
 *
 * Density is copies per unit depth, so the count scales with the gap and slice
 * SPACING stays consistent across rooms -- which is what the eye judges. Always
 * returns at least 2 for a non-zero fill (one copy is just the plane again, so a
 * density that rounded to 1 would silently disable the stack), and never more
 * than kDioramaStackMax, since each copy is another full-layer draw call.
 * Returns 1 for a zero/negative depth or density, i.e. no stack. Pure. */
int DioramaLayerOrder_StackCopiesForDensity(float depth, float density);

/* Manifest token for a stack direction ("forward"/"backward"/"both"), and its
 * inverse (-1 on an unknown token). These strings ARE the file grammar, so
 * renaming one invalidates authored files. */
const char *DioramaLayerOrder_StackDirectionToken(int direction);
int DioramaLayerOrder_StackDirectionFromToken(const char *token);

/* Find a room, or NULL. Pure lookup, no insertion. */
const DioramaRoomOverride *DioramaLayerOrder_Find(
    const DioramaLayerOrderTable *table, uint8_t map_group, uint8_t map_number);

/* Find a room or create it. Returns NULL only when the table is full, which is
 * reported rather than silently dropping an edit. */
DioramaRoomOverride *DioramaLayerOrder_FindOrAdd(
    DioramaLayerOrderTable *table, uint8_t map_group, uint8_t map_number);

/* Scoped variants used by section-aware presentation and the live editor.
 * The legacy two-byte helpers above deliberately address only the base room. */
const DioramaRoomOverride *DioramaLayerOrder_FindSection(
    const DioramaLayerOrderTable *table, uint8_t map_group,
    uint8_t map_number, uint8_t section);
DioramaRoomOverride *DioramaLayerOrder_FindOrAddSection(
    DioramaLayerOrderTable *table, uint8_t map_group, uint8_t map_number,
    uint8_t section);

/* True when the room has an authored plane or virtual action-layer record. A
 * room whose every override was reset is NOT active, so it behaves exactly as
 * if it had never been authored. */
bool DioramaLayerOrder_RoomIsActive(const DioramaRoomOverride *room);

/* Resolve one action tile's presentation band without changing its SNES
 * priority. `bg` is 0 for BG1 or 1 for BG2. Cell rules are checked newest
 * first so a later line in the INI refines an earlier rectangle. Invalid
 * inputs fall back to the authentic priority bit (band 1 or 2). */
int DioramaLayerOrder_VirtualBand(const DioramaRoomOverride *room, int bg,
                                  int cell_x, int cell_y,
                                  uint8_t metatile, uint16_t tile_word);

/* Whether one BG has authored geometry or classification records. */
bool DioramaLayerOrder_VirtualLayerIsAuthored(
    const DioramaVirtualLayerOverride *layer);

/* Drop only the legacy in-game plane editor's overrides. Virtual action-layer
 * records are authored by the standalone action editor and must survive this
 * operation. The full reset functions below remain available for callers that
 * intentionally own the complete section. */
void DioramaLayerOrder_ResetPlaneOverridesSection(
    DioramaLayerOrderTable *table, uint8_t map_group, uint8_t map_number,
    uint8_t section);

/* Drop every override for a room or section. */
void DioramaLayerOrder_ResetRoom(DioramaLayerOrderTable *table,
                                 uint8_t map_group, uint8_t map_number);
void DioramaLayerOrder_ResetSection(DioramaLayerOrderTable *table,
                                    uint8_t map_group, uint8_t map_number,
                                    uint8_t section);

/* Resolve the draw list for a room.
 *
 * `defaults` / `default_count` are the built-in planes in their built-in table
 * order, with their built-in z. On no override the output is the defaults
 * verbatim, in the same order, with alpha 255 — so an inactive table preserves
 * the built-in behavior exactly.
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

/* Resolve the base room and then an optional section refinement. */
int DioramaLayerOrder_ResolveSection(const DioramaLayerOrderTable *table,
                                     uint8_t map_group, uint8_t map_number,
                                     uint8_t section,
                                     const DioramaResolvedLayer *defaults,
                                     int default_count,
                                     DioramaResolvedLayer *out, int capacity);

/* The plane's manifest token ("bg1", "bg2hi", "obj2", ...), or NULL if the
 * plane index is not one the diorama draws. Stable across versions: these
 * strings are the manifest's grammar, so renaming one breaks authored files. */
const char *DioramaLayerOrder_PlaneToken(int plane);

/* Inverse of PlaneToken. Returns -1 on an unknown token. */
int DioramaLayerOrder_PlaneFromToken(const char *token);

/* Iterate the drawable planes in MANIFEST ORDER — the order FormatRoom emits,
 * which is not plane-index order. Exported so the layer editor can list planes
 * the same way an export writes them without holding a second copy of the
 * order; `index` runs 0..DioramaLayerOrder_PlaneCount()-1 and PlaneAt returns
 * -1 outside it. */
int DioramaLayerOrder_PlaneCount(void);
int DioramaLayerOrder_PlaneAt(int index);

/* Parse one manifest body line into `room`, e.g. "bg1 = z:0.55 alpha:255".
 * Returns false on a malformed line; `*out_error` (optional) gets a short
 * reason for the log. Whitespace-tolerant; a line may set z, alpha, or both. */
bool DioramaLayerOrder_ParseLine(DioramaRoomOverride *room, const char *line,
                                 const char **out_error);

/* Parse a section header of the form "layers:GG:MM" (hex, as the WRAM bytes
 * read). Returns false if it is not one of ours or is malformed. */
bool DioramaLayerOrder_ParseSection(const char *section, uint8_t *out_group,
                                    uint8_t *out_map);

/* Extended grammar: "layers:GG:MM[:token]". The two-byte parser above remains
 * strict and accepts only the base-room spelling. */
bool DioramaLayerOrder_ParseScopedSection(const char *section,
                                          uint8_t *out_group,
                                          uint8_t *out_map,
                                          uint8_t *out_layer_section);

/* Render a room as manifest text into `buffer`. Returns the number of bytes
 * that WOULD be written (like snprintf), so a caller can detect truncation.
 * Emits nothing for an inactive room. */
size_t DioramaLayerOrder_FormatRoom(const DioramaRoomOverride *room,
                                    char *buffer, size_t size);

/* Rewrite a manifest, PRESERVING everything the editor does not own.
 *
 * The naive save rewrites the whole file from the table, which destroys the
 * documentation preamble, any hand-written comments, and any section the editor
 * has never touched -- the file is one people are invited to edit, so wiping it
 * on the next slider move is the worst thing this code can do. Instead this
 * merges: `existing` is the file's current contents (may be NULL/empty on a
 * first write), and every line is passed through UNCHANGED except the body of a
 * section that resolves to a room the table currently marks active -- that body
 * is regenerated in place from FormatRoom. Active rooms with no section already
 * in the file are appended at the end.
 *
 * What this guarantees:
 *   - the preamble, blank lines, standalone comments and any FOREIGN section
 *     survive byte-for-byte;
 *   - a managed room's body is exactly what FormatRoom would emit, so the file
 *     still round-trips;
 *   - a room reset to inactive keeps its section header and any comments around
 *     it, but its PLANE LINES are dropped. Both halves matter: keeping the header
 *     means clearing a room does not erase a section the user wrote notes around,
 *     and dropping the body is what makes "Reset room" persist -- leaving the
 *     stale overrides in the file would make the next load read the room as
 *     active again and silently undo the reset.
 *
 * `default_preamble` is written only when `existing` is NULL or empty -- i.e. a
 * genuinely new file gets the shipped documentation, and an existing file keeps
 * whatever preamble it already has, since re-emitting ours would duplicate it.
 *
 * snprintf contract: returns the byte count that WOULD be written, and never
 * writes past `size`, so a caller sizes its buffer by calling once with size 0.
 * Pure: no I/O, so it is fully testable. */
size_t DioramaLayerOrder_MergeManifest(const DioramaLayerOrderTable *table,
                                       const char *existing,
                                       const char *default_preamble,
                                       char *buffer, size_t size);

#endif /* DIORAMA_LAYER_ORDER_H */
