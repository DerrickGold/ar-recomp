#include "diorama_layer_order.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"

/* Manifest tokens. These ARE the file grammar — renaming one invalidates every
 * authored manifest, so they are deliberately terse and stable. */
static const struct { int plane; const char *token; } kPlaneTokens[] = {
  { kDioramaPlane_Backdrop, "backdrop" },
  { kPpuOverlaySource_Bg1,  "bg1" },
  { kDioramaPlane_Bg1Hi,    "bg1hi" },
  { kPpuOverlaySource_Bg2,  "bg2" },
  { kDioramaPlane_Bg2Hi,    "bg2hi" },
  { kPpuOverlaySource_Bg3,  "bg3" },
  { kPpuOverlaySource_Obj,  "obj0" },
  { kDioramaPlane_Obj1,     "obj1" },
  { kDioramaPlane_Obj2,     "obj2" },
  { kDioramaPlane_Obj3,     "obj3" },
};
static const int kPlaneTokenCount =
    (int)(sizeof(kPlaneTokens) / sizeof(kPlaneTokens[0]));

static const char *const kSectionTokens[kDioramaLayerSection_Count] = {
  [kDioramaLayerSection_Room] = NULL,
  [kDioramaLayerSection_AitosWaterfall] = "waterfall",
};

const char *DioramaLayerOrder_SectionToken(int section) {
  if (section <= kDioramaLayerSection_Room ||
      section >= kDioramaLayerSection_Count)
    return NULL;
  return kSectionTokens[section];
}

int DioramaLayerOrder_SectionFromToken(const char *token) {
  if (!token || !token[0]) return kDioramaLayerSection_Room;
  for (int i = kDioramaLayerSection_Room + 1;
       i < kDioramaLayerSection_Count; i++)
    if (kSectionTokens[i] && !strcmp(kSectionTokens[i], token)) return i;
  return -1;
}

const char *DioramaLayerOrder_SourceToken(int source) {
  static char token[20];
  uint8_t group = 0, map = 0, bg = 0;
  if (source == kDioramaLayerSource_Captured) return "captured";
  if (!DioramaLayerOrder_DecodeActionBgSource(source, &group, &map, &bg))
    return "captured";
  snprintf(token, sizeof(token), "rom-%02x-%02x-bg%u", group, map, bg);
  return token;
}

int DioramaLayerOrder_SourceFromToken(const char *token) {
  if (!token) return -1;
  if (!strcmp(token, "captured")) return kDioramaLayerSource_Captured;
  if (!strcmp(token, "aitos-sky")) return kDioramaLayerSource_AitosSky;
  unsigned group = 0, map = 0, bg = 0;
  char tail = 0;
  if (sscanf(token, "rom-%2x-%2x-bg%u%c", &group, &map, &bg, &tail) != 3 ||
      group > 0xFF || map > 0xFF || bg > 0xFF)
    return -1;
  return DioramaLayerOrder_ActionBgSource(
      (uint8_t)group, (uint8_t)map, (uint8_t)bg);
}

int DioramaLayerOrder_ActionBgSource(uint8_t map_group, uint8_t map_number,
                                     uint8_t bg_layer) {
  if (!ActRaiser_IsActionMap(map_group, map_number) ||
      (bg_layer != 1 && bg_layer != 2))
    return -1;
  return kDioramaLayerSource_ActionBgFirst +
      (((int)map_group - 1) * 8 + ((int)map_number - 1)) * 2 +
      ((int)bg_layer - 1);
}

bool DioramaLayerOrder_DecodeActionBgSource(int source,
                                            uint8_t *out_map_group,
                                            uint8_t *out_map_number,
                                            uint8_t *out_bg_layer) {
  const int slot = source - kDioramaLayerSource_ActionBgFirst;
  if (slot < 0 || slot >= 7 * 8 * 2) return false;
  const uint8_t group = (uint8_t)(slot / (8 * 2) + 1);
  const uint8_t map = (uint8_t)((slot / 2) % 8 + 1);
  const uint8_t bg = (uint8_t)(slot % 2 + 1);
  if (!ActRaiser_IsActionMap(group, map)) return false;
  if (out_map_group) *out_map_group = group;
  if (out_map_number) *out_map_number = map;
  if (out_bg_layer) *out_bg_layer = bg;
  return true;
}

bool DioramaLayerOrder_SourceIsValid(int source) {
  return source == kDioramaLayerSource_Captured ||
      DioramaLayerOrder_DecodeActionBgSource(source, NULL, NULL, NULL);
}

int DioramaLayerOrder_NextSource(int source, int direction) {
  if (!DioramaLayerOrder_SourceIsValid(source))
    source = kDioramaLayerSource_Captured;
  for (int attempts = 0; attempts < kDioramaLayerSource_Count; attempts++) {
    source += direction < 0 ? -1 : 1;
    if (source < 0) source = kDioramaLayerSource_Count - 1;
    if (source >= kDioramaLayerSource_Count) source = 0;
    if (DioramaLayerOrder_SourceIsValid(source)) return source;
  }
  return kDioramaLayerSource_Captured;
}

const char *DioramaLayerOrder_PlaneToken(int plane) {
  for (int i = 0; i < kPlaneTokenCount; i++)
    if (kPlaneTokens[i].plane == plane) return kPlaneTokens[i].token;
  return NULL;
}

int DioramaLayerOrder_PlaneFromToken(const char *token) {
  if (!token) return -1;
  for (int i = 0; i < kPlaneTokenCount; i++)
    if (!strcmp(kPlaneTokens[i].token, token)) return kPlaneTokens[i].plane;
  return -1;
}

int DioramaLayerOrder_PlaneCount(void) { return kPlaneTokenCount; }

int DioramaLayerOrder_PlaneAt(int index) {
  if (index < 0 || index >= kPlaneTokenCount) return -1;
  return kPlaneTokens[index].plane;
}

static const char *const kStrategyNames[] = {
  "flat", "rake", "bow", "thick", "stack", "voxel",
};

const char *DioramaLayerOrder_StrategyName(DioramaDepthStrategy strategy) {
  if (strategy < 0 || strategy >= kDioramaDepth_StrategyCount) return "flat";
  return kStrategyNames[strategy];
}

DioramaDepthStrategy DioramaLayerOrder_StrategyOf(
    const DioramaResolvedLayer *l) {
  if (!l) return kDioramaDepth_Flat;
  /* Same precedence the renderer applies, most specific first. A plane can carry
   * several keys at once -- the manifest deliberately allows it -- so this reports
   * the one that dominates the look rather than pretending they are exclusive. */
  if (l->stack > 0.0f && l->stack_solid) return kDioramaDepth_Voxel;
  if (l->stack > 0.0f && l->stack_copies > 1) return kDioramaDepth_Stack;
  if (l->thickness > 0.0f) return kDioramaDepth_Thick;
  if (l->bow != 0.0f) return kDioramaDepth_Bow;
  if (l->rake != 0.0f) return kDioramaDepth_Rake;
  return kDioramaDepth_Flat;
}

int DioramaLayerOrder_SkyboxSource(const DioramaResolvedLayer *layers,
                                   int count) {
  if (!layers || count <= 0) return kDioramaLayerSource_Captured;
  for (int i = 0; i < count; i++) {
    if (layers[i].plane == kDioramaPlane_Backdrop &&
        DioramaLayerOrder_SourceIsValid(layers[i].source))
      return layers[i].source;
  }
  return kDioramaLayerSource_Captured;
}

static const struct { int direction; const char *token; } kStackDirTokens[] = {
  { kDioramaStack_Forward,  "forward" },
  { kDioramaStack_Backward, "backward" },
  { kDioramaStack_Both,     "both" },
};

const char *DioramaLayerOrder_StackDirectionToken(int direction) {
  for (size_t i = 0; i < sizeof(kStackDirTokens) / sizeof(kStackDirTokens[0]); i++)
    if (kStackDirTokens[i].direction == direction) return kStackDirTokens[i].token;
  return "forward";
}

int DioramaLayerOrder_StackDirectionFromToken(const char *token) {
  if (!token) return -1;
  for (size_t i = 0; i < sizeof(kStackDirTokens) / sizeof(kStackDirTokens[0]); i++)
    if (!strcmp(kStackDirTokens[i].token, token)) return kStackDirTokens[i].direction;
  return -1;
}

int DioramaLayerOrder_StackCopiesForDensity(float depth, float density) {
  if (!(depth > 0.0f) || !(density > 0.0f)) return 1;   /* also catches NaN */
  /* +1 because N slices span N-1 intervals: a density of 10 over a 0.2 fill wants
   * 2 intervals and therefore 3 planes. */
  float exact = depth * density;
  int copies = (int)(exact + 0.5f) + 1;
  if (copies < 2) copies = 2;
  if (copies > kDioramaStackMax) copies = kDioramaStackMax;
  return copies;
}

const DioramaRoomOverride *DioramaLayerOrder_Find(
    const DioramaLayerOrderTable *table, uint8_t map_group,
    uint8_t map_number) {
  return DioramaLayerOrder_FindSection(
      table, map_group, map_number, kDioramaLayerSection_Room);
}

const DioramaRoomOverride *DioramaLayerOrder_FindSection(
    const DioramaLayerOrderTable *table, uint8_t map_group,
    uint8_t map_number, uint8_t section) {
  if (!table) return NULL;
  for (int i = 0; i < table->count; i++) {
    const DioramaRoomOverride *room = &table->rooms[i];
    if (room->used && room->map_group == map_group &&
        room->map_number == map_number && room->section == section)
      return room;
  }
  return NULL;
}

DioramaRoomOverride *DioramaLayerOrder_FindOrAdd(
    DioramaLayerOrderTable *table, uint8_t map_group, uint8_t map_number) {
  return DioramaLayerOrder_FindOrAddSection(
      table, map_group, map_number, kDioramaLayerSection_Room);
}

DioramaRoomOverride *DioramaLayerOrder_FindOrAddSection(
    DioramaLayerOrderTable *table, uint8_t map_group, uint8_t map_number,
    uint8_t section) {
  if (section >= kDioramaLayerSection_Count) return NULL;
  if (!table) return NULL;
  for (int i = 0; i < table->count; i++) {
    DioramaRoomOverride *room = &table->rooms[i];
    if (room->used && room->map_group == map_group &&
        room->map_number == map_number && room->section == section)
      return room;
  }
  /* Reuse a slot vacated by a reset before growing. */
  for (int i = 0; i < table->count; i++) {
    if (!table->rooms[i].used) {
      DioramaRoomOverride *room = &table->rooms[i];
      memset(room, 0, sizeof(*room));
      room->used = true;
      room->map_group = map_group;
      room->map_number = map_number;
      room->section = section;
      return room;
    }
  }
  if (table->count >= kDioramaRoomOverrideMax) return NULL;
  DioramaRoomOverride *room = &table->rooms[table->count];
  memset(room, 0, sizeof(*room));
  room->used = true;
  room->map_group = map_group;
  room->map_number = map_number;
  room->section = section;
  table->count++;
  return room;
}

/* True when the author set any of a plane's twelve override knobs. RoomIsActive
 * and the per-plane emit skip in FormatRoomBody must agree on this exact set, so
 * it lives in one place. NOT the same as the editor's shape-only clear subset. */
static bool PlaneOverrideIsAuthored(const DioramaPlaneOverride *o) {
  return o->set_order || o->set_z || o->set_alpha || o->set_source ||
         o->set_rake || o->set_bow ||
         o->set_thickness || o->set_stack || o->set_stack_copies ||
         o->set_stack_density || o->set_stack_direction || o->set_voxel ||
         o->set_voxel_copies;
}

bool DioramaLayerOrder_RoomIsActive(const DioramaRoomOverride *room) {
  if (!room || !room->used) return false;
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    if (PlaneOverrideIsAuthored(&room->planes[plane]))
      return true;
  }
  return false;
}

void DioramaLayerOrder_ResetRoom(DioramaLayerOrderTable *table,
                                 uint8_t map_group, uint8_t map_number) {
  DioramaLayerOrder_ResetSection(
      table, map_group, map_number, kDioramaLayerSection_Room);
}

void DioramaLayerOrder_ResetSection(DioramaLayerOrderTable *table,
                                    uint8_t map_group, uint8_t map_number,
                                    uint8_t section) {
  if (!table) return;
  for (int i = 0; i < table->count; i++) {
    DioramaRoomOverride *room = &table->rooms[i];
    if (room->used && room->map_group == map_group &&
        room->map_number == map_number && room->section == section) {
      memset(room, 0, sizeof(*room));
      /* Left !used so the slot is recycled; count is not decremented because
       * later entries must keep their indices. */
      return;
    }
  }
}

int DioramaLayerOrder_Resolve(const DioramaLayerOrderTable *table,
                              uint8_t map_group, uint8_t map_number,
                              const DioramaResolvedLayer *defaults,
                              int default_count,
                              DioramaResolvedLayer *out, int capacity) {
  return DioramaLayerOrder_ResolveSection(
      table, map_group, map_number, kDioramaLayerSection_Room,
      defaults, default_count, out, capacity);
}

int DioramaLayerOrder_ResolveSection(const DioramaLayerOrderTable *table,
                                     uint8_t map_group, uint8_t map_number,
                                     uint8_t section,
                                     const DioramaResolvedLayer *defaults,
                                     int default_count,
                                     DioramaResolvedLayer *out, int capacity) {
  if (!defaults || !out || default_count <= 0 || capacity <= 0) return 0;
  int n = default_count < capacity ? default_count : capacity;

  const DioramaRoomOverride *rooms[2] = {
    DioramaLayerOrder_Find(table, map_group, map_number),
    section == kDioramaLayerSection_Room ? NULL :
        DioramaLayerOrder_FindSection(table, map_group, map_number, section),
  };
  const bool active = DioramaLayerOrder_RoomIsActive(rooms[0]) ||
                      DioramaLayerOrder_RoomIsActive(rooms[1]);

  /* Sort keys, parallel to `out`. A plane with no authored order keeps its
   * built-in slot, so it cannot drift: only authored planes move. */
  int keys[kDioramaPlane_Count > 32 ? kDioramaPlane_Count : 32];

  for (int i = 0; i < n; i++) {
    out[i] = defaults[i];
    if (out[i].alpha == 0) out[i].alpha = kDioramaLayerAlphaOpaque;
    keys[i] = i;  /* built-in slot */
    if (!active) continue;
    for (int scope = 0; scope < 2; scope++) {
      const DioramaRoomOverride *room = rooms[scope];
      if (!DioramaLayerOrder_RoomIsActive(room)) continue;
      const DioramaPlaneOverride *o = NULL;
      if (out[i].plane >= 0 && out[i].plane < kDioramaPlane_Count)
        o = &room->planes[out[i].plane];
      if (!o) continue;
      if (o->set_z) out[i].z = o->z;
      if (o->set_alpha) out[i].alpha = o->alpha;
      if (o->set_source) out[i].source = o->source;
      if (o->set_rake) out[i].rake = o->rake;
      if (o->set_bow) out[i].bow = o->bow;
      if (o->set_thickness) out[i].thickness = o->thickness;
      if (o->set_stack) out[i].stack = o->stack;
      if (o->set_stack_direction) out[i].stack_direction = o->stack_direction;
      /* Later section values refine inherited room values. A scoped `density`
       * therefore resolves against the base room's fill depth, while a scoped
       * plain `stack` intentionally replaces an inherited copy policy. */
      if (o->set_stack_copies) {
        out[i].stack_copies = o->stack_copies;
      } else if (o->set_stack_density) {
        out[i].stack_copies = DioramaLayerOrder_StackCopiesForDensity(
            out[i].stack, o->stack_density);
      } else if (o->set_stack) {
        out[i].stack_copies = kDioramaStackCopiesDefault;
      }
      if (o->set_voxel) {
        out[i].stack = o->voxel;
        out[i].stack_solid = true;
        out[i].stack_copies = o->set_voxel_copies ? o->voxel_copies
                                                  : kDioramaVoxelCopiesDefault;
        if (out[i].stack_copies > kDioramaVoxelMax)
          out[i].stack_copies = kDioramaVoxelMax;
      }
      if (o->set_order) keys[i] = o->order;
    }
  }
  if (!active) return n;

  /* Stable insertion sort on the paint key.
   *
   * Deliberately NOT a sort by z. diorama.c's default order is table order, and
   * the two disagree: Bg2Hi (z=0.21) paints after Bg1 (z=0.50). Sorting by z
   * would therefore reshuffle planes for an edit that changed nothing — an
   * earlier revision of this file did exactly that, moving five planes on a
   * no-op edit. Sorting on an explicit key that defaults to the built-in slot
   * makes an un-authored plane provably immovable.
   *
   * Stability matters for the same reason it did before: the four OBJ planes
   * share z and adjacent slots, and reshuffling them would change the sprite
   * priority interleave the defaults get right. */
  for (int i = 1; i < n; i++) {
    DioramaResolvedLayer value = out[i];
    int key = keys[i];
    int j = i - 1;
    while (j >= 0 && keys[j] > key) {
      out[j + 1] = out[j];
      keys[j + 1] = keys[j];
      j--;
    }
    out[j + 1] = value;
    keys[j + 1] = key;
  }
  return n;
}

/* ── manifest text ───────────────────────────────────────────────────── */

/* Parse one bounded float key. Returns false on a malformed or out-of-range
 * value, leaving *out untouched.
 *
 * ONE helper rather than the range test repeated per key, because the obvious
 * spelling of that test is WRONG FOR NaN: every comparison with NaN is false, so
 * `v < lo || v > hi` accepts it, and strtod parses "nan", "NaN", "-nan" and
 * "nan(0)" consuming the whole token so the trailing-character check passes too.
 * Six keys had that shape. A NaN then survives into the vertex depth, where
 * `rake == 0.0f` is false so the tilt branch is taken, and every projected
 * vertex is non-finite -- so the layer silently vanishes while the load log
 * still reports the override as applied.
 *
 * `!(v >= lo)` rather than `v < lo` is the load-bearing detail: it is true for
 * NaN, so one expression rejects both out-of-range and non-finite values.
 * (Infinities were already rejected by the range tests, being > any bound.) */
static bool ParseBoundedFloat(const char *value, double lo, double hi,
                              float *out) {
  char *end = NULL;
  double v = strtod(value, &end);
  if (end == value || (end && *end)) return false;
  if (!(v >= lo) || !(v <= hi)) return false;   /* also rejects NaN */
  *out = (float)v;
  return true;
}

static const char *SkipSpace(const char *p) {
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

/* Copy the next whitespace-delimited word into `out`. Returns the position
 * after it, or NULL when there is no word left. */
static const char *NextWord(const char *p, char *out, size_t size) {
  p = SkipSpace(p);
  if (!*p) return NULL;
  size_t n = 0;
  while (*p && *p != ' ' && *p != '\t') {
    if (n + 1 < size) out[n++] = *p;
    p++;
  }
  out[n] = '\0';
  return p;
}

bool DioramaLayerOrder_ParseSection(const char *section, uint8_t *out_group,
                                    uint8_t *out_map) {
  uint8_t scope = kDioramaLayerSection_Room;
  return DioramaLayerOrder_ParseScopedSection(
             section, out_group, out_map, &scope) &&
         scope == kDioramaLayerSection_Room;
}

bool DioramaLayerOrder_ParseScopedSection(const char *section,
                                          uint8_t *out_group,
                                          uint8_t *out_map,
                                          uint8_t *out_layer_section) {
  if (!section) return false;
  static const char kPrefix[] = "layers:";
  const size_t prefix_len = sizeof(kPrefix) - 1;
  if (strncmp(section, kPrefix, prefix_len) != 0) return false;
  const char *rest = section + prefix_len;

  char *end = NULL;
  long group = strtol(rest, &end, 16);
  if (end == rest || !end || *end != ':') return false;
  const char *map_text = end + 1;
  long map = strtol(map_text, &end, 16);
  if (end == map_text || !end) return false;
  if (group < 0 || group > 0xFF || map < 0 || map > 0xFF) return false;

  int layer_section = kDioramaLayerSection_Room;
  if (*end == ':') {
    layer_section = DioramaLayerOrder_SectionFromToken(end + 1);
    if (layer_section <= kDioramaLayerSection_Room) return false;
  } else if (*end != '\0') {
    return false;
  }

  if (out_group) *out_group = (uint8_t)group;
  if (out_map) *out_map = (uint8_t)map;
  if (out_layer_section) *out_layer_section = (uint8_t)layer_section;
  return true;
}

bool DioramaLayerOrder_ParseLine(DioramaRoomOverride *room, const char *line,
                                 const char **out_error) {
  if (out_error) *out_error = NULL;
  if (!room || !line) {
    if (out_error) *out_error = "no room";
    return false;
  }

  /* "<plane> = <key:value> [<key:value>...]" */
  const char *equals = strchr(line, '=');
  if (!equals) {
    if (out_error) *out_error = "expected 'plane = ...'";
    return false;
  }

  char token[32];
  const char *after = NextWord(line, token, sizeof(token));
  if (!after || after > equals) {
    if (out_error) *out_error = "missing plane name";
    return false;
  }
  int plane = DioramaLayerOrder_PlaneFromToken(token);
  if (plane < 0 || plane >= kDioramaPlane_Count) {
    if (out_error) *out_error = "unknown plane";
    return false;
  }

  /* Start from whatever the room already holds so a second line for the same
   * plane refines rather than clobbers. */
  DioramaPlaneOverride edit = room->planes[plane];
  bool touched = false;
  const char *p = equals + 1;
  char word[64];
  while ((p = NextWord(p, word, sizeof(word))) != NULL) {
    if (!word[0]) break;
    char *colon = strchr(word, ':');
    if (!colon) {
      if (out_error) *out_error = "expected key:value";
      return false;
    }
    *colon = '\0';
    const char *value = colon + 1;
    if (!*value) {
      if (out_error) *out_error = "empty value";
      return false;
    }
    char *end = NULL;
    if (!strcmp(word, "order")) {
      long slot = strtol(value, &end, 10);
      if (end == value || (end && *end) || slot < 0 ||
          slot >= kDioramaPlane_Count * 4) {
        if (out_error) *out_error = "bad order";
        return false;
      }
      edit.order = (int)slot;
      edit.set_order = true;
      touched = true;
    } else if (!strcmp(word, "z")) {
      /* Bounded, unlike every earlier revision of this parser, which accepted
       * any float at all. The scene places the backdrop at 0.00 and the HUD at
       * 0.95, so a z outside 0..1 is not a deep layer -- it is a layer BEHIND
       * the camera. `z:3` at the tightest legal camera pose gives a clip w of
       * -0.23, Scene3D_ProjectWorldPoint rejects every vertex, BuildLayerMesh
       * returns with no geometry, and the layer VANISHES with no diagnostic. A
       * typo in a hand-edited file therefore looked like a renderer bug.
       *
       * The range is deliberately wider than the planes the game ships (0.00 ..
       * 0.95) so authoring slightly outside the existing spread stays possible;
       * it only excludes the values that cannot project. */
      if (!ParseBoundedFloat(value, -1.0, 2.0, &edit.z)) {
        if (out_error) *out_error = "bad z (-1..2)";
        return false;
      }
      edit.set_z = true;
      touched = true;
    } else if (!strcmp(word, "alpha")) {
      long a = strtol(value, &end, 10);
      if (end == value || (end && *end) || a < 0 ||
          a > kDioramaLayerAlphaOpaque) {
        if (out_error) *out_error = "bad alpha (0-255)";
        return false;
      }
      edit.alpha = (uint8_t)a;
      edit.set_alpha = true;
      touched = true;
    } else if (!strcmp(word, "source")) {
      int source = DioramaLayerOrder_SourceFromToken(value);
      if (plane != kDioramaPlane_Backdrop ||
          !DioramaLayerOrder_SourceIsValid(source)) {
        if (out_error) *out_error =
            "bad skybox source (backdrop captured/rom-GG-MM-bgN)";
        return false;
      }
      edit.source = (uint8_t)source;
      edit.set_source = true;
      touched = true;
    } else if (!strcmp(word, "rake")) {
      /* Signed: a negative rake tilts the bottom edge AWAY, which is the right
       * shape for a ceiling. Bounded to one world unit so a typo cannot fling a
       * plane through the camera. */
      if (!ParseBoundedFloat(value, -1.0, 1.0, &edit.rake)) {
        if (out_error) *out_error = "bad rake (-1..1)";
        return false;
      }
      edit.set_rake = true;
      touched = true;
    } else if (!strcmp(word, "stack")) {
      /* Non-negative and same units as rake. Fills FORWARD (toward the camera),
       * matching thickness; a stack behind the plane would be hidden by it. */
      if (!ParseBoundedFloat(value, 0.0, 1.0, &edit.stack)) {
        if (out_error) *out_error = "bad stack (0..1)";
        return false;
      }
      edit.set_stack = true;
      touched = true;
    } else if (!strcmp(word, "bow")) {
      /* Signed like a rake, and the same range: a bow is a rake's curve, not a
       * different quantity. */
      if (!ParseBoundedFloat(value, -1.0, 1.0, &edit.bow)) {
        if (out_error) *out_error = "bad bow (-1..1)";
        return false;
      }
      edit.set_bow = true;
      touched = true;
    } else if (!strcmp(word, "voxel")) {
      if (!ParseBoundedFloat(value, 0.0, 1.0, &edit.voxel)) {
        if (out_error) *out_error = "bad voxel (0..1)";
        return false;
      }
      edit.set_voxel = true;
      touched = true;
    } else if (!strcmp(word, "slices")) {
      long slices = strtol(value, &end, 10);
      if (end == value || (end && *end) || slices < 2 ||
          slices > kDioramaVoxelMax) {
        if (out_error) *out_error = "bad slices (2..24)";
        return false;
      }
      edit.voxel_copies = (int)slices;
      edit.set_voxel_copies = true;
      touched = true;
    } else if (!strcmp(word, "density")) {
      /* Copies per unit depth. Upper bound is generous because the resolved count
       * is clamped anyway; this only rejects nonsense. */
      /* Not ParseBoundedFloat: density's lower bound is EXCLUSIVE (zero slices
       * per unit depth is not a density), and that helper's inclusive `>= lo`
       * would admit 0. The NaN case is still covered, because `!(v > 0.0)` is
       * true for NaN exactly as `!(v >= lo)` is. */
      double density = strtod(value, &end);
      if (end == value || (end && *end) || !(density > 0.0) ||
          !(density <= 1000.0)) {
        if (out_error) *out_error = "bad density (>0)";
        return false;
      }
      edit.stack_density = (float)density;
      edit.set_stack_density = true;
      touched = true;
    } else if (!strcmp(word, "dir")) {
      int dir = DioramaLayerOrder_StackDirectionFromToken(value);
      if (dir < 0) {
        if (out_error) *out_error = "bad dir (forward/backward/both)";
        return false;
      }
      edit.stack_direction = dir;
      edit.set_stack_direction = true;
      touched = true;
    } else if (!strcmp(word, "copies")) {
      long copies = strtol(value, &end, 10);
      if (end == value || (end && *end) || copies < 1 ||
          copies > kDioramaStackMax) {
        if (out_error) *out_error = "bad copies (1..8)";
        return false;
      }
      edit.stack_copies = (int)copies;
      edit.set_stack_copies = true;
      touched = true;
    } else if (!strcmp(word, "thick")) {
      /* Non-negative: a thickness extrudes the bottom edge FORWARD only (toward
       * the camera). For the other direction, author a negative rake instead --
       * they are different shapes, so this is not an arbitrary restriction. */
      if (!ParseBoundedFloat(value, 0.0, 1.0, &edit.thickness)) {
        if (out_error) *out_error = "bad thick (0..1)";
        return false;
      }
      edit.set_thickness = true;
      touched = true;
    } else {
      if (out_error) *out_error = "unknown key";
      return false;
    }
  }
  if (!touched) {
    if (out_error) *out_error = "no values";
    return false;
  }
  /* A line that authored alpha:0 means it; one that never mentioned alpha must
   * not leave a zeroed struct behind, which would render the plane invisible. */
  if (!edit.set_alpha && edit.alpha == 0)
    edit.alpha = kDioramaLayerAlphaOpaque;
  room->planes[plane] = edit;
  return true;
}

static void DioramaLayerOrder_FormatRoomBody(const DioramaRoomOverride *room,
                                             size_t *total_io, char *buffer,
                                             size_t size);

size_t DioramaLayerOrder_FormatRoom(const DioramaRoomOverride *room,
                                    char *buffer, size_t size) {
  if (!DioramaLayerOrder_RoomIsActive(room)) {
    if (buffer && size) buffer[0] = '\0';
    return 0;
  }
  size_t total = 0;
  DioramaLayerOrder_FormatRoomBody(room, &total, buffer, size);
  if (buffer && size) buffer[size - 1] = '\0';
  return total;
}

/* The body of FormatRoom, factored out so the merge can regenerate one room's
 * text into a running snprintf-style accumulator without a second copy of the
 * emit rules. Appends to `*total` and writes into `buffer`/`size` exactly as
 * FormatRoom does; caller owns the trailing NUL guarantee. */
static void DioramaLayerOrder_FormatRoomBody(const DioramaRoomOverride *room,
                                             size_t *total_io, char *buffer,
                                             size_t size) {
  size_t total = *total_io;
#define APPEND(...)                                                        \
  do {                                                                     \
    size_t remaining = (total < size) ? size - total : 0;                   \
    char *at = buffer ? buffer + (total < size ? total : size) : NULL;      \
    int wrote = snprintf(at, remaining, __VA_ARGS__);                       \
    if (wrote > 0) total += (size_t)wrote;                                  \
  } while (0)

  const char *section = DioramaLayerOrder_SectionToken(room->section);
  if (section)
    APPEND("[layers:%02X:%02X:%s]\n", room->map_group, room->map_number,
           section);
  else
    APPEND("[layers:%02X:%02X]\n", room->map_group, room->map_number);
  /* Emit in table-token order, not plane-index order, so a diff between two
   * exports is stable and readable. */
  for (int i = 0; i < kPlaneTokenCount; i++) {
    int plane = kPlaneTokens[i].plane;
    const DioramaPlaneOverride *o = &room->planes[plane];
    if (!PlaneOverrideIsAuthored(o))
      continue;
    /* Emit only the authored knobs, so a re-import reproduces exactly this
     * override rather than pinning the two the author never touched. */
    APPEND("%s =", kPlaneTokens[i].token);
    if (o->set_order) APPEND(" order:%d", o->order);
    if (o->set_z) APPEND(" z:%.4g", (double)o->z);
    if (o->set_alpha) APPEND(" alpha:%u", (unsigned)o->alpha);
    if (o->set_source)
      APPEND(" source:%s", DioramaLayerOrder_SourceToken(o->source));
    if (o->set_rake) APPEND(" rake:%.4g", (double)o->rake);
    if (o->set_bow) APPEND(" bow:%.4g", (double)o->bow);
    if (o->set_thickness) APPEND(" thick:%.4g", (double)o->thickness);
    if (o->set_stack) APPEND(" stack:%.4g", (double)o->stack);
    if (o->set_stack_copies) APPEND(" copies:%d", o->stack_copies);
    if (o->set_voxel) APPEND(" voxel:%.4g", (double)o->voxel);
    if (o->set_voxel_copies) APPEND(" slices:%d", o->voxel_copies);
    if (o->set_stack_density) APPEND(" density:%.4g", (double)o->stack_density);
    if (o->set_stack_direction)
      APPEND(" dir:%s", DioramaLayerOrder_StackDirectionToken(
                            o->stack_direction));
    APPEND("\n");
  }
#undef APPEND
  *total_io = total;
}

/* True when the line, once leading space and any trailing `;`/`#` comment are
 * removed, still has non-header content -- i.e. it is a plane-override line, the
 * kind FormatRoomBody regenerates. Blank lines and comment-only lines return
 * false: those are the user's, and the merge passes them through even inside a
 * managed section, so a comment written next to a room survives the save. */
static bool MergeLineIsPlaneBody(const char *line) {
  const char *at = line;
  while (*at == ' ' || *at == '\t') at++;
  if (*at == '\0' || *at == '\n' || *at == '\r') return false;  /* blank */
  if (*at == ';' || *at == '#') return false;                   /* comment */
  if (*at == '[') return false;                                 /* a section */
  /* Something is here before any comment marker: a plane line. */
  return true;
}

/* Is `line` (a raw file line, possibly with leading space and a trailing
 * newline) a section header "[...]"? If so and it is one of ours, report the
 * room. Mirrors the loader's own header handling in diorama.c so the merge
 * splits the file on exactly the sections the loader would recognise. */
static bool MergeLineIsSection(const char *line, bool *is_ours,
                               uint8_t *group, uint8_t *map,
                               uint8_t *section) {
  *is_ours = false;
  const char *at = line;
  while (*at == ' ' || *at == '\t') at++;
  if (*at != '[') return false;
  const char *close = strchr(at, ']');
  if (!close) return true;   /* a malformed header, but still a section line */
  char inner[64];
  size_t n = 0;
  for (const char *p = at + 1; p < close && n + 1 < sizeof(inner); p++)
    inner[n++] = *p;
  inner[n] = '\0';
  uint8_t g = 0, m = 0, s = kDioramaLayerSection_Room;
  if (DioramaLayerOrder_ParseScopedSection(inner, &g, &m, &s)) {
    *is_ours = true;
    if (group) *group = g;
    if (map) *map = m;
    if (section) *section = s;
  }
  return true;
}

size_t DioramaLayerOrder_MergeManifest(const DioramaLayerOrderTable *table,
                                       const char *existing,
                                       const char *default_preamble,
                                       char *buffer, size_t size) {
  size_t total = 0;
#define OUT(...)                                                            \
  do {                                                                      \
    size_t remaining = (total < size) ? size - total : 0;                    \
    char *at = buffer ? buffer + (total < size ? total : size) : NULL;       \
    int wrote = snprintf(at, remaining, __VA_ARGS__);                        \
    if (wrote > 0) total += (size_t)wrote;                                   \
  } while (0)
#define OUT_TEXT(str)                                                       \
  do {                                                                      \
    size_t len = strlen(str);                                               \
    if (buffer && total < size) {                                           \
      size_t room_left = size - total;                                      \
      size_t copy = len < room_left ? len : (room_left ? room_left - 1 : 0); \
      memcpy(buffer + total, (str), copy);                                  \
    }                                                                       \
    total += len;                                                           \
  } while (0)

  if (existing == NULL) existing = "";

  /* Tracks which managed rooms have been written, so an active room with no
   * section in the file can be appended once at the end. Sized to the table's
   * own capacity; a room index maps 1:1. */
  bool written[kDioramaRoomOverrideMax];
  memset(written, 0, sizeof(written));

  /* A genuinely new file gets the shipped preamble; an existing one keeps whatever
   * it already has. The test is simply "did the walk below emit anything" -- see
   * the branch after it. */
  bool wrote_any_line = false;

  const char *cursor = existing;
  bool skipping_managed_body = false;
  while (*cursor) {
    const char *newline = strchr(cursor, '\n');
    size_t line_len = newline ? (size_t)(newline - cursor) + 1
                              : strlen(cursor);
    /* Copy the line into a small scratch buffer for inspection; long lines are
     * still emitted in full, only the classification uses the prefix. */
    char probe[128];
    size_t probe_len = line_len < sizeof(probe) - 1 ? line_len : sizeof(probe) - 1;
    memcpy(probe, cursor, probe_len);
    probe[probe_len] = '\0';

    bool is_ours = false;
    uint8_t group = 0, map = 0, section = kDioramaLayerSection_Room;
    if (MergeLineIsSection(probe, &is_ours, &group, &map, &section)) {
      const DioramaRoomOverride *managed = DioramaLayerOrder_FindSection(
          table, group, map, section);
      if (is_ours && managed &&
          DioramaLayerOrder_RoomIsActive(
              managed)) {
        /* A section we manage and the table still marks active: regenerate its
         * body here, then skip the file's old body until the next section. */
        const DioramaRoomOverride *room = managed;
        /* Preserve any inline comment the user put ON the header line
         * ("[layers:01:02]  ; Fillmore act 2"). FormatRoomBody re-emits the header
         * bare, so without this the label is silently dropped -- a small loss, but
         * the same class as everything else this merge exists to prevent. Emitted
         * as its own line before the regenerated body, since the body must start
         * with the canonical header. */
        {
          const char *marker = NULL;
          for (const char *scan = probe; *scan; scan++) {
            if (*scan == ']') { marker = scan + 1; break; }
          }
          if (marker) {
            while (*marker == ' ' || *marker == '\t') marker++;
            if (*marker == ';' || *marker == '#') {
              char note[128];
              size_t note_len = 0;
              for (; marker[note_len] && marker[note_len] != '\n' &&
                     marker[note_len] != '\r' && note_len + 1 < sizeof(note);
                   note_len++)
                note[note_len] = marker[note_len];
              note[note_len] = '\0';
              if (note[0]) OUT("%s\n", note);
            }
          }
        }
        DioramaLayerOrder_FormatRoomBody(room, &total, buffer, size);
        wrote_any_line = true;
        skipping_managed_body = true;
        /* Mark it written so it is not appended again below. */
        for (int i = 0; i < table->count; i++)
          if (&table->rooms[i] == room) { written[i] = true; break; }
        cursor += line_len;
        continue;
      }
      /* A section of OURS that the table no longer marks active is a room the
       * editor RESET. Its header and comments pass through, but its plane lines
       * must be dropped -- otherwise "Reset room" does not persist: the stale
       * overrides stay in the file and the next load makes the room active again,
       * silently undoing the reset the user asked for. Emptying the section rather
       * than deleting it keeps any comments they wrote around it.
       *
       * A FOREIGN section (not ours at all) is passed through whole, bodies
       * included -- we do not own it and must not touch it. */
      if (is_ours) {
        skipping_managed_body = true;   /* drop this reset room's plane lines */
      } else {
        skipping_managed_body = false;
      }
    } else if (skipping_managed_body && MergeLineIsPlaneBody(probe)) {
      /* A plane line under a managed section we just regenerated: drop it, since
       * FormatRoomBody already emitted the current planes.
       *
       * The skip is NOT cleared by a comment or blank line, only by the next
       * SECTION header. An earlier revision cleared it on the first non-plane
       * line, which let every plane line AFTER a mid-body comment survive -- and
       * because ParseLine refines rather than clobbers, the loader then applied
       * those stale lines over the regenerated ones, silently reverting the edit.
       * A plane the user CLEARED came back. Comments and blanks still pass
       * through untouched (they fall to the verbatim copy below); the difference
       * is that they no longer end the skip. */
      cursor += line_len;
      continue;
    }

    /* Pass the line through verbatim. */
    if (buffer && total < size) {
      size_t room_left = size - total;
      size_t copy = line_len < room_left ? line_len : room_left - 1;
      memcpy(buffer + total, cursor, copy);
    }
    total += line_len;
    wrote_any_line = true;
    cursor += line_len;
  }

  /* Seed the shipped preamble only when the input is empty. An existing file,
   * even one with no managed section, must keep its own preamble unchanged. */
  if (!wrote_any_line && default_preamble && default_preamble[0]) {
    total = 0;
    OUT_TEXT(default_preamble);
    for (int i = 0; i < table->count; i++) {
      const DioramaRoomOverride *room = &table->rooms[i];
      if (!DioramaLayerOrder_RoomIsActive(room)) continue;
      DioramaLayerOrder_FormatRoomBody(room, &total, buffer, size);
      OUT("\n");
      written[i] = true;
    }
    if (buffer && size) buffer[size - 1] = '\0';
    return total;
  }

  /* Append any active room that had no section in the file. A blank line before
   * each keeps them readable against whatever preceded. */
  for (int i = 0; i < table->count; i++) {
    const DioramaRoomOverride *room = &table->rooms[i];
    if (written[i] || !DioramaLayerOrder_RoomIsActive(room)) continue;
    /* Separate from prior content with a blank line, unless the file is empty. */
    if (total > 0) OUT("\n");
    DioramaLayerOrder_FormatRoomBody(room, &total, buffer, size);
  }

#undef OUT
#undef OUT_TEXT
  if (buffer && size) buffer[size - 1] = '\0';
  return total;
}
