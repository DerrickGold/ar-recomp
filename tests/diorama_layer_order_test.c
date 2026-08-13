/* Per-room diorama layer overrides (F3/F4 from the 2026-07-26 handback).
 *
 * The load-bearing assertions here are:
 *   1. NO-OP: with no override, Resolve returns the defaults verbatim IN ORDER.
 *      If that ever breaks, every unedited room in the game changes.
 *   2. AN ORDER-ONLY EDIT MOVES ONLY WHAT IT NAMES. Paint order is an explicit
 *      key that defaults to the plane's built-in slot -- NOT a sort by z. The
 *      two disagree (Bg2Hi z=0.21 paints after Bg1 z=0.50), so sorting by
 *      z would reshuffle planes for an edit that changed nothing.
 *   3. ORDER AND Z ARE INDEPENDENT: z feeds depth-of-field
 *      (DofRadiusForLayer), so reordering must not silently
 *      refocus a layer.
 *   4. STABILITY: planes with equal keys keep built-in order, or the four OBJ
 *      priority planes would reshuffle against each other.
 */
#include "diorama_layer_order.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

/* A stand-in for diorama.c's kDioramaLayers, in its real order and with its
 * real z values. Keeping the real numbers means a test
 * failure maps directly onto what the game would do. */
static const DioramaResolvedLayer kDefaults[] = {
  { kDioramaPlane_Backdrop, 0.00f, 255 },
  { kPpuOverlaySource_Obj,  0.51f, 255 },
  { kDioramaPlane_Obj1,     0.51f, 255 },
  { kPpuOverlaySource_Bg2,  0.20f, 255 },
  { kPpuOverlaySource_Bg1,  0.50f, 255 },
  { kDioramaPlane_Obj2,     0.51f, 255 },
  { kDioramaPlane_Bg2Hi,    0.21f, 255 },
  { kDioramaPlane_Bg1Hi,    0.51f, 255 },
  { kDioramaPlane_Obj3,     0.52f, 255 },
  { kPpuOverlaySource_Bg3,  0.95f, 255 },
};
static const int kDefaultCount =
    (int)(sizeof(kDefaults) / sizeof(kDefaults[0]));

/* THE no-op guarantee: an empty table must not perturb anything. */
static void TestNoOverrideIsIdentity(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  CHECK(n == kDefaultCount);
  for (int i = 0; i < n; i++) {
    CHECK(out[i].plane == kDefaults[i].plane);
    CHECK(out[i].z == kDefaults[i].z);
    CHECK(out[i].alpha == 255);
  }
}

/* A room authored for a DIFFERENT room must not leak. */
static void TestOverrideIsScopedToItsRoom(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  CHECK(room != NULL);
  room->planes[kPpuOverlaySource_Bg1].set_order = true;
  room->planes[kPpuOverlaySource_Bg1].order = 0;

  /* Same group, different room ($19 differs) — must be untouched. */
  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x03, kDefaults,
                                    kDefaultCount, out, 16);
  CHECK(n == kDefaultCount);
  for (int i = 0; i < n; i++) CHECK(out[i].plane == kDefaults[i].plane);
}

/* Fillmore Act 2's case: get the water plane painting in FRONT of the rock
 * path. */
static void TestOrderEditReordersPaint(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  CHECK(room != NULL);
  /* Push BG2 (built-in slot 3) past BG1 (built-in slot 4) by authoring an
   * explicit paint slot. Note this does NOT touch z, so depth-of-field is
   * unaffected -- which is the whole reason order and z are separate keys. */
  room->planes[kPpuOverlaySource_Bg2].set_order = true;
  room->planes[kPpuOverlaySource_Bg2].order = 5;

  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  CHECK(n == kDefaultCount);

  int bg2_at = -1, bg1_at = -1;
  for (int i = 0; i < n; i++) {
    if (out[i].plane == kPpuOverlaySource_Bg2) bg2_at = i;
    if (out[i].plane == kPpuOverlaySource_Bg1) bg1_at = i;
  }
  CHECK(bg2_at >= 0 && bg1_at >= 0);
  /* The whole point: BG2 now paints AFTER BG1, i.e. in front of it. */
  CHECK(bg2_at > bg1_at);
  /* z is untouched by an order-only edit: BG2 keeps its default 0.20, so its
   * DOF blur is unchanged. */
  CHECK(out[bg2_at].z == 0.20f);
}

/* Planes the override does not name must keep their built-in relative order.
 * The four OBJ planes reshuffling would change sprite priority interleave the
 * defaults already get right. */
static void TestSortIsStableForUnnamedPlanes(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x07, 0x02);
  CHECK(room != NULL);
  /* Any single edit activates the room and therefore the sort. */
  room->planes[kDioramaPlane_Backdrop].set_order = true;
  room->planes[kDioramaPlane_Backdrop].order = 0;

  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x07, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  /* Among the z==0.51 group, built-in order is Obj0, Obj1, Obj2, Bg1Hi. */
  int seen[4], k = 0;
  for (int i = 0; i < n && k < 4; i++) {
    if (out[i].z != 0.51f) continue;
    seen[k++] = out[i].plane;
  }
  CHECK(k == 4);
  CHECK(seen[0] == kPpuOverlaySource_Obj);
  CHECK(seen[1] == kDioramaPlane_Obj1);
  CHECK(seen[2] == kDioramaPlane_Obj2);
  CHECK(seen[3] == kDioramaPlane_Bg1Hi);
}

/* THE regression this design exists for. An edit that authors only z or only
 * alpha -- or authors an order equal to the built-in slot -- must leave the
 * paint sequence byte-identical. The previous revision sorted by ascending z
 * whenever a room was active, which moved FIVE planes for a no-op edit because
 * z and paint order disagree in the defaults (Bg2Hi z=0.21 paints after Bg1
 * z=0.50). */
static void TestNonOrderEditDoesNotReorder(void) {
  const struct { const char *what; int plane; bool z, alpha; } cases[] = {
    { "z only",     kPpuOverlaySource_Bg2, true,  false },
    { "alpha only", kPpuOverlaySource_Bg1, false, true  },
    { "both",       kDioramaPlane_Bg2Hi,   true,  true  },
  };
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    DioramaLayerOrderTable table;
    memset(&table, 0, sizeof(table));
    DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
    DioramaPlaneOverride *o = &room->planes[cases[c].plane];
    if (cases[c].z)     { o->set_z = true; o->z = 0.33f; }
    if (cases[c].alpha) { o->set_alpha = true; o->alpha = 200; }

    DioramaResolvedLayer out[16];
    int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                      kDefaultCount, out, 16);
    CHECK(n == kDefaultCount);
    for (int i = 0; i < n; i++) {
      if (out[i].plane == kDefaults[i].plane) continue;
      printf("FAIL %s reordered slot %d: got %s\n", cases[c].what, i,
             DioramaLayerOrder_PlaneToken(out[i].plane));
      g_failures++;
    }
  }

  /* An order authored to the plane's OWN built-in slot is also a no-op. */
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kPpuOverlaySource_Bg2].set_order = true;
  room->planes[kPpuOverlaySource_Bg2].order = 3;  /* its built-in slot */
  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  for (int i = 0; i < n; i++) CHECK(out[i].plane == kDefaults[i].plane);
}

/* Alpha must reach the output, and an un-authored plane stays opaque. */
static void TestAlphaOverride(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kPpuOverlaySource_Bg2].set_alpha = true;
  room->planes[kPpuOverlaySource_Bg2].alpha = 128;

  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  for (int i = 0; i < n; i++) {
    if (out[i].plane == kPpuOverlaySource_Bg2) CHECK(out[i].alpha == 128);
    else CHECK(out[i].alpha == 255);
  }
}

/* Reset must be a true undo: the room becomes inactive and Resolve returns to
 * the identity path, including the original table ORDER. */
static void TestResetRoomRestoresDefaults(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kPpuOverlaySource_Bg2].set_order = true;
  room->planes[kPpuOverlaySource_Bg2].order = 9;
  CHECK(DioramaLayerOrder_RoomIsActive(
      DioramaLayerOrder_Find(&table, 0x01, 0x02)));

  DioramaLayerOrder_ResetRoom(&table, 0x01, 0x02);
  CHECK(DioramaLayerOrder_Find(&table, 0x01, 0x02) == NULL);

  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  for (int i = 0; i < n; i++) CHECK(out[i].plane == kDefaults[i].plane);
}

/* A reset slot must be recycled rather than leaking table capacity. */
static void TestResetSlotIsRecycled(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *a = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x01);
  a->planes[kPpuOverlaySource_Bg1].set_order = true;
  int after_first = table.count;
  DioramaLayerOrder_ResetRoom(&table, 0x01, 0x01);
  DioramaRoomOverride *b = DioramaLayerOrder_FindOrAdd(&table, 0x02, 0x02);
  CHECK(b != NULL);
  CHECK(table.count == after_first);  /* reused, did not grow */
  CHECK(b->map_group == 0x02 && b->map_number == 0x02);
  /* And the recycled slot carries none of the old room's edits. */
  CHECK(!b->planes[kPpuOverlaySource_Bg1].set_order);
}

static void TestSectionParsing(void) {
  uint8_t group = 0, map = 0, section = 0xFF;
  CHECK(DioramaLayerOrder_ParseSection("layers:01:02", &group, &map));
  CHECK(group == 0x01 && map == 0x02);
  /* Hex, because these are the WRAM bytes as everything else prints them. */
  CHECK(DioramaLayerOrder_ParseSection("layers:07:08", &group, &map));
  CHECK(group == 0x07 && map == 0x08);
  CHECK(DioramaLayerOrder_ParseSection("layers:0A:FF", &group, &map));
  CHECK(group == 0x0A && map == 0xFF);
  /* Not ours, or malformed. */
  CHECK(!DioramaLayerOrder_ParseSection("replace:bg1", &group, &map));
  CHECK(!DioramaLayerOrder_ParseSection("layers:01", &group, &map));
  CHECK(!DioramaLayerOrder_ParseSection("layers:01:02:03", &group, &map));
  CHECK(!DioramaLayerOrder_ParseSection("layers::", &group, &map));
  CHECK(!DioramaLayerOrder_ParseSection("", &group, &map));

  CHECK(DioramaLayerOrder_ParseScopedSection(
      "layers:04:02:waterfall", &group, &map, &section));
  CHECK(group == 0x04 && map == 0x02);
  CHECK(section == kDioramaLayerSection_AitosWaterfall);
  CHECK(!DioramaLayerOrder_ParseScopedSection(
      "layers:04:02:unknown", &group, &map, &section));
  /* The legacy parser intentionally addresses only the base room. */
  CHECK(!DioramaLayerOrder_ParseSection(
      "layers:04:02:waterfall", &group, &map));
}

static void TestScopedSourceInheritsBaseRoom(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *base =
      DioramaLayerOrder_FindOrAdd(&table, 0x04, 0x02);
  DioramaRoomOverride *waterfall = DioramaLayerOrder_FindOrAddSection(
      &table, 0x04, 0x02, kDioramaLayerSection_AitosWaterfall);
  CHECK(base != NULL && waterfall != NULL);
  if (!base || !waterfall) return;

  base->planes[kPpuOverlaySource_Bg1].set_z = true;
  base->planes[kPpuOverlaySource_Bg1].z = 0.63f;
  waterfall->planes[kDioramaPlane_Backdrop].set_source = true;
  waterfall->planes[kDioramaPlane_Backdrop].source =
      kDioramaLayerSource_AitosSky;

  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_ResolveSection(
      &table, 0x04, 0x02, kDioramaLayerSection_AitosWaterfall,
      kDefaults, kDefaultCount, out, 16);
  bool saw_bg1 = false, saw_backdrop = false;
  for (int i = 0; i < n; i++) {
    if (out[i].plane == kPpuOverlaySource_Bg1) {
      saw_bg1 = true;
      CHECK(out[i].z == 0.63f);       /* inherited from base room */
    }
    if (out[i].plane == kDioramaPlane_Backdrop) {
      saw_backdrop = true;
      CHECK(out[i].source == kDioramaLayerSource_AitosSky);
    }
  }
  CHECK(saw_bg1 && saw_backdrop);

  /* Outside the positively identified section, only the base applies. */
  n = DioramaLayerOrder_Resolve(&table, 0x04, 0x02, kDefaults,
                                kDefaultCount, out, 16);
  for (int i = 0; i < n; i++)
    if (out[i].plane == kDioramaPlane_Backdrop)
      CHECK(out[i].source == kDioramaLayerSource_Captured);

  char text[256];
  CHECK(DioramaLayerOrder_FormatRoom(waterfall, text, sizeof(text)) > 0);
  CHECK(strstr(text, "[layers:04:02:waterfall]") != NULL);
  CHECK(strstr(text, "backdrop = source:rom-04-01-bg2") != NULL);
}

static void TestRomSourceCatalogue(void) {
  CHECK(DioramaLayerOrder_ActionBgSource(0x04, 0x01, 2) ==
        kDioramaLayerSource_AitosSky);
  CHECK(DioramaLayerOrder_SourceFromToken("aitos-sky") ==
        kDioramaLayerSource_AitosSky);  /* compatibility alias */
  CHECK(DioramaLayerOrder_SourceFromToken("rom-04-01-bg2") ==
        kDioramaLayerSource_AitosSky);
  CHECK(!strcmp(DioramaLayerOrder_SourceToken(
                    kDioramaLayerSource_AitosSky),
                "rom-04-01-bg2"));

  uint8_t group = 0, map = 0, bg = 0;
  const int northwall = DioramaLayerOrder_ActionBgSource(0x06, 0x08, 1);
  CHECK(DioramaLayerOrder_DecodeActionBgSource(
      northwall, &group, &map, &bg));
  CHECK(group == 0x06 && map == 0x08 && bg == 1);
  CHECK(DioramaLayerOrder_ActionBgSource(0x01, 0x05, 1) < 0);
  CHECK(DioramaLayerOrder_ActionBgSource(0x04, 0x01, 3) < 0);
  CHECK(DioramaLayerOrder_SourceFromToken("rom-01-05-bg1") < 0);

  int source = kDioramaLayerSource_Captured;
  int valid = 1;
  for (;;) {
    source = DioramaLayerOrder_NextSource(source, +1);
    if (source == kDioramaLayerSource_Captured) break;
    CHECK(DioramaLayerOrder_SourceIsValid(source));
    valid++;
    CHECK(valid < kDioramaLayerSource_Count);
  }
  CHECK(valid == (4 + 8 + 6 + 7 + 8 + 8 + 8) * 2 + 1);
  CHECK(DioramaLayerOrder_NextSource(
            kDioramaLayerSource_Captured, -1) ==
        DioramaLayerOrder_ActionBgSource(0x07, 0x08, 2));
}

static void TestLineParsing(void) {
  DioramaRoomOverride room;
  memset(&room, 0, sizeof(room));
  const char *error = NULL;

  CHECK(DioramaLayerOrder_ParseLine(&room, "bg2 = z:0.9 alpha:128", &error));
  CHECK(room.planes[kPpuOverlaySource_Bg2].set_z);
  CHECK(room.planes[kPpuOverlaySource_Bg2].set_alpha);
  CHECK(room.planes[kPpuOverlaySource_Bg2].z == 0.9f);
  CHECK(room.planes[kPpuOverlaySource_Bg2].alpha == 128);

  /* z only: alpha must default to opaque, NOT to 0 (invisible). */
  memset(&room, 0, sizeof(room));
  CHECK(DioramaLayerOrder_ParseLine(&room, "bg1 = z:0.55", &error));
  CHECK(room.planes[kPpuOverlaySource_Bg1].alpha == 255);

  /* Whitespace tolerance and the band planes. */
  memset(&room, 0, sizeof(room));
  CHECK(DioramaLayerOrder_ParseLine(&room, "  bg2hi   =   z:0.21  ", &error));
  CHECK(room.planes[kDioramaPlane_Bg2Hi].set_z);

  /* A second line for the same plane refines rather than clobbers. */
  CHECK(DioramaLayerOrder_ParseLine(&room, "bg2hi = alpha:64", &error));
  CHECK(room.planes[kDioramaPlane_Bg2Hi].z == 0.21f);
  CHECK(room.planes[kDioramaPlane_Bg2Hi].alpha == 64);

  memset(&room, 0, sizeof(room));
  CHECK(DioramaLayerOrder_ParseLine(
      &room, "backdrop = source:aitos-sky", &error));
  CHECK(room.planes[kDioramaPlane_Backdrop].set_source);
  CHECK(room.planes[kDioramaPlane_Backdrop].source ==
        kDioramaLayerSource_AitosSky);
  memset(&room, 0, sizeof(room));
  CHECK(DioramaLayerOrder_ParseLine(
      &room, "backdrop = source:rom-06-08-bg1", &error));
  CHECK(room.planes[kDioramaPlane_Backdrop].source ==
        DioramaLayerOrder_ActionBgSource(0x06, 0x08, 1));

  /* Rejections, each with a reason for the log. */
  const char *cases[] = {
    "nosuchplane = z:0.5",
    "bg1 z:0.5",            /* no '=' */
    "bg1 = ",               /* no values */
    "bg1 = z",              /* no colon */
    "bg1 = z:",             /* empty value */
    "bg1 = z:abc",
    "bg1 = alpha:999",
    "bg1 = alpha:-1",
    "bg1 = source:aitos-sky",
    "backdrop = source:unknown",
    "backdrop = source:rom-01-05-bg1", /* Fillmore has no map 5 */
    "backdrop = source:rom-04-01-bg3",
    "bg1 = colour:red",
    "= z:0.5",              /* no plane */
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    DioramaRoomOverride tmp;
    memset(&tmp, 0, sizeof(tmp));
    error = NULL;
    if (DioramaLayerOrder_ParseLine(&tmp, cases[i], &error)) {
      printf("FAIL accepted bad line: %s\n", cases[i]);
      g_failures++;
    } else if (!error) {
      printf("FAIL rejected without a reason: %s\n", cases[i]);
      g_failures++;
    }
  }
}

/* Export then re-import must round-trip: that is what makes the authored
 * manifest reusable rather than a one-off. */
static void TestFormatRoundTrips(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kPpuOverlaySource_Bg2].set_order = true;
  room->planes[kPpuOverlaySource_Bg2].order = 5;
  room->planes[kPpuOverlaySource_Bg2].set_z = true;
  room->planes[kPpuOverlaySource_Bg2].z = 0.875f;
  room->planes[kPpuOverlaySource_Bg2].set_alpha = true;
  room->planes[kPpuOverlaySource_Bg2].alpha = 128;
  room->planes[kPpuOverlaySource_Bg1].set_z = true;
  room->planes[kPpuOverlaySource_Bg1].z = 0.5f;

  char text[512];
  size_t need = DioramaLayerOrder_FormatRoom(room, text, sizeof(text));
  CHECK(need > 0 && need < sizeof(text));
  CHECK(strstr(text, "[layers:01:02]") != NULL);
  CHECK(strstr(text, "bg1 = z:0.5\n") != NULL);   /* only the authored knob */
  CHECK(strstr(text, "bg2 = order:5 z:0.875 alpha:128") != NULL);

  /* Feed it back through the parsers. */
  DioramaRoomOverride reparsed;
  memset(&reparsed, 0, sizeof(reparsed));
  uint8_t group = 0, map = 0;
  char *save = NULL;
  char copy[512];
  snprintf(copy, sizeof(copy), "%s", text);
  for (char *line = strtok_r(copy, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    if (line[0] == '[') {
      char section[64];
      snprintf(section, sizeof(section), "%s", line + 1);
      char *close = strchr(section, ']');
      if (close) *close = '\0';
      CHECK(DioramaLayerOrder_ParseSection(section, &group, &map));
      continue;
    }
    const char *error = NULL;
    CHECK(DioramaLayerOrder_ParseLine(&reparsed, line, &error));
  }
  CHECK(group == 0x01 && map == 0x02);
  CHECK(reparsed.planes[kPpuOverlaySource_Bg2].order == 5);
  CHECK(reparsed.planes[kPpuOverlaySource_Bg2].z == 0.875f);
  CHECK(reparsed.planes[kPpuOverlaySource_Bg2].alpha == 128);
  CHECK(reparsed.planes[kPpuOverlaySource_Bg1].set_z);
  CHECK(reparsed.planes[kPpuOverlaySource_Bg1].z == 0.5f);
  /* BG1 never authored order or alpha, so the re-import must not invent them. */
  CHECK(!reparsed.planes[kPpuOverlaySource_Bg1].set_order);
  CHECK(!reparsed.planes[kPpuOverlaySource_Bg1].set_alpha);
}

/* An inactive room emits nothing, so a manifest never gains empty sections. */
/* The rake is what closes the visible void between two parallel planes once the
 * diorama camera tilts (Fillmore act 2: water at Bg2Hi z=0.21 floating behind a
 * gap, rock path at Bg1 z=0.50). Thickness ships parsed-but-inert alongside it. */
static void TestRakeAndThicknessResolve(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kPpuOverlaySource_Bg2].set_rake = true;
  room->planes[kPpuOverlaySource_Bg2].rake = 0.29f;
  room->planes[kPpuOverlaySource_Bg2].set_thickness = true;
  room->planes[kPpuOverlaySource_Bg2].thickness = 0.10f;

  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  CHECK(n == kDefaultCount);
  for (int i = 0; i < n; i++) {
    if (out[i].plane == kPpuOverlaySource_Bg2) {
      CHECK(out[i].rake == 0.29f);
      CHECK(out[i].thickness == 0.10f);
    } else {
      /* Every other plane stays parallel — a rake is per-plane, never global. */
      CHECK(out[i].rake == 0.0f);
      CHECK(out[i].thickness == 0.0f);
    }
  }
  /* A rake alone must not reorder anything: paint order is keyed on `order`. */
  for (int i = 0; i < n; i++)
    CHECK(out[i].plane == kDefaults[i].plane);
}

/* STACK: a third shape for the same void. The reason it exists is that a rake
 * TILTS the plane, which puts one layer's own rows at different depths -- so it
 * picks up two parallax rates within itself and shears as the camera moves. A
 * stack repeats the layer at parallel depths instead, so each copy has a single
 * depth and the layer keeps one parallax rate. */
static void TestStackResolve(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kDioramaPlane_Bg2Hi].set_stack = true;
  room->planes[kDioramaPlane_Bg2Hi].stack = 0.29f;

  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  CHECK(n == kDefaultCount);
  for (int i = 0; i < n; i++) {
    if (out[i].plane == kDioramaPlane_Bg2Hi) {
      CHECK(out[i].stack == 0.29f);
      /* A stack depth with NO explicit count must resolve to a usable default,
       * or `stack:` alone silently draws nothing and looks broken. */
      CHECK(out[i].stack_copies == kDioramaStackCopiesDefault);
      CHECK(out[i].stack_copies > 1);
    } else {
      CHECK(out[i].stack == 0.0f);
    }
  }
  /* A stack alone must not reorder: paint order is keyed on `order` only. */
  for (int i = 0; i < n; i++) CHECK(out[i].plane == kDefaults[i].plane);
  /* Nor tilt: stack and rake are independent shapes. */
  for (int i = 0; i < n; i++) CHECK(out[i].rake == 0.0f);

  /* An explicit count wins over the default. */
  room->planes[kDioramaPlane_Bg2Hi].set_stack_copies = true;
  room->planes[kDioramaPlane_Bg2Hi].stack_copies = 6;
  n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults, kDefaultCount,
                                out, 16);
  for (int i = 0; i < n; i++)
    if (out[i].plane == kDioramaPlane_Bg2Hi) CHECK(out[i].stack_copies == 6);
}

/* A stack-only room is a real override: "Reset room" must have something to undo
 * and the exporter must emit it, or an authored stack vanishes on save. */
static void TestStackParseAndRoundTrip(void) {
  DioramaRoomOverride room;
  memset(&room, 0, sizeof(room));
  const char *error = NULL;

  CHECK(DioramaLayerOrder_ParseLine(&room, "bg2hi = stack:0.29 copies:4",
                                    &error));
  CHECK(room.planes[kDioramaPlane_Bg2Hi].set_stack);
  CHECK(room.planes[kDioramaPlane_Bg2Hi].stack == 0.29f);
  CHECK(room.planes[kDioramaPlane_Bg2Hi].set_stack_copies);
  CHECK(room.planes[kDioramaPlane_Bg2Hi].stack_copies == 4);
  room.used = true; room.map_group = 0x01; room.map_number = 0x02;
  CHECK(DioramaLayerOrder_RoomIsActive(&room));

  /* Bounds. copies must be >= 1 (zero copies is not a shape) and <= the cap,
   * since every copy is another full-layer draw call. */
  const char *bad[] = {
    "bg2hi = stack:-0.1", "bg2hi = stack:9", "bg2hi = stack:abc",
    "bg2hi = copies:0",   "bg2hi = copies:9", "bg2hi = copies:2.5",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    DioramaRoomOverride tmp;
    memset(&tmp, 0, sizeof(tmp));
    error = NULL;
    if (DioramaLayerOrder_ParseLine(&tmp, bad[i], &error)) {
      printf("FAIL accepted bad stack line: %s\n", bad[i]);
      g_failures++;
    } else if (!error) {
      printf("FAIL rejected without a reason: %s\n", bad[i]);
      g_failures++;
    }
  }

  /* Export then re-import losslessly, so an authored stack survives a save. */
  char text[512];
  size_t need = DioramaLayerOrder_FormatRoom(&room, text, sizeof(text));
  CHECK(need > 0 && need < sizeof(text));
  CHECK(strstr(text, "stack:0.29") != NULL);
  CHECK(strstr(text, "copies:4") != NULL);
  DioramaRoomOverride reparsed;
  memset(&reparsed, 0, sizeof(reparsed));
  error = NULL;
  CHECK(DioramaLayerOrder_ParseLine(&reparsed, "bg2hi = stack:0.29 copies:4",
                                    &error));
  CHECK(reparsed.planes[kDioramaPlane_Bg2Hi].stack == 0.29f);
  CHECK(reparsed.planes[kDioramaPlane_Bg2Hi].stack_copies == 4);
  /* And it must not invent the OTHER shapes it never authored. */
  CHECK(!reparsed.planes[kDioramaPlane_Bg2Hi].set_rake);
  CHECK(!reparsed.planes[kDioramaPlane_Bg2Hi].set_thickness);
}

/* DENSITY resolves to a count against the room's own fill depth. Density rather
 * than a bare count because slice SPACING is what the eye judges, and a fixed
 * count spaces slices differently in every room: copies:4 over a 0.29 gap spaces
 * them 0.097 apart, over a 0.10 gap only 0.033. */
static void TestStackDensityAndDirection(void) {
  /* Count grows with the gap at a fixed density, which is the whole point. */
  int wide = DioramaLayerOrder_StackCopiesForDensity(0.60f, 14.0f);
  int narrow = DioramaLayerOrder_StackCopiesForDensity(0.10f, 14.0f);
  CHECK(wide > narrow);
  /* Spacing stays roughly constant across those two, unlike a fixed count. */
  float space_wide = 0.60f / (float)(wide - 1);
  float space_narrow = 0.10f / (float)(narrow - 1);
  CHECK(space_wide < space_narrow * 3.0f);

  /* EXACT counts, computed by hand rather than by calling the function under
   * test. N slices span N-1 intervals, so a density of D over a depth of P wants
   * round(P*D) intervals and therefore round(P*D)+1 planes. Asserting only
   * relative properties here let an off-by-one survive: without the +1 a 0.29
   * fill at density 14 gives 4 slices instead of 5. */
  CHECK(DioramaLayerOrder_StackCopiesForDensity(0.29f, 14.0f) == 5);  /* 4.06 -> 4+1 */
  CHECK(DioramaLayerOrder_StackCopiesForDensity(0.20f, 10.0f) == 3);  /* 2.00 -> 2+1 */
  CHECK(DioramaLayerOrder_StackCopiesForDensity(0.50f, 6.0f) == 4);   /* 3.00 -> 3+1 */
  /* A fractional interval count must ROUND, not truncate: 0.30 x 9 = 2.7 intervals
   * is nearer 3 than 2, so 4 slices. Truncating would give 3 and quietly space
   * them wider than the author asked for. The cases above are all whole or
   * near-whole, so they cannot tell the two apart. */
  CHECK(DioramaLayerOrder_StackCopiesForDensity(0.30f, 9.0f) == 4);   /* 2.70 -> 3+1 */
  CHECK(DioramaLayerOrder_StackCopiesForDensity(0.13f, 20.0f) == 4);  /* 2.60 -> 3+1 */
  /* Spacing therefore lands on the authored density's reciprocal: 0.20 over
   * 3 slices = 2 intervals of 0.10, which is 1/density. */
  {
    int c = DioramaLayerOrder_StackCopiesForDensity(0.20f, 10.0f);
    float spacing = 0.20f / (float)(c - 1);
    CHECK(spacing > 0.099f && spacing < 0.101f);
  }

  /* Never 1 for a real fill: one copy is just the plane again, so a density that
   * rounded down to 1 would silently disable the stack it was asked for. */
  CHECK(DioramaLayerOrder_StackCopiesForDensity(0.01f, 1.0f) >= 2);
  /* Clamped to the cap, since every copy is another full-layer draw call. */
  CHECK(DioramaLayerOrder_StackCopiesForDensity(1.0f, 1000.0f) ==
        kDioramaStackMax);
  /* No fill or no density means no stack. */
  CHECK(DioramaLayerOrder_StackCopiesForDensity(0.0f, 14.0f) == 1);
  CHECK(DioramaLayerOrder_StackCopiesForDensity(0.29f, 0.0f) == 1);
  CHECK(DioramaLayerOrder_StackCopiesForDensity(-1.0f, 14.0f) == 1);

  /* Resolve: a density authored with a stack yields that count. */
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kDioramaPlane_Bg2Hi].set_stack = true;
  room->planes[kDioramaPlane_Bg2Hi].stack = 0.29f;
  room->planes[kDioramaPlane_Bg2Hi].set_stack_density = true;
  room->planes[kDioramaPlane_Bg2Hi].stack_density = 14.0f;
  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  /* Hand-computed, NOT via the function under test -- otherwise this assertion
   * holds for any implementation, including a wrong one. */
  for (int i = 0; i < n; i++)
    if (out[i].plane == kDioramaPlane_Bg2Hi)
      CHECK(out[i].stack_copies == 5);

  /* An explicit count is the more specific instruction and must WIN over a
   * density authored alongside it. */
  room->planes[kDioramaPlane_Bg2Hi].set_stack_copies = true;
  room->planes[kDioramaPlane_Bg2Hi].stack_copies = 2;
  n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults, kDefaultCount,
                                out, 16);
  for (int i = 0; i < n; i++)
    if (out[i].plane == kDioramaPlane_Bg2Hi) CHECK(out[i].stack_copies == 2);

  /* Direction tokens round-trip, and default to forward. */
  CHECK(DioramaLayerOrder_StackDirectionFromToken("forward") ==
        kDioramaStack_Forward);
  CHECK(DioramaLayerOrder_StackDirectionFromToken("backward") ==
        kDioramaStack_Backward);
  CHECK(DioramaLayerOrder_StackDirectionFromToken("both") == kDioramaStack_Both);
  CHECK(DioramaLayerOrder_StackDirectionFromToken("sideways") == -1);
  CHECK(DioramaLayerOrder_StackDirectionFromToken(NULL) == -1);
  for (int d = 0; d < kDioramaStack_DirectionCount; d++)
    CHECK(DioramaLayerOrder_StackDirectionFromToken(
              DioramaLayerOrder_StackDirectionToken(d)) == d);
  /* An unresolved plane keeps forward, so an unauthored room is unchanged. */
  for (int i = 0; i < n; i++)
    if (out[i].plane != kDioramaPlane_Bg2Hi)
      CHECK(out[i].stack_direction == kDioramaStack_Forward);

  /* Parse + export both keys. */
  DioramaRoomOverride parsed;
  memset(&parsed, 0, sizeof(parsed));
  const char *error = NULL;
  CHECK(DioramaLayerOrder_ParseLine(
      &parsed, "bg2hi = stack:0.29 density:14 dir:both", &error));
  CHECK(parsed.planes[kDioramaPlane_Bg2Hi].stack_density == 14.0f);
  CHECK(parsed.planes[kDioramaPlane_Bg2Hi].stack_direction ==
        kDioramaStack_Both);
  parsed.used = true; parsed.map_group = 0x01; parsed.map_number = 0x02;
  CHECK(DioramaLayerOrder_RoomIsActive(&parsed));
  char text[512];
  CHECK(DioramaLayerOrder_FormatRoom(&parsed, text, sizeof(text)) > 0);
  CHECK(strstr(text, "density:14") != NULL);
  CHECK(strstr(text, "dir:both") != NULL);

  const char *bad[] = {
    "bg2hi = density:0", "bg2hi = density:-3", "bg2hi = density:abc",
    "bg2hi = dir:sideways", "bg2hi = dir:", "bg2hi = dir:1",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    DioramaRoomOverride tmp;
    memset(&tmp, 0, sizeof(tmp));
    error = NULL;
    if (DioramaLayerOrder_ParseLine(&tmp, bad[i], &error)) {
      printf("FAIL accepted bad line: %s\n", bad[i]);
      g_failures++;
    }
  }
}

/* VOXEL resolves onto the stack fields plus the solid flag, with its own higher
 * cap: solidity is a function of how close consecutive slices land, so it needs
 * many more than a stack whose job is to read as distinguishable layers. */
static void TestVoxelResolve(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kPpuOverlaySource_Bg1].set_voxel = true;
  room->planes[kPpuOverlaySource_Bg1].voxel = 0.20f;

  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  for (int i = 0; i < n; i++) {
    if (out[i].plane == kPpuOverlaySource_Bg1) {
      CHECK(out[i].stack == 0.20f);        /* shares the stack's depth field */
      CHECK(out[i].stack_solid);
      /* A voxel default must be dense enough to read as solid, so it is much
       * higher than the stack default -- otherwise `voxel:` alone looks striped. */
      CHECK(out[i].stack_copies == kDioramaVoxelCopiesDefault);
      CHECK(out[i].stack_copies > kDioramaStackCopiesDefault);
    } else {
      CHECK(!out[i].stack_solid);          /* per-plane, never global */
      CHECK(out[i].stack == 0.0f);
    }
  }
  /* Neither reorders nor tilts. */
  for (int i = 0; i < n; i++) {
    CHECK(out[i].plane == kDefaults[i].plane);
    CHECK(out[i].rake == 0.0f);
  }

  /* An explicit slice count wins, and is clamped to the VOXEL cap (not the
   * stack's), since that is the budget the author opted into. */
  room->planes[kPpuOverlaySource_Bg1].set_voxel_copies = true;
  room->planes[kPpuOverlaySource_Bg1].voxel_copies = kDioramaVoxelMax;
  n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults, kDefaultCount,
                                out, 16);
  for (int i = 0; i < n; i++)
    if (out[i].plane == kPpuOverlaySource_Bg1)
      CHECK(out[i].stack_copies == kDioramaVoxelMax);
  CHECK(kDioramaVoxelMax > kDioramaStackMax);

  /* The resolve-side clamp is defence in depth: the PARSER already rejects
   * slices > kDioramaVoxelMax, so a manifest cannot reach this -- but the editor
   * and any future caller set the struct field directly, and an unclamped count
   * blows the per-frame draw budget. Set it out of range on purpose. */
  room->planes[kPpuOverlaySource_Bg1].voxel_copies = kDioramaVoxelMax + 50;
  n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults, kDefaultCount,
                                out, 16);
  for (int i = 0; i < n; i++)
    if (out[i].plane == kPpuOverlaySource_Bg1)
      CHECK(out[i].stack_copies == kDioramaVoxelMax);

  /* A voxel authored alongside a stack WINS -- it is the more specific intent,
   * and silently blending the two would give neither. */
  memset(&table, 0, sizeof(table));
  room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kPpuOverlaySource_Bg1].set_stack = true;
  room->planes[kPpuOverlaySource_Bg1].stack = 0.50f;
  room->planes[kPpuOverlaySource_Bg1].set_voxel = true;
  room->planes[kPpuOverlaySource_Bg1].voxel = 0.20f;
  n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults, kDefaultCount,
                                out, 16);
  for (int i = 0; i < n; i++)
    if (out[i].plane == kPpuOverlaySource_Bg1) {
      CHECK(out[i].stack == 0.20f);
      CHECK(out[i].stack_solid);
    }

  /* Parse, bounds, activity, round-trip. */
  DioramaRoomOverride parsed;
  memset(&parsed, 0, sizeof(parsed));
  const char *error = NULL;
  CHECK(DioramaLayerOrder_ParseLine(&parsed, "bg1 = voxel:0.20 slices:16",
                                    &error));
  CHECK(parsed.planes[kPpuOverlaySource_Bg1].voxel == 0.20f);
  CHECK(parsed.planes[kPpuOverlaySource_Bg1].voxel_copies == 16);
  parsed.used = true; parsed.map_group = 0x01; parsed.map_number = 0x02;
  CHECK(DioramaLayerOrder_RoomIsActive(&parsed));
  char text[512];
  CHECK(DioramaLayerOrder_FormatRoom(&parsed, text, sizeof(text)) > 0);
  CHECK(strstr(text, "voxel:0.2") != NULL);
  CHECK(strstr(text, "slices:16") != NULL);

  const char *bad[] = {
    "bg1 = voxel:-0.1", "bg1 = voxel:9", "bg1 = voxel:abc",
    "bg1 = slices:1",   /* one slice is not an extrusion */
    "bg1 = slices:25",  /* past the voxel cap */
    "bg1 = slices:abc",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    DioramaRoomOverride tmp;
    memset(&tmp, 0, sizeof(tmp));
    error = NULL;
    if (DioramaLayerOrder_ParseLine(&tmp, bad[i], &error)) {
      printf("FAIL accepted bad voxel line: %s\n", bad[i]);
      g_failures++;
    }
  }

  /* `slices:` with no `voxel:` is inert rather than inventing a depth -- but it
   * still marks the room active so the value survives a round trip. */
  DioramaLayerOrderTable lone;
  memset(&lone, 0, sizeof(lone));
  DioramaRoomOverride *r2 = DioramaLayerOrder_FindOrAdd(&lone, 0x02, 0x01);
  r2->planes[kPpuOverlaySource_Bg1].set_voxel_copies = true;
  r2->planes[kPpuOverlaySource_Bg1].voxel_copies = 16;
  CHECK(DioramaLayerOrder_RoomIsActive(r2));
  n = DioramaLayerOrder_Resolve(&lone, 0x02, 0x01, kDefaults, kDefaultCount,
                                out, 16);
  for (int i = 0; i < n; i++) {
    CHECK(!out[i].stack_solid);
    CHECK(out[i].stack == 0.0f);
  }
}

/* The STRATEGY enum is what the layer editor will cycle a plane through, so it has
 * to name exactly one strategy per plane even though the manifest lets a plane
 * carry several keys at once. */
static void TestStrategyOf(void) {
  DioramaResolvedLayer l;

  /* An unauthored plane is Flat -- which is every plane in every shipped room, so
   * getting this wrong mislabels the entire game. */
  memset(&l, 0, sizeof(l));
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Flat);
  CHECK(DioramaLayerOrder_StrategyOf(NULL) == kDioramaDepth_Flat);

  memset(&l, 0, sizeof(l));
  l.rake = 0.29f;
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Rake);

  memset(&l, 0, sizeof(l));
  l.bow = 0.29f;
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Bow);

  memset(&l, 0, sizeof(l));
  l.thickness = 0.20f;
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Thick);

  memset(&l, 0, sizeof(l));
  l.stack = 0.29f; l.stack_copies = 4;
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Stack);

  memset(&l, 0, sizeof(l));
  l.stack = 0.20f; l.stack_copies = 12; l.stack_solid = true;
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Voxel);

  /* PRECEDENCE, when a plane carries several. Most specific wins, matching the
   * order the renderer applies them -- otherwise the editor's label would
   * disagree with what is on screen. */
  memset(&l, 0, sizeof(l));
  l.rake = 0.29f; l.bow = 0.10f;
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Bow);
  l.thickness = 0.20f;
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Thick);
  l.stack = 0.29f; l.stack_copies = 4;
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Stack);
  l.stack_solid = true;
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Voxel);

  /* A stack depth with only ONE copy is not a stack -- the single copy is the
   * plane itself -- so it must not be labelled one. */
  memset(&l, 0, sizeof(l));
  l.stack = 0.29f; l.stack_copies = 1;
  CHECK(DioramaLayerOrder_StrategyOf(&l) == kDioramaDepth_Flat);

  /* Every strategy has a distinct, non-empty name for the editor row. */
  for (int a = 0; a < kDioramaDepth_StrategyCount; a++) {
    const char *na = DioramaLayerOrder_StrategyName(a);
    CHECK(na && na[0]);
    for (int b = a + 1; b < kDioramaDepth_StrategyCount; b++)
      CHECK(strcmp(na, DioramaLayerOrder_StrategyName(b)) != 0);
  }
  /* Out of range never returns NULL, since the editor prints it unconditionally. */
  CHECK(DioramaLayerOrder_StrategyName(-1) != NULL);
  CHECK(DioramaLayerOrder_StrategyName(kDioramaDepth_StrategyCount) != NULL);
  /* Flat is 0 so a zeroed struct means "no depth", and the cheapest strategies
   * come first so cycling forward escalates cost. */
  CHECK(kDioramaDepth_Flat == 0);
  CHECK(kDioramaDepth_Voxel == kDioramaDepth_StrategyCount - 1);
}

/* BOW resolves and round-trips like a rake, being the same quantity curved. */
static void TestBowResolveAndRoundTrip(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kDioramaPlane_Bg2Hi].set_bow = true;
  room->planes[kDioramaPlane_Bg2Hi].bow = 0.29f;

  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  for (int i = 0; i < n; i++) {
    if (out[i].plane == kDioramaPlane_Bg2Hi) {
      CHECK(out[i].bow == 0.29f);
      CHECK(out[i].rake == 0.0f);   /* independent of the linear tilt */
      CHECK(DioramaLayerOrder_StrategyOf(&out[i]) == kDioramaDepth_Bow);
    } else {
      CHECK(out[i].bow == 0.0f);
    }
  }
  /* A bow alone must not reorder. */
  for (int i = 0; i < n; i++) CHECK(out[i].plane == kDefaults[i].plane);

  DioramaRoomOverride parsed;
  memset(&parsed, 0, sizeof(parsed));
  const char *error = NULL;
  CHECK(DioramaLayerOrder_ParseLine(&parsed, "bg2hi = bow:0.29", &error));
  CHECK(parsed.planes[kDioramaPlane_Bg2Hi].bow == 0.29f);
  CHECK(DioramaLayerOrder_ParseLine(&parsed, "bg1 = rake:0.1 bow:0.2", &error));
  CHECK(parsed.planes[kPpuOverlaySource_Bg1].rake == 0.1f);
  CHECK(parsed.planes[kPpuOverlaySource_Bg1].bow == 0.2f);
  parsed.used = true; parsed.map_group = 0x01; parsed.map_number = 0x02;
  CHECK(DioramaLayerOrder_RoomIsActive(&parsed));
  char text[512];
  CHECK(DioramaLayerOrder_FormatRoom(&parsed, text, sizeof(text)) > 0);
  CHECK(strstr(text, "bow:0.29") != NULL);

  const char *bad[] = { "bg2hi = bow:2", "bg2hi = bow:-2", "bg2hi = bow:abc" };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    DioramaRoomOverride tmp;
    memset(&tmp, 0, sizeof(tmp));
    error = NULL;
    if (DioramaLayerOrder_ParseLine(&tmp, bad[i], &error)) {
      printf("FAIL accepted bad bow line: %s\n", bad[i]);
      g_failures++;
    }
  }
}

/* A rake-only room is a real override, so "Reset room" has something to undo and
 * the exporter must emit it. Getting RoomIsActive wrong here would make an
 * authored rake vanish on save. */
static void TestRakeOnlyRoomIsActiveAndRoundTrips(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kDioramaPlane_Bg2Hi].set_rake = true;
  room->planes[kDioramaPlane_Bg2Hi].rake = -0.25f;
  CHECK(DioramaLayerOrder_RoomIsActive(room));

  char text[512];
  size_t need = DioramaLayerOrder_FormatRoom(room, text, sizeof(text));
  CHECK(need > 0 && need < sizeof(text));
  CHECK(strstr(text, "bg2hi = rake:-0.25\n") != NULL);

  DioramaRoomOverride reparsed;
  memset(&reparsed, 0, sizeof(reparsed));
  const char *error = NULL;
  CHECK(DioramaLayerOrder_ParseLine(&reparsed, "bg2hi = rake:-0.25", &error));
  CHECK(error == NULL);
  CHECK(reparsed.planes[kDioramaPlane_Bg2Hi].set_rake);
  CHECK(reparsed.planes[kDioramaPlane_Bg2Hi].rake == -0.25f);
  /* Parsing a rake must not imply the other knobs. */
  CHECK(!reparsed.planes[kDioramaPlane_Bg2Hi].set_z);
  CHECK(!reparsed.planes[kDioramaPlane_Bg2Hi].set_order);
  CHECK(!reparsed.planes[kDioramaPlane_Bg2Hi].set_thickness);

  /* Both knobs together, and the reserved one round-trips even though nothing
   * consumes it yet — otherwise authored files would silently lose it. */
  memset(&reparsed, 0, sizeof(reparsed));
  CHECK(DioramaLayerOrder_ParseLine(&reparsed, "bg2 = rake:0.3 thick:0.125",
                                    &error));
  CHECK(reparsed.planes[kPpuOverlaySource_Bg2].rake == 0.3f);
  CHECK(reparsed.planes[kPpuOverlaySource_Bg2].thickness == 0.125f);
}

static void TestRakeAndThicknessRejectBadValues(void) {
  DioramaRoomOverride room;
  memset(&room, 0, sizeof(room));
  const char *error = NULL;
  /* Out of range in both directions — a typo must not fling a plane through
   * the camera. */
  CHECK(!DioramaLayerOrder_ParseLine(&room, "bg2 = rake:2.5", &error));
  CHECK(error != NULL);
  CHECK(!DioramaLayerOrder_ParseLine(&room, "bg2 = rake:-2.5", &error));
  /* Thickness extrudes forward only; negative is a rake's job. */
  CHECK(!DioramaLayerOrder_ParseLine(&room, "bg2 = thick:-0.1", &error));
  CHECK(!DioramaLayerOrder_ParseLine(&room, "bg2 = thick:9", &error));
  CHECK(!DioramaLayerOrder_ParseLine(&room, "bg2 = rake:abc", &error));
  /* A rejected line leaves nothing behind. */
  CHECK(!room.planes[kPpuOverlaySource_Bg2].set_rake);
  CHECK(!room.planes[kPpuOverlaySource_Bg2].set_thickness);
}

/* NON-FINITE values, which every earlier version of this parser ACCEPTED.
 *
 * This test exists because the one above passed the whole time: it only ever
 * tried finite out-of-range values, and the natural spelling of a range check --
 * `v < lo || v > hi` -- is FALSE for NaN, since every comparison with NaN is.
 * strtod also parses "nan"/"NaN"/"-nan"/"nan(0)" consuming the whole token, so
 * the trailing-character check passed too. Six float keys had that shape.
 *
 * What it cost: a NaN survives into the vertex depth (`rake == 0.0f` is false,
 * so the tilt branch runs), every projected vertex is non-finite, and the layer
 * VANISHES -- while the load log still counts the override as applied. A typo
 * therefore presented as a renderer bug with no diagnostic pointing at the file.
 *
 * Infinities were already rejected, being greater than any bound; they are
 * asserted anyway so a future refactor cannot lose that for free. */
static void TestNonFiniteValuesAreRejected(void) {
  static const char *const kSpellings[] = {
    "nan", "NaN", "-nan", "nan(0)", "inf", "-inf", "INF",
  };
  /* Every float key in the grammar. If a key is added without a bound, adding it
   * here is what catches it. */
  static const char *const kKeys[] = {
    "z", "rake", "bow", "thick", "stack", "voxel", "density",
  };
  for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); k++) {
    for (size_t s = 0; s < sizeof(kSpellings) / sizeof(kSpellings[0]); s++) {
      DioramaRoomOverride room;
      memset(&room, 0, sizeof(room));
      char line[64];
      snprintf(line, sizeof(line), "bg2 = %s:%s", kKeys[k], kSpellings[s]);
      const char *error = NULL;
      CHECK(!DioramaLayerOrder_ParseLine(&room, line, &error));
      CHECK(error != NULL);
      /* And the room stays untouched, so a bad line cannot make an otherwise
       * unauthored room active. */
      CHECK(!DioramaLayerOrder_RoomIsActive(&room));
    }
  }
}

/* `z` is now bounded. It was the one float key with NO range check at all, and
 * it is the REACHABLE version of the "plane behind the camera" hazard: `z:3` in
 * a hand-edited manifest puts the plane past the near plane at the tightest
 * legal camera pose, every vertex fails projection, and the layer disappears
 * silently. No editor or in-memory writer needed. */
static void TestZIsBounded(void) {
  DioramaRoomOverride room;
  memset(&room, 0, sizeof(room));
  const char *error = NULL;
  CHECK(!DioramaLayerOrder_ParseLine(&room, "bg2 = z:3", &error));
  CHECK(error != NULL);
  CHECK(!DioramaLayerOrder_ParseLine(&room, "bg2 = z:-99", &error));
  CHECK(!DioramaLayerOrder_RoomIsActive(&room));

  /* The range stays wider than the planes the game ships (0.00 .. 0.95) so a
   * room can still author just outside the existing spread. */
  CHECK(DioramaLayerOrder_ParseLine(&room, "bg2 = z:0", &error));
  CHECK(DioramaLayerOrder_ParseLine(&room, "bg2 = z:0.95", &error));
  CHECK(DioramaLayerOrder_ParseLine(&room, "bg2 = z:1.5", &error));
  CHECK(DioramaLayerOrder_ParseLine(&room, "bg2 = z:-0.5", &error));
  CHECK(room.planes[kPpuOverlaySource_Bg2].set_z);
}

static void TestInactiveRoomEmitsNothing(void) {
  DioramaRoomOverride room;
  memset(&room, 0, sizeof(room));
  room.used = true;
  room.map_group = 0x01;
  room.map_number = 0x02;
  char text[64];
  CHECK(DioramaLayerOrder_FormatRoom(&room, text, sizeof(text)) == 0);
  CHECK(text[0] == '\0');
}

/* Truncation must be reported, not silently produce a half-written section. */
static void TestFormatReportsTruncation(void) {
  DioramaRoomOverride room;
  memset(&room, 0, sizeof(room));
  room.used = true;
  room.map_group = 0x01;
  room.map_number = 0x02;
  room.planes[kPpuOverlaySource_Bg1].set_z = true;
  room.planes[kPpuOverlaySource_Bg1].z = 0.5f;
  char tiny[8];
  size_t need = DioramaLayerOrder_FormatRoom(&room, tiny, sizeof(tiny));
  CHECK(need >= sizeof(tiny));      /* caller can detect it did not fit */
  CHECK(tiny[sizeof(tiny) - 1] == '\0');  /* still NUL-terminated */
}

static void TestTokenRoundTrip(void) {
  static const int kPlanes[] = {
    kDioramaPlane_Backdrop, kPpuOverlaySource_Bg1, kDioramaPlane_Bg1Hi,
    kPpuOverlaySource_Bg2, kDioramaPlane_Bg2Hi, kPpuOverlaySource_Bg3,
    kPpuOverlaySource_Obj, kDioramaPlane_Obj1, kDioramaPlane_Obj2,
    kDioramaPlane_Obj3,
  };
  for (size_t i = 0; i < sizeof(kPlanes) / sizeof(kPlanes[0]); i++) {
    const char *token = DioramaLayerOrder_PlaneToken(kPlanes[i]);
    CHECK(token != NULL);
    CHECK(DioramaLayerOrder_PlaneFromToken(token) == kPlanes[i]);
  }
  CHECK(DioramaLayerOrder_PlaneFromToken("nope") == -1);
  CHECK(DioramaLayerOrder_PlaneFromToken(NULL) == -1);
}

/* Capacity is bounded and reported rather than overrunning. */
static void TestTableCapacity(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  for (int i = 0; i < kDioramaRoomOverrideMax; i++) {
    DioramaRoomOverride *room =
        DioramaLayerOrder_FindOrAdd(&table, 0x01, (uint8_t)i);
    CHECK(room != NULL);
    room->planes[kPpuOverlaySource_Bg1].set_order = true;
  }
  CHECK(DioramaLayerOrder_FindOrAdd(&table, 0x7F, 0x7F) == NULL);
  /* An existing room is still findable when full. */
  CHECK(DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x00) != NULL);
}

/* ── merge: the save path must preserve everything the editor does not own ──
 *
 * The whole reason this exists: the editor's save used to rewrite the file from
 * the table, wiping the documentation preamble and every hand-written comment.
 * These pin that it no longer does -- a user's file survives a save byte-for-byte
 * except the managed section bodies. */

static char *MergeToHeap(const DioramaLayerOrderTable *table,
                         const char *existing, const char *preamble) {
  size_t need = DioramaLayerOrder_MergeManifest(table, existing, preamble,
                                                NULL, 0);
  char *out = (char *)malloc(need + 1);
  CHECK(out != NULL);
  if (!out) return NULL;
  size_t wrote = DioramaLayerOrder_MergeManifest(table, existing, preamble,
                                                 out, need + 1);
  /* The sizing pass and the writing pass must agree, or a caller that trusts
   * the first to size its buffer overflows or truncates. */
  CHECK(wrote == need);
  return out;
}

static void TestMergePreservesUnownedContent(void) {
  /* A file with a documentation preamble, a comment, a foreign section, and a
   * managed section. Everything except the managed body must survive verbatim. */
  const char *existing =
      "# my own notes about this file\n"
      "# do not delete me\n"
      "\n"
      "[notes:whatever]\n"
      "this is not a layers section and must pass through\n"
      "\n"
      "[layers:01:02]  ; Fillmore act 2 -- this HEADER stays\n"
      "bg2hi = rake:0.29\n";

  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  CHECK(room != NULL);
  /* The table now says stack, not rake -- the editor changed the shape. */
  room->planes[kDioramaPlane_Bg2Hi].set_stack = true;
  room->planes[kDioramaPlane_Bg2Hi].stack = 0.29f;
  room->planes[kDioramaPlane_Bg2Hi].set_stack_copies = true;
  room->planes[kDioramaPlane_Bg2Hi].stack_copies = 4;

  char *out = MergeToHeap(&table, existing, NULL);
  if (!out) return;

  /* Every unowned line survived. */
  CHECK(strstr(out, "# my own notes about this file\n") != NULL);
  CHECK(strstr(out, "# do not delete me\n") != NULL);
  CHECK(strstr(out, "[notes:whatever]\n") != NULL);
  CHECK(strstr(out, "this is not a layers section and must pass through\n") != NULL);
  /* The managed body was regenerated: rake gone, stack present. */
  CHECK(strstr(out, "rake:0.29") == NULL);
  CHECK(strstr(out, "bg2hi = stack:0.29 copies:4") != NULL);
  /* No default preamble was injected, because the file already had content. */
  CHECK(strstr(out, "THERE IS AN IN-GAME EDITOR") == NULL);
  free(out);
}

static void TestMergeRegeneratesManagedSectionInPlace(void) {
  /* The managed section is NOT at the end of the file. Its body must be replaced
   * where it sits, and the content after it must remain after it. */
  const char *existing =
      "[layers:01:02]\n"
      "bg2hi = rake:0.29\n"
      "bg1 = z:0.5\n"
      "\n"
      "# a trailing comment that must stay BELOW the room\n";

  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  CHECK(room != NULL);
  room->planes[kDioramaPlane_Bg2Hi].set_bow = true;
  room->planes[kDioramaPlane_Bg2Hi].bow = 0.2f;

  char *out = MergeToHeap(&table, existing, NULL);
  if (!out) return;

  /* Old body gone, new body present, trailing comment still after it. */
  CHECK(strstr(out, "rake:0.29") == NULL);
  CHECK(strstr(out, "z:0.5") == NULL);   /* bg1 is no longer authored */
  const char *bow = strstr(out, "bg2hi = bow:0.2");
  const char *tail = strstr(out, "# a trailing comment");
  CHECK(bow != NULL);
  CHECK(tail != NULL);
  CHECK(bow != NULL && tail != NULL && bow < tail);
  free(out);
}

static void TestMergeAppendsNewRooms(void) {
  /* A room the file has never mentioned is appended, without disturbing the
   * existing content. */
  const char *existing =
      "# preamble\n"
      "[layers:01:02]\n"
      "bg2hi = rake:0.29\n";

  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *keep = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  keep->planes[kDioramaPlane_Bg2Hi].set_rake = true;
  keep->planes[kDioramaPlane_Bg2Hi].rake = 0.29f;
  DioramaRoomOverride *fresh = DioramaLayerOrder_FindOrAdd(&table, 0x02, 0x01);
  fresh->planes[kPpuOverlaySource_Bg1].set_thickness = true;
  fresh->planes[kPpuOverlaySource_Bg1].thickness = 0.2f;

  char *out = MergeToHeap(&table, existing, NULL);
  if (!out) return;

  CHECK(strstr(out, "# preamble\n") != NULL);
  CHECK(strstr(out, "[layers:01:02]") != NULL);
  const char *first = strstr(out, "[layers:01:02]");
  const char *second = strstr(out, "[layers:02:01]");
  CHECK(second != NULL);
  CHECK(first != NULL && second != NULL && first < second);   /* appended AFTER */
  CHECK(strstr(out, "bg1 = thick:0.2") != NULL);
  free(out);
}

static void TestMergeSeedsPreambleOnlyForANewFile(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kDioramaPlane_Bg2Hi].set_rake = true;
  room->planes[kDioramaPlane_Bg2Hi].rake = 0.29f;

  const char *preamble = "# SHIPPED DOCS\n\n";

  /* New file (NULL existing): preamble leads, room follows. */
  char *fresh = MergeToHeap(&table, NULL, preamble);
  if (fresh) {
    CHECK(strncmp(fresh, "# SHIPPED DOCS\n", 15) == 0);
    CHECK(strstr(fresh, "bg2hi = rake:0.29") != NULL);
    free(fresh);
  }

  /* Existing file with its OWN preamble: the shipped one must NOT appear. */
  char *kept = MergeToHeap(&table,
                           "# the user's own header\n[layers:01:02]\nbg2hi = rake:0.29\n",
                           preamble);
  if (kept) {
    CHECK(strstr(kept, "SHIPPED DOCS") == NULL);
    CHECK(strstr(kept, "# the user's own header") != NULL);
    free(kept);
  }
}

static void TestMergeKeepsAnInactiveSectionsText(void) {
  /* A room the editor RESET must actually STAY reset. Its plane lines are dropped
   * -- otherwise the stale overrides remain in the file and the next load makes
   * the room active again, silently undoing the reset the user asked for. The
   * section HEADER and any standalone comments around it are kept, so the user's
   * own annotations survive; only the machine-owned override lines go.
   *
   * An earlier revision of this test asserted the opposite (that an inline comment
   * ON a plane line survived), which encoded the wrong priority: a comment
   * attached to a dropped override cannot outlive it, and "Reset room" silently
   * not persisting is far worse than losing a trailing note. Standalone comment
   * lines are asserted below because those CAN be kept. */
  const char *existing =
      "; a note ABOUT this room that must survive\n"
      "[layers:01:02]\n"
      "bg2hi = rake:0.29  ; an inline note, tied to the override\n"
      "; a standalone note after it\n";

  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  /* Room present in the table but INACTIVE (reset). */
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  CHECK(room != NULL);
  DioramaLayerOrder_ResetRoom(&table, 0x01, 0x02);
  CHECK(!DioramaLayerOrder_RoomIsActive(
      DioramaLayerOrder_Find(&table, 0x01, 0x02)) ||
        DioramaLayerOrder_Find(&table, 0x01, 0x02) == NULL);

  char *out = MergeToHeap(&table, existing, NULL);
  if (!out) return;
  /* The header and the standalone comments survive; the override does NOT. */
  CHECK(strstr(out, "[layers:01:02]") != NULL);
  CHECK(strstr(out, "a note ABOUT this room that must survive") != NULL);
  CHECK(strstr(out, "a standalone note after it") != NULL);
  CHECK(strstr(out, "rake:0.29") == NULL);   /* the reset actually persisted */

  /* And re-loading the merged file leaves the room INACTIVE, which is the whole
   * point -- asserted through the parser rather than by eyeballing the text. */
  DioramaRoomOverride reloaded;
  memset(&reloaded, 0, sizeof(reloaded));
  reloaded.used = true;
  char *scratch = strdup(out);
  char *save = NULL;
  for (char *line = strtok_r(scratch, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char *at = line;
    while (*at == ' ' || *at == '\t') at++;
    for (char *c = at; *c; c++) if (*c == ';' || *c == '#') { *c = '\0'; break; }
    char *end = at + strlen(at);
    while (end > at && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    if (!*at || *at == '[') continue;
    const char *error = NULL;
    (void)DioramaLayerOrder_ParseLine(&reloaded, at, &error);
  }
  CHECK(!DioramaLayerOrder_RoomIsActive(&reloaded));
  free(scratch);
  free(out);
}

/* A COMMENT MID-BODY MUST NOT RESURRECT STALE OVERRIDES.
 *
 * Regression: an earlier fix made a comment inside a managed section end the
 * body skip, so every plane line AFTER that comment survived. Because ParseLine
 * refines rather than clobbers, the loader then applied those stale lines over the
 * regenerated ones -- silently reverting the edit, and bringing back a plane the
 * user had CLEARED. Found by an audit lens.
 *
 * The skip now ends only at the next SECTION header; comments and blanks still
 * pass through, they just no longer re-arm the copy of old overrides. */
static void TestMergeDropsStalePlaneLinesAfterAComment(void) {
  const char *existing =
      "[layers:01:02]\n"
      "bg2hi = rake:0.29\n"
      "; a note in the middle of the body\n"
      "bg1 = z:0.6\n"
      "bg3 = alpha:100\n";

  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  CHECK(room != NULL);
  if (!room) return;
  /* The table authors ONLY bg2hi -- bg1 and bg3 were cleared in the editor. */
  room->planes[kDioramaPlane_Bg2Hi].set_bow = true;
  room->planes[kDioramaPlane_Bg2Hi].bow = 0.2f;

  char *out = MergeToHeap(&table, existing, NULL);
  if (!out) return;
  CHECK(strstr(out, "bg2hi = bow:0.2") != NULL);      /* the live edit */
  CHECK(strstr(out, "; a note in the middle") != NULL); /* comment survives */
  CHECK(strstr(out, "bg1 = z:0.6") == NULL);          /* stale line GONE */
  CHECK(strstr(out, "bg3 = alpha:100") == NULL);

  /* Prove it through the PARSER, since the loader is what the bug fooled: the
   * reloaded room must carry bow on bg2hi and nothing on bg1/bg3. */
  DioramaRoomOverride back;
  memset(&back, 0, sizeof(back));
  back.used = true;
  char *scratch = strdup(out);
  char *save = NULL;
  for (char *line = strtok_r(scratch, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char *at = line;
    while (*at == ' ' || *at == '\t') at++;
    for (char *c = at; *c; c++) if (*c == ';' || *c == '#') { *c = '\0'; break; }
    char *end = at + strlen(at);
    while (end > at && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    if (!*at || *at == '[') continue;
    const char *error = NULL;
    (void)DioramaLayerOrder_ParseLine(&back, at, &error);
  }
  CHECK(back.planes[kDioramaPlane_Bg2Hi].set_bow);
  CHECK(!back.planes[kPpuOverlaySource_Bg1].set_z);
  CHECK(!back.planes[kPpuOverlaySource_Bg3].set_alpha);
  free(scratch);
  free(out);
}

static void TestMergeIsIdempotentAndRoundTrips(void) {
  /* Saving twice with no change between must be a no-op, and the merged file
   * must reload into the same table -- the file is still a valid manifest. */
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  room->planes[kDioramaPlane_Bg2Hi].set_stack = true;
  room->planes[kDioramaPlane_Bg2Hi].stack = 0.29f;
  room->planes[kDioramaPlane_Bg2Hi].set_stack_copies = true;
  room->planes[kDioramaPlane_Bg2Hi].stack_copies = 4;

  char *first = MergeToHeap(&table, "# docs\n\n", NULL);
  if (!first) return;
  char *second = MergeToHeap(&table, first, NULL);   /* feed it back in */
  if (second) {
    CHECK(strcmp(first, second) == 0);   /* idempotent */
    free(second);
  }

  /* Re-parse the merged text: the managed room must come back identical. */
  DioramaRoomOverride reloaded;
  memset(&reloaded, 0, sizeof(reloaded));
  reloaded.used = true;
  char *scratch = strdup(first);
  char *save = NULL;
  for (char *line = strtok_r(scratch, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    char *at = line;
    while (*at == ' ' || *at == '\t') at++;
    for (char *s = at; *s; s++) if (*s == ';' || *s == '#') { *s = '\0'; break; }
    char *end = at + strlen(at);
    while (end > at && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    if (!*at || *at == '[') continue;
    const char *error = NULL;
    CHECK(DioramaLayerOrder_ParseLine(&reloaded, at, &error));
  }
  const DioramaPlaneOverride *back = &reloaded.planes[kDioramaPlane_Bg2Hi];
  CHECK(back->set_stack && back->stack == 0.29f);
  CHECK(back->set_stack_copies && back->stack_copies == 4);
  free(scratch);
  free(first);
}

static void TestMergeSizingContract(void) {
  /* The size-0 pass returns the exact length the write pass produces, and the
   * write never exceeds the buffer -- the contract diorama.c relies on to size
   * its allocation in one probe. */
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x03, 0x04);
  room->planes[kPpuOverlaySource_Bg1].set_voxel = true;
  room->planes[kPpuOverlaySource_Bg1].voxel = 0.18f;
  room->planes[kPpuOverlaySource_Bg1].set_voxel_copies = true;
  room->planes[kPpuOverlaySource_Bg1].voxel_copies = 14;

  const char *existing = "# doc line one\n# doc line two\n\n[layers:03:04]\nbg1 = voxel:0.18 slices:14\n";
  size_t need = DioramaLayerOrder_MergeManifest(&table, existing, NULL, NULL, 0);
  CHECK(need > 0);

  /* An exactly-sized buffer: result is NUL-terminated and full. */
  char *exact = (char *)malloc(need + 1);
  CHECK(exact != NULL);
  if (exact) {
    size_t wrote = DioramaLayerOrder_MergeManifest(&table, existing, NULL,
                                                   exact, need + 1);
    CHECK(wrote == need);
    CHECK(strlen(exact) == need);
    free(exact);
  }

  /* An UNDERSIZED buffer must not overflow and must stay terminated. */
  char small[16];
  memset(small, 0x7F, sizeof(small));
  size_t wrote_small = DioramaLayerOrder_MergeManifest(&table, existing, NULL,
                                                       small, sizeof(small));
  CHECK(wrote_small == need);              /* still reports the true length */
  CHECK(small[sizeof(small) - 1] == '\0'); /* never wrote past the end */

  /* The NEW-FILE path writes the preamble through a different accumulator
   * (OUT_TEXT); its size pass and write pass must agree just as tightly, or a
   * first save into a fresh file mis-sizes. Exercised with a NULL existing so
   * the preamble branch is taken. */
  const char *preamble = "# a preamble whose exact length must be counted\n\n";
  size_t new_need = DioramaLayerOrder_MergeManifest(&table, NULL, preamble,
                                                    NULL, 0);
  char *new_out = (char *)malloc(new_need + 1);
  CHECK(new_out != NULL);
  if (new_out) {
    size_t new_wrote = DioramaLayerOrder_MergeManifest(&table, NULL, preamble,
                                                       new_out, new_need + 1);
    CHECK(new_wrote == new_need);
    CHECK(strlen(new_out) == new_need);
    /* The preamble is actually there and complete -- a miscount that happened to
     * net out would still be caught by the substring. */
    CHECK(strstr(new_out, "whose exact length must be counted") != NULL);
    free(new_out);
  }
}

int main(void) {
  TestNoOverrideIsIdentity();
  TestOverrideIsScopedToItsRoom();
  TestOrderEditReordersPaint();
  TestSortIsStableForUnnamedPlanes();
  TestNonOrderEditDoesNotReorder();
  TestAlphaOverride();
  TestResetRoomRestoresDefaults();
  TestResetSlotIsRecycled();
  TestSectionParsing();
  TestScopedSourceInheritsBaseRoom();
  TestRomSourceCatalogue();
  TestLineParsing();
  TestFormatRoundTrips();
  TestRakeAndThicknessResolve();
  TestStackResolve();
  TestStackParseAndRoundTrip();
  TestStackDensityAndDirection();
  TestVoxelResolve();
  TestBowResolveAndRoundTrip();
  TestStrategyOf();
  TestRakeOnlyRoomIsActiveAndRoundTrips();
  TestRakeAndThicknessRejectBadValues();
  TestNonFiniteValuesAreRejected();
  TestZIsBounded();
  TestInactiveRoomEmitsNothing();
  TestFormatReportsTruncation();
  TestTokenRoundTrip();
  TestTableCapacity();
  TestMergePreservesUnownedContent();
  TestMergeRegeneratesManagedSectionInPlace();
  TestMergeAppendsNewRooms();
  TestMergeSeedsPreambleOnlyForANewFile();
  TestMergeKeepsAnInactiveSectionsText();
  TestMergeIsIdempotentAndRoundTrips();
  TestMergeSizingContract();
  TestMergeDropsStalePlaneLinesAfterAComment();
  if (g_failures) {
    printf("diorama_layer_order_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("diorama_layer_order_test: all checks passed\n");
  return 0;
}
