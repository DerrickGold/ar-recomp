#include "diorama_layer_order.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  if (!table) return NULL;
  for (int i = 0; i < table->count; i++) {
    const DioramaRoomOverride *room = &table->rooms[i];
    if (room->used && room->map_group == map_group &&
        room->map_number == map_number)
      return room;
  }
  return NULL;
}

DioramaRoomOverride *DioramaLayerOrder_FindOrAdd(
    DioramaLayerOrderTable *table, uint8_t map_group, uint8_t map_number) {
  if (!table) return NULL;
  for (int i = 0; i < table->count; i++) {
    DioramaRoomOverride *room = &table->rooms[i];
    if (room->used && room->map_group == map_group &&
        room->map_number == map_number)
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
      return room;
    }
  }
  if (table->count >= kDioramaRoomOverrideMax) return NULL;
  DioramaRoomOverride *room = &table->rooms[table->count];
  memset(room, 0, sizeof(*room));
  room->used = true;
  room->map_group = map_group;
  room->map_number = map_number;
  table->count++;
  return room;
}

bool DioramaLayerOrder_RoomIsActive(const DioramaRoomOverride *room) {
  if (!room || !room->used) return false;
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    const DioramaPlaneOverride *o = &room->planes[plane];
    if (o->set_order || o->set_z || o->set_alpha || o->set_rake || o->set_bow ||
        o->set_thickness || o->set_stack || o->set_stack_copies ||
        o->set_stack_density || o->set_stack_direction || o->set_voxel ||
        o->set_voxel_copies)
      return true;
  }
  return false;
}

void DioramaLayerOrder_ResetRoom(DioramaLayerOrderTable *table,
                                 uint8_t map_group, uint8_t map_number) {
  if (!table) return;
  for (int i = 0; i < table->count; i++) {
    DioramaRoomOverride *room = &table->rooms[i];
    if (room->used && room->map_group == map_group &&
        room->map_number == map_number) {
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
  if (!defaults || !out || default_count <= 0 || capacity <= 0) return 0;
  int n = default_count < capacity ? default_count : capacity;

  const DioramaRoomOverride *room =
      DioramaLayerOrder_Find(table, map_group, map_number);
  const bool active = DioramaLayerOrder_RoomIsActive(room);

  /* Sort keys, parallel to `out`. A plane with no authored order keeps its
   * built-in slot, so it cannot drift: only authored planes move. */
  int keys[kDioramaPlane_Count > 32 ? kDioramaPlane_Count : 32];

  for (int i = 0; i < n; i++) {
    out[i] = defaults[i];
    if (out[i].alpha == 0) out[i].alpha = kDioramaLayerAlphaOpaque;
    keys[i] = i;  /* built-in slot */
    if (!active) continue;
    const DioramaPlaneOverride *o = NULL;
    if (out[i].plane >= 0 && out[i].plane < kDioramaPlane_Count)
      o = &room->planes[out[i].plane];
    if (!o) continue;
    if (o->set_z) out[i].z = o->z;
    if (o->set_alpha) out[i].alpha = o->alpha;
    if (o->set_rake) out[i].rake = o->rake;
    if (o->set_bow) out[i].bow = o->bow;
    if (o->set_thickness) out[i].thickness = o->thickness;
    if (o->set_stack) out[i].stack = o->stack;
    if (o->set_stack_direction) out[i].stack_direction = o->stack_direction;
    /* Count precedence: an explicit `copies` is the most specific instruction and
     * wins; then a `density`, resolved against this room's own fill depth; then a
     * plain `stack:` gets a usable default rather than silently resolving to zero
     * copies, which would make the key look broken when authored alone. */
    if (o->set_stack_copies) {
      out[i].stack_copies = o->stack_copies;
    } else if (o->set_stack_density) {
      out[i].stack_copies = DioramaLayerOrder_StackCopiesForDensity(
          out[i].stack, o->stack_density);
    } else if (o->set_stack) {
      out[i].stack_copies = kDioramaStackCopiesDefault;
    }
    /* A voxel resolves onto the SAME stack fields -- the geometry is identical,
     * only the falloff and the cap differ -- plus the solid flag the renderer
     * keys the fade off. Authored after the stack keys so a room that sets both
     * gets the voxel, which is the more specific intent. */
    if (o->set_voxel) {
      out[i].stack = o->voxel;
      out[i].stack_solid = true;
      out[i].stack_copies = o->set_voxel_copies ? o->voxel_copies
                                                : kDioramaVoxelCopiesDefault;
      if (out[i].stack_copies > kDioramaVoxelMax)
        out[i].stack_copies = kDioramaVoxelMax;
    }
    /* `slices:` with no `voxel:` is deliberately inert: a count without a depth
     * is not a shape, and inventing a depth the author never asked for would be
     * worse than doing nothing. It still marks the room active, so the value
     * survives an export/re-import round trip. */
    if (o->set_order) keys[i] = o->order;
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
  if (end == map_text || !end || *end != '\0') return false;
  if (group < 0 || group > 0xFF || map < 0 || map > 0xFF) return false;

  if (out_group) *out_group = (uint8_t)group;
  if (out_map) *out_map = (uint8_t)map;
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
      double z = strtod(value, &end);
      if (end == value || (end && *end)) {
        if (out_error) *out_error = "bad z";
        return false;
      }
      edit.z = (float)z;
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
    } else if (!strcmp(word, "rake")) {
      /* Signed: a negative rake tilts the bottom edge AWAY, which is the right
       * shape for a ceiling. Bounded to one world unit so a typo cannot fling a
       * plane through the camera. */
      double rake = strtod(value, &end);
      if (end == value || (end && *end) || rake < -1.0 || rake > 1.0) {
        if (out_error) *out_error = "bad rake (-1..1)";
        return false;
      }
      edit.rake = (float)rake;
      edit.set_rake = true;
      touched = true;
    } else if (!strcmp(word, "stack")) {
      /* Non-negative and same units as rake. Fills FORWARD (toward the camera),
       * matching thickness; a stack behind the plane would be hidden by it. */
      double stack = strtod(value, &end);
      if (end == value || (end && *end) || stack < 0.0 || stack > 1.0) {
        if (out_error) *out_error = "bad stack (0..1)";
        return false;
      }
      edit.stack = (float)stack;
      edit.set_stack = true;
      touched = true;
    } else if (!strcmp(word, "bow")) {
      /* Signed like a rake, and the same range: a bow is a rake's curve, not a
       * different quantity. */
      double bow = strtod(value, &end);
      if (end == value || (end && *end) || bow < -1.0 || bow > 1.0) {
        if (out_error) *out_error = "bad bow (-1..1)";
        return false;
      }
      edit.bow = (float)bow;
      edit.set_bow = true;
      touched = true;
    } else if (!strcmp(word, "voxel")) {
      double voxel = strtod(value, &end);
      if (end == value || (end && *end) || voxel < 0.0 || voxel > 1.0) {
        if (out_error) *out_error = "bad voxel (0..1)";
        return false;
      }
      edit.voxel = (float)voxel;
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
      double density = strtod(value, &end);
      if (end == value || (end && *end) || density <= 0.0 || density > 1000.0) {
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
      double thickness = strtod(value, &end);
      if (end == value || (end && *end) || thickness < 0.0 || thickness > 1.0) {
        if (out_error) *out_error = "bad thick (0..1)";
        return false;
      }
      edit.thickness = (float)thickness;
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

size_t DioramaLayerOrder_FormatRoom(const DioramaRoomOverride *room,
                                    char *buffer, size_t size) {
  if (!DioramaLayerOrder_RoomIsActive(room)) {
    if (buffer && size) buffer[0] = '\0';
    return 0;
  }
  size_t total = 0;
  /* Local helper: append and track the would-be length like snprintf. */
#define APPEND(...)                                                        \
  do {                                                                     \
    size_t remaining = (total < size) ? size - total : 0;                   \
    char *at = buffer ? buffer + (total < size ? total : size) : NULL;      \
    int wrote = snprintf(at, remaining, __VA_ARGS__);                       \
    if (wrote > 0) total += (size_t)wrote;                                  \
  } while (0)

  APPEND("[layers:%02X:%02X]\n", room->map_group, room->map_number);
  /* Emit in table-token order, not plane-index order, so a diff between two
   * exports is stable and readable. */
  for (int i = 0; i < kPlaneTokenCount; i++) {
    int plane = kPlaneTokens[i].plane;
    const DioramaPlaneOverride *o = &room->planes[plane];
    if (!o->set_order && !o->set_z && !o->set_alpha && !o->set_rake &&
        !o->set_bow &&
        !o->set_thickness && !o->set_stack && !o->set_stack_copies &&
        !o->set_stack_density && !o->set_stack_direction && !o->set_voxel &&
        !o->set_voxel_copies)
      continue;
    /* Emit only the authored knobs, so a re-import reproduces exactly this
     * override rather than pinning the two the author never touched. */
    APPEND("%s =", kPlaneTokens[i].token);
    if (o->set_order) APPEND(" order:%d", o->order);
    if (o->set_z) APPEND(" z:%.4g", (double)o->z);
    if (o->set_alpha) APPEND(" alpha:%u", (unsigned)o->alpha);
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
  if (buffer && size) buffer[size - 1] = '\0';
  return total;
}
