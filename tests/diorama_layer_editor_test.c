/* The in-game diorama layer editor's row model and strategy authoring.
 *
 * THE LOAD-BEARING ASSERTION is exclusivity. The six depth shapes are NOT
 * exclusive in the data model: the renderer sums rake and bow, a voxel and a
 * stack resolve onto the SAME field, and
 * DioramaLayerOrder_StrategyOf reports whichever key DOMINATES rather than the
 * only one set. So "select stack" must clear the keys that would otherwise
 * dominate or add to it. If that clearing is wrong, the row says one shape and
 * the renderer draws another -- silently, and on a machine that has no ROM to
 * check it on. Hence:
 *
 *   1. EXCLUSIVITY: after authoring any strategy, exactly that strategy is what
 *      both this module and the RESOLVED layer report -- checked by running the
 *      override through DioramaLayerOrder_Resolve, i.e. the real renderer path.
 *   2. ROUND TRIP: authored strategies survive FormatRoom -> ParseLine, because
 *      the editor's whole output is a file the game reloads at boot.
 *   3. TRUE UNDO: cycling back to Flat, or clearing a plane, leaves a room
 *      indistinguishable from one never authored -- the unedited-game guarantee.
 *   4. BOUNDS AGREE WITH THE PARSER: a value the editor can author must be a
 *      value the manifest can reload. The UI clamps where the parser rejects, so
 *      the two are separate code paths that must not drift.
 *   5. ROWS MATCH THE SHAPE: only parameters the active shape actually consumes
 *      are listed, or the player steps a row that does nothing.
 */
#include "diorama_layer_editor.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"
#include "diorama_layer_order.h"

static int g_failures;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
      g_failures++;                                                        \
    }                                                                      \
  } while (0)

/* diorama.c's real kDioramaLayers table, so a failure maps onto what the
 * game would actually draw. */
static const DioramaResolvedLayer kDefaults[] = {
  { kDioramaPlane_Backdrop, 0.00f, 255 },
  { kPpuOverlaySource_Obj,  0.51f, 255 },
  { kDioramaPlane_Obj1,     0.51f, 255 },
  { kDioramaPlane_Bg2Far,   0.05f, 255 },
  { kPpuOverlaySource_Bg2,  0.20f, 255 },
  { kDioramaPlane_Bg1Far,   0.35f, 255 },
  { kPpuOverlaySource_Bg1,  0.50f, 255 },
  { kDioramaPlane_Obj2,     0.51f, 255 },
  { kDioramaPlane_Bg2Hi,    0.21f, 255 },
  { kDioramaPlane_Bg1Hi,    0.51f, 255 },
  { kDioramaPlane_Obj3,     0.52f, 255 },
  { kPpuOverlaySource_Bg3,  0.95f, 255 },
};
static const int kDefaultCount =
    (int)(sizeof(kDefaults) / sizeof(kDefaults[0]));

static const DioramaResolvedLayer *FindResolved(
    const DioramaResolvedLayer *out, int n, int plane) {
  for (int i = 0; i < n; i++)
    if (out[i].plane == plane) return &out[i];
  return NULL;
}

/* ── 1. exclusivity, checked through the real resolve path ───────────────── */

/* Author each strategy in turn on one plane and confirm the RESOLVED layer
 * reports exactly it. This is the assertion that catches a bad clear: authoring
 * a bow while a rake's value lingers resolves to a plane carrying both, and
 * since they sum, the renderer tilts it further than the row claims. */
static void TestEachStrategyResolvesToItself(void) {
  for (int s = 0; s < kDioramaDepth_StrategyCount; s++) {
    DioramaLayerOrderTable table;
    memset(&table, 0, sizeof(table));
    DioramaRoomOverride *room =
        DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
    CHECK(room != NULL);
    if (!room) continue;
    DioramaPlaneOverride *plane = &room->planes[kDioramaPlane_Bg2Hi];

    /* Author EVERY shape first, so each iteration starts from the worst case:
     * a plane carrying all of them at once. A SetStrategy that forgets to clear
     * one will be caught here and nowhere else. */
    plane->set_rake = true;       plane->rake = 0.40f;
    plane->set_bow = true;        plane->bow = 0.30f;
    plane->set_thickness = true;  plane->thickness = 0.25f;
    plane->set_stack = true;      plane->stack = 0.35f;
    plane->set_stack_copies = true; plane->stack_copies = 5;
    plane->set_voxel = true;      plane->voxel = 0.15f;
    plane->set_voxel_copies = true; plane->voxel_copies = 20;

    DioramaLayerEditor_SetStrategy(plane, (DioramaDepthStrategy)s);
    CHECK(DioramaLayerEditor_StrategyOfPlane(plane) ==
          (DioramaDepthStrategy)s);

    DioramaResolvedLayer out[16];
    int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                      kDefaultCount, out, 16);
    const DioramaResolvedLayer *layer =
        FindResolved(out, n, kDioramaPlane_Bg2Hi);
    CHECK(layer != NULL);
    if (!layer) continue;
    /* The renderer's own verdict, not the editor's. */
    CHECK(DioramaLayerOrder_StrategyOf(layer) == (DioramaDepthStrategy)s);

    /* And the fields the renderer reads for the shapes NOT chosen must be
     * inert, since diorama.c gates each pass on its own value being non-zero
     * rather than on a strategy enum. Checked field by field: a resolved layer
     * that reports "stack" while still carrying a thickness would draw a skirt
     * nobody asked for. */
    switch (s) {
      case kDioramaDepth_Flat:
        CHECK(layer->rake == 0.0f);
        CHECK(layer->bow == 0.0f);
        CHECK(layer->thickness == 0.0f);
        CHECK(layer->stack == 0.0f);
        break;
      case kDioramaDepth_Rake:
        CHECK(layer->rake != 0.0f);
        CHECK(layer->bow == 0.0f);
        CHECK(layer->thickness == 0.0f);
        CHECK(layer->stack == 0.0f);
        break;
      case kDioramaDepth_Bow:
        CHECK(layer->bow != 0.0f);
        CHECK(layer->rake == 0.0f);
        CHECK(layer->thickness == 0.0f);
        CHECK(layer->stack == 0.0f);
        break;
      case kDioramaDepth_Thick:
        CHECK(layer->thickness > 0.0f);
        CHECK(layer->rake == 0.0f);
        CHECK(layer->bow == 0.0f);
        CHECK(layer->stack == 0.0f);
        break;
      case kDioramaDepth_Stack:
        CHECK(layer->stack > 0.0f);
        CHECK(layer->stack_copies > 1);
        CHECK(!layer->stack_solid);   /* a faded stack, not a solid voxel */
        CHECK(layer->rake == 0.0f);
        CHECK(layer->bow == 0.0f);
        CHECK(layer->thickness == 0.0f);
        break;
      case kDioramaDepth_Voxel:
        CHECK(layer->stack > 0.0f);
        CHECK(layer->stack_solid);
        CHECK(layer->stack_copies > 1);
        CHECK(layer->rake == 0.0f);
        CHECK(layer->bow == 0.0f);
        CHECK(layer->thickness == 0.0f);
        break;
      default:
        break;
    }
  }
}

/* Cycling forward through all six returns to where it started, and every step
 * lands on the strategy the enum order promises. The editor's contract is that
 * Left/Right walks a ring; if it stuck or skipped, a shape would be unreachable
 * from the UI while still being authorable by hand. */
static void TestCycleWrapsThroughEveryStrategy(void) {
  DioramaPlaneOverride plane;
  memset(&plane, 0, sizeof(plane));
  CHECK(DioramaLayerEditor_StrategyOfPlane(&plane) == kDioramaDepth_Flat);

  for (int i = 1; i <= kDioramaDepth_StrategyCount; i++) {
    DioramaDepthStrategy got = DioramaLayerEditor_CycleStrategy(&plane, +1);
    CHECK((int)got == i % kDioramaDepth_StrategyCount);
  }
  CHECK(DioramaLayerEditor_StrategyOfPlane(&plane) == kDioramaDepth_Flat);

  /* Backwards too: the first press from Flat must reach Voxel, not stall. */
  CHECK(DioramaLayerEditor_CycleStrategy(&plane, -1) == kDioramaDepth_Voxel);
  for (int i = 0; i < kDioramaDepth_StrategyCount - 1; i++)
    DioramaLayerEditor_CycleStrategy(&plane, -1);
  CHECK(DioramaLayerEditor_StrategyOfPlane(&plane) == kDioramaDepth_Flat);
}

/* Authoring a shape must produce a VISIBLE one. A shape with magnitude zero
 * satisfies "the flag is set" while drawing nothing, which is exactly the bug
 * that makes a UI feel broken -- and `thick` shipped inert for a week for a
 * related reason. */
static void TestAuthoredStrategyIsNonZero(void) {
  for (int s = kDioramaDepth_Rake; s < kDioramaDepth_StrategyCount; s++) {
    DioramaPlaneOverride plane;
    memset(&plane, 0, sizeof(plane));
    DioramaLayerEditor_SetStrategy(&plane, (DioramaDepthStrategy)s);

    DioramaLayerOrderTable table;
    memset(&table, 0, sizeof(table));
    DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
    CHECK(room != NULL);
    if (!room) continue;
    room->planes[kPpuOverlaySource_Bg1] = plane;

    DioramaResolvedLayer out[16];
    int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                      kDefaultCount, out, 16);
    const DioramaResolvedLayer *layer =
        FindResolved(out, n, kPpuOverlaySource_Bg1);
    CHECK(layer != NULL);
    if (!layer) continue;
    /* Whichever shape it is, SOMETHING must be non-zero, and a repeat-based
     * shape needs more than one copy or the renderer's layer_stack_copies gate
     * skips it entirely. */
    const bool visible =
        layer->rake != 0.0f || layer->bow != 0.0f || layer->thickness > 0.0f ||
        (layer->stack > 0.0f && layer->stack_copies > 1);
    CHECK(visible);
  }
}

/* A tilt magnitude carries across rake<->bow, and a fill across stack<->voxel,
 * because those pairs are the same quantity in a different curve/fade. Tuning a
 * rake to 0.15 and cycling to bow must not silently snap back to the default --
 * the player is comparing the SAME depth in two shapes. */
static void TestMagnitudeCarriesBetweenRelatedShapes(void) {
  DioramaPlaneOverride plane;
  memset(&plane, 0, sizeof(plane));
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Rake);
  plane.rake = 0.15f;
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Bow);
  CHECK(plane.bow == 0.15f);
  CHECK(plane.rake == 0.0f);   /* and the old key is gone, not merely unflagged */
  CHECK(!plane.set_rake);

  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Stack);
  plane.stack = 0.42f;
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Voxel);
  CHECK(plane.voxel == 0.42f);
  CHECK(plane.stack == 0.0f);
  CHECK(!plane.set_stack);

  /* But a thickness is NOT a tilt, so cycling through it must not smuggle a
   * magnitude across. Rake -> Thick -> Rake returns the DEFAULT, not 0.15. */
  memset(&plane, 0, sizeof(plane));
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Rake);
  plane.rake = 0.15f;
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Thick);
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Rake);
  CHECK(plane.rake != 0.15f);
}

/* ── 2. round trip through the manifest ─────────────────────────────────── */

/* Everything the editor authors must survive a save/load, since that file IS
 * the editor's persistence. A lossy field would mean an edit that vanishes on
 * restart -- the worst possible failure for a tool whose purpose is comparing
 * shapes across sessions. */
static void TestStrategyRoundTripsThroughManifest(void) {
  for (int s = kDioramaDepth_Rake; s < kDioramaDepth_StrategyCount; s++) {
    DioramaLayerOrderTable table;
    memset(&table, 0, sizeof(table));
    DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x03, 0x04);
    CHECK(room != NULL);
    if (!room) continue;
    DioramaPlaneOverride *plane = &room->planes[kDioramaPlane_Bg2Hi];
    DioramaLayerEditor_SetStrategy(plane, (DioramaDepthStrategy)s);
    /* Author a direction too where the shape has one, since it is the only enum
     * key and the only one whose token spelling could drift. The repeat-based
     * shapes are the only ones with a fill side -- a tilt and a skirt have no
     * direction to choose -- and StepParam must refuse it on the others rather
     * than authoring a key the renderer would ignore. */
    const bool directional = s == kDioramaDepth_Stack || s == kDioramaDepth_Voxel;
    CHECK(DioramaLayerEditor_StepParam(plane, kDioramaEditorParam_Direction,
                                       +1) == directional);

    char text[1024];
    size_t need = DioramaLayerOrder_FormatRoom(room, text, sizeof(text));
    CHECK(need > 0 && need < sizeof(text));

    /* Re-parse the emitted body lines into a fresh room. */
    DioramaRoomOverride reloaded;
    memset(&reloaded, 0, sizeof(reloaded));
    reloaded.used = true;
    reloaded.map_group = 0x03;
    reloaded.map_number = 0x04;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
      if (line[0] == '[' || !line[0]) continue;   /* section header */
      const char *error = NULL;
      CHECK(DioramaLayerOrder_ParseLine(&reloaded, line, &error));
    }

    const DioramaPlaneOverride *back = &reloaded.planes[kDioramaPlane_Bg2Hi];
    CHECK(DioramaLayerEditor_StrategyOfPlane(back) == (DioramaDepthStrategy)s);
    CHECK(back->set_stack_direction == directional);
    CHECK(back->stack_direction == plane->stack_direction);
    /* Exact magnitudes too: %.4g must not have rounded the value the editor
     * chose into a different one. */
    CHECK(back->rake == plane->rake);
    CHECK(back->bow == plane->bow);
    CHECK(back->thickness == plane->thickness);
    CHECK(back->stack == plane->stack);
    CHECK(back->voxel == plane->voxel);
    CHECK(back->stack_copies == plane->stack_copies);
    CHECK(back->voxel_copies == plane->voxel_copies);
  }
}

/* ── 3. true undo ───────────────────────────────────────────────────────── */

/* The unedited-game guarantee, from the editor's side: authoring then undoing
 * must leave the room INACTIVE, so Resolve returns the built-in table verbatim.
 * A room that stayed "active" with all-zero overrides would still take the sort
 * path, and even if that sort is a no-op, relying on it would make the
 * guarantee accidental rather than structural. */
static void TestUndoLeavesRoomInactive(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  CHECK(room != NULL);
  if (!room) return;
  DioramaPlaneOverride *plane = &room->planes[kDioramaPlane_Bg2Hi];

  DioramaLayerEditor_SetStrategy(plane, kDioramaDepth_Voxel);
  CHECK(DioramaLayerOrder_RoomIsActive(room));

  /* Cycling to Flat is the player's most likely undo: press Left until the row
   * says FLAT. That must fully deactivate, not leave a zeroed voxel behind. */
  DioramaLayerEditor_SetStrategy(plane, kDioramaDepth_Flat);
  CHECK(!DioramaLayerOrder_RoomIsActive(room));

  /* And the explicit clear, from a plane carrying non-shape keys too. */
  DioramaLayerEditor_SetStrategy(plane, kDioramaDepth_Stack);
  DioramaLayerEditor_StepParam(plane, kDioramaEditorParam_Alpha, -1);
  DioramaLayerEditor_StepParam(plane, kDioramaEditorParam_Z, +1);
  CHECK(DioramaLayerOrder_RoomIsActive(room));
  DioramaLayerEditor_ClearPlane(plane);
  CHECK(!DioramaLayerOrder_RoomIsActive(room));

  DioramaResolvedLayer out[16];
  int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                    kDefaultCount, out, 16);
  CHECK(n == kDefaultCount);
  for (int i = 0; i < n; i++) {
    CHECK(out[i].plane == kDefaults[i].plane);
    CHECK(out[i].z == kDefaults[i].z);
    CHECK(out[i].rake == 0.0f);
    CHECK(out[i].bow == 0.0f);
    CHECK(out[i].thickness == 0.0f);
    CHECK(out[i].stack == 0.0f);
  }
}

/* Clearing a shape's magnitude removes the SHAPE, not just the number. A stack
 * whose depth is zero draws nothing while keeping the room authored, so the file
 * would grow an entry that has no effect and cannot be seen. */
static void TestClearingDepthRemovesTheShape(void) {
  DioramaPlaneOverride plane;
  memset(&plane, 0, sizeof(plane));
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Stack);
  DioramaLayerEditor_ClearParam(&plane, kDioramaEditorParam_Depth);
  CHECK(DioramaLayerEditor_StrategyOfPlane(&plane) == kDioramaDepth_Flat);
  CHECK(!plane.set_stack);
  CHECK(!plane.set_stack_copies);
}

/* ── 4. bounds agree with the parser ────────────────────────────────────── */

/* Whatever the editor can reach, the manifest must accept. The UI clamps and the
 * parser rejects, so they are separate code paths over the same grammar; this
 * drives each key to both ends and re-parses the result. */
static void TestSteppingStaysWithinParserBounds(void) {
  static const DioramaEditorParam kParams[] = {
    kDioramaEditorParam_Depth, kDioramaEditorParam_Copies,
    kDioramaEditorParam_Density, kDioramaEditorParam_Z,
    kDioramaEditorParam_Alpha, kDioramaEditorParam_Order,
  };
  for (int s = kDioramaDepth_Rake; s < kDioramaDepth_StrategyCount; s++) {
    for (size_t k = 0; k < sizeof(kParams) / sizeof(kParams[0]); k++) {
      for (int dir = -1; dir <= 1; dir += 2) {
        DioramaRoomOverride room;
        memset(&room, 0, sizeof(room));
        room.used = true;
        DioramaPlaneOverride *plane = &room.planes[kPpuOverlaySource_Bg1];
        DioramaLayerEditor_SetStrategy(plane, (DioramaDepthStrategy)s);
        /* Density is only offered once authored, so author it first when that
         * is the key under test. */
        if (kParams[k] == kDioramaEditorParam_Density)
          DioramaLayerEditor_StepParam(plane, kDioramaEditorParam_Density, +1);

        /* Far more presses than any range needs, so every key ends pinned at a
         * bound rather than merely somewhere legal. */
        for (int press = 0; press < 400; press++)
          DioramaLayerEditor_StepParam(plane, kParams[k], dir);

        char text[1024];
        size_t need = DioramaLayerOrder_FormatRoom(&room, text, sizeof(text));
        /* Zero is a legitimate outcome now, not a failure: holding Left on an
         * UNSIGNED shape's depth (thick/stack/voxel) steps it to zero, which
         * removes the shape entirely -- so the room is inactive and there is
         * nothing to emit. An earlier version of this test asserted `need > 0`
         * unconditionally, which was encoding the bug where a zero-magnitude
         * shape stayed authored and wrote a dead `stack:0` line. Only the upper
         * bound is a real requirement here; what a zero-step should DO is
         * asserted by TestSteppingToZeroClearsTheShape. */
        CHECK(need < sizeof(text));
        char *save = NULL;
        DioramaRoomOverride reloaded;
        memset(&reloaded, 0, sizeof(reloaded));
        reloaded.used = true;
        for (char *line = strtok_r(text, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
          if (line[0] == '[' || !line[0]) continue;
          const char *error = NULL;
          /* THE assertion: the parser must accept every value the UI produced.
           * A failure here means the editor can write a file the game rejects
           * on the next boot. */
          CHECK(DioramaLayerOrder_ParseLine(&reloaded, line, &error));
        }
      }
    }
  }
}

/* Stepping reports whether anything moved, so the overlay can say "at the
 * limit" instead of appearing to ignore the key. At a bound it must report
 * false. */
static void TestStepReportsNoChangeAtBound(void) {
  DioramaPlaneOverride plane;
  memset(&plane, 0, sizeof(plane));
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Stack);
  for (int i = 0; i < 200; i++)
    DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Copies, +1);
  CHECK(plane.stack_copies == kDioramaStackMax);
  CHECK(!DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Copies, +1));
  /* And a voxel's own, higher cap is respected rather than the stack's. */
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Voxel);
  for (int i = 0; i < 200; i++)
    DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Copies, +1);
  CHECK(plane.voxel_copies == kDioramaVoxelMax);
  CHECK(!DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Copies, +1));
}

/* An explicit copy count outranks a density in Resolve, so the editor must not
 * leave both authored -- the density row would show a number with no effect. */
static void TestCopiesAndDensityAreMutuallyExclusive(void) {
  DioramaPlaneOverride plane;
  memset(&plane, 0, sizeof(plane));
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Stack);
  CHECK(plane.set_stack_copies);

  DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Density, +1);
  CHECK(plane.set_stack_density);
  CHECK(!plane.set_stack_copies);

  DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Copies, +1);
  CHECK(plane.set_stack_copies);
  CHECK(!plane.set_stack_density);
}

/* Stepping snaps a hand-edited value onto the editor's grid rather than
 * carrying its remainder forever, so the displayed 2-decimal value always
 * matches the stored one. */
static void TestStepSnapsToGrid(void) {
  DioramaPlaneOverride plane;
  memset(&plane, 0, sizeof(plane));
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Rake);

  /* Every value below has a fractional part ABOVE half a step, so rounding and
   * truncation give different answers. That matters: an earlier version of this
   * test used 0.293, where 0.293/0.01 = 29.3 truncates and rounds alike, so a
   * probe replacing the rounding with truncation SURVIVED it. Cases that cannot
   * distinguish the two implementations assert nothing about which is used.
   *
   * 0.297 / 0.01 = 29.7 -> rounds to 30, truncates to 29. Hand-computed. */
  plane.rake = 0.297f;   /* as if loaded from a hand-edited manifest */
  DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Depth, +1);
  CHECK(plane.rake > 0.3099f && plane.rake < 0.3101f);   /* 31, not 30 */

  plane.rake = 0.297f;
  DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Depth, -1);
  CHECK(plane.rake > 0.2899f && plane.rake < 0.2901f);   /* 29, not 28 */

  /* Negative side: a rake may be negative (a ceiling), and the rounding must be
   * symmetric about zero rather than biased toward it -- which is exactly what a
   * bare (int) cast would do, since C truncates toward zero. -0.297 must round
   * to -30, so one step down is -0.31 and not -0.30. */
  plane.rake = -0.297f;
  DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Depth, -1);
  CHECK(plane.rake < -0.3099f && plane.rake > -0.3101f);

  plane.rake = -0.297f;
  DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Depth, +1);
  CHECK(plane.rake < -0.2899f && plane.rake > -0.2901f);

  /* A second magnitude entirely, so the assertion is about the rule and not
   * about one lucky value: 0.156 / 0.01 = 15.6 -> 16, +1 = 0.17. */
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Stack);
  plane.stack = 0.156f;
  DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Depth, +1);
  CHECK(plane.stack > 0.1699f && plane.stack < 0.1701f);
}

/* ── 5. rows match the shape ─────────────────────────────────────────────── */

static const DioramaEditorRow *FindRow(const DioramaEditorRow *rows, int n,
                                       int plane, DioramaEditorParam param) {
  for (int i = 0; i < n; i++)
    if (rows[i].plane == plane && rows[i].param == param) return &rows[i];
  return NULL;
}

/* A tab for a level the player is not in explains itself in one row rather than
 * rendering an unexplained blank panel. */
static void TestForeignLevelTabExplainsItself(void) {
  DioramaEditorContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.room_live = true;
  ctx.map_group = kActRaiserMapGroup_Fillmore;
  ctx.map_number = 0x02;
  ctx.selected_plane = -1;

  DioramaEditorRow rows[kDioramaEditorRowMax];
  int bloodpool = DioramaLayerEditor_LevelIndexOfGroup(
      kActRaiserMapGroup_Bloodpool);
  CHECK(bloodpool >= 0);
  int n = DioramaLayerEditor_BuildRows(NULL, &ctx, bloodpool, rows,
                                       kDioramaEditorRowMax);
  CHECK(n == 1);
  CHECK(rows[0].kind == kDioramaEditorRow_Header);
  CHECK(!rows[0].selectable);

  /* Same when no room is live at all (the diorama is off, or the player is in a
   * town): every tab explains itself rather than showing stale planes. */
  ctx.room_live = false;
  n = DioramaLayerEditor_BuildRows(NULL, &ctx, 0, rows, kDioramaEditorRowMax);
  CHECK(n == 1);
  CHECK(rows[0].kind == kDioramaEditorRow_Header);
}

/* The live level's tab lists every drawable plane, even unauthored ones -- the
 * player must be able to reach a plane in order to author it. */
static void TestLiveTabListsEveryPlane(void) {
  DioramaEditorContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.room_live = true;
  ctx.map_group = kActRaiserMapGroup_Fillmore;
  ctx.map_number = 0x02;
  ctx.selected_plane = -1;

  DioramaEditorRow rows[kDioramaEditorRowMax];
  int level = DioramaLayerEditor_LevelIndexOfGroup(kActRaiserMapGroup_Fillmore);
  int n = DioramaLayerEditor_BuildRows(NULL, &ctx, level, rows,
                                       kDioramaEditorRowMax);
  int planes = 0;
  for (int i = 0; i < n; i++)
    if (rows[i].kind == kDioramaEditorRow_Plane) planes++;
  /* Every token the manifest grammar accepts, so no plane is authorable by hand
   * but unreachable in the UI. */
  int tokens = 0;
  for (int p = 0; p < kDioramaPlane_Count; p++)
    if (DioramaLayerOrder_PlaneToken(p)) tokens++;
  CHECK(planes == tokens);
  CHECK(rows[0].kind == kDioramaEditorRow_Header);
  CHECK(rows[n - 1].kind == kDioramaEditorRow_ResetRoom);
  /* With nothing authored every plane reads FLAT, which is the honest report of
   * a room the file does not mention. */
  for (int i = 0; i < n; i++)
    if (rows[i].kind == kDioramaEditorRow_Plane)
      CHECK(!strcmp(rows[i].value, "FLAT"));
}

/* Only the SELECTED plane expands, and only into the parameters its active
 * shape consumes. A listed row the renderer ignores is the failure mode: the
 * player steps it, nothing changes, and the tool loses their trust. */
static void TestParamRowsFollowTheActiveShape(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  CHECK(room != NULL);
  if (!room) return;
  DioramaPlaneOverride *plane = &room->planes[kDioramaPlane_Bg2Hi];

  DioramaEditorContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.room_live = true;
  ctx.map_group = kActRaiserMapGroup_Fillmore;
  ctx.map_number = 0x02;
  ctx.selected_plane = kDioramaPlane_Bg2Hi;
  const int level =
      DioramaLayerEditor_LevelIndexOfGroup(kActRaiserMapGroup_Fillmore);
  DioramaEditorRow rows[kDioramaEditorRowMax];

  /* Flat: no depth row at all, because there is no magnitude to step. */
  int n = DioramaLayerEditor_BuildRows(&table, &ctx, level, rows,
                                      kDioramaEditorRowMax);
  CHECK(!FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Depth));
  CHECK(!FindRow(rows, n, kDioramaPlane_Bg2Hi,
                 kDioramaEditorParam_TransparentFill));
  /* But z/alpha/order apply to any plane, shape or not. */
  CHECK(FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Z));

  /* Rake: a depth row, and NO copies/direction -- a tilt has neither. */
  DioramaLayerEditor_SetStrategy(plane, kDioramaDepth_Rake);
  n = DioramaLayerEditor_BuildRows(&table, &ctx, level, rows,
                                   kDioramaEditorRowMax);
  CHECK(FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Depth));
  CHECK(!FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Copies));
  CHECK(!FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Direction));

  /* Thick: a depth row, still no copies -- a skirt is one extrusion. */
  DioramaLayerEditor_SetStrategy(plane, kDioramaDepth_Thick);
  n = DioramaLayerEditor_BuildRows(&table, &ctx, level, rows,
                                   kDioramaEditorRowMax);
  CHECK(FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Depth));
  CHECK(!FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Copies));

  /* Stack: depth, copies and direction; density only once authored. */
  DioramaLayerEditor_SetStrategy(plane, kDioramaDepth_Stack);
  n = DioramaLayerEditor_BuildRows(&table, &ctx, level, rows,
                                   kDioramaEditorRowMax);
  CHECK(FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Copies));
  CHECK(FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Direction));
  CHECK(!FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Density));
  const DioramaEditorRow *copies =
      FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Copies);
  CHECK(copies && !strcmp(copies->label, "copies"));

  DioramaLayerEditor_StepParam(plane, kDioramaEditorParam_Density, +1);
  n = DioramaLayerEditor_BuildRows(&table, &ctx, level, rows,
                                   kDioramaEditorRowMax);
  CHECK(FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Density));

  /* Voxel: the same rows, but the count is called "slices" -- the manifest key
   * differs, and a row labelled "copies" would send the player to the wrong
   * key when they hand-edit the file. */
  DioramaLayerEditor_SetStrategy(plane, kDioramaDepth_Voxel);
  n = DioramaLayerEditor_BuildRows(&table, &ctx, level, rows,
                                   kDioramaEditorRowMax);
  const DioramaEditorRow *count_row =
      FindRow(rows, n, kDioramaPlane_Bg2Hi, kDioramaEditorParam_Copies);
  CHECK(count_row && !strcmp(count_row->label, "slices"));

  /* Only ONE plane expands: no other plane contributes parameter rows. */
  for (int i = 0; i < n; i++)
    if (rows[i].nested) CHECK(rows[i].plane == kDioramaPlane_Bg2Hi);
}

/* An unauthored knob shows a dash, not the built-in number -- otherwise every
 * plane looks authored and the player cannot tell what the room contributes. */
static void TestUnauthoredKnobsShowDash(void) {
  DioramaEditorContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.room_live = true;
  ctx.map_group = kActRaiserMapGroup_Fillmore;
  ctx.map_number = 0x02;
  ctx.selected_plane = kPpuOverlaySource_Bg1;

  DioramaEditorRow rows[kDioramaEditorRowMax];
  const int level =
      DioramaLayerEditor_LevelIndexOfGroup(kActRaiserMapGroup_Fillmore);
  int n = DioramaLayerEditor_BuildRows(NULL, &ctx, level, rows,
                                       kDioramaEditorRowMax);
  const DioramaEditorRow *z =
      FindRow(rows, n, kPpuOverlaySource_Bg1, kDioramaEditorParam_Z);
  CHECK(z && !strcmp(z->value, "--"));
  const DioramaEditorRow *fill = FindRow(
      rows, n, kPpuOverlaySource_Bg1,
      kDioramaEditorParam_TransparentFill);
  CHECK(fill && fill->kind == kDioramaEditorRow_ParamEnum);
  CHECK(fill && !strcmp(fill->value, "OFF"));

  DioramaPlaneOverride plane;
  memset(&plane, 0, sizeof(plane));
  CHECK(DioramaLayerEditor_StepParam(
      &plane, kDioramaEditorParam_TransparentFill, +1));
  CHECK(plane.set_transparent_fill);
  CHECK(plane.transparent_fill_kind == kDioramaTransparentFill_Black);
  CHECK(DioramaLayerEditor_StepParam(
      &plane, kDioramaEditorParam_TransparentFill, -1));
  CHECK(plane.set_transparent_fill);
  CHECK(plane.transparent_fill_kind == kDioramaTransparentFill_None);
  DioramaLayerEditor_ClearParam(
      &plane, kDioramaEditorParam_TransparentFill);
  CHECK(!plane.set_transparent_fill);
}

static void TestBackdropSourceIsScopedAndEditable(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *waterfall = DioramaLayerOrder_FindOrAddSection(
      &table, kActRaiserMapGroup_Aitos, 0x02,
      kDioramaLayerSection_AitosWaterfall);
  CHECK(waterfall != NULL);
  if (!waterfall) return;
  waterfall->planes[kDioramaPlane_Backdrop].set_source = true;
  waterfall->planes[kDioramaPlane_Backdrop].source =
      kDioramaLayerSource_AitosSky;

  DioramaEditorContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.room_live = true;
  ctx.map_group = kActRaiserMapGroup_Aitos;
  ctx.map_number = 0x02;
  ctx.section = kDioramaLayerSection_AitosWaterfall;
  ctx.selected_plane = kDioramaPlane_Backdrop;
  DioramaEditorRow rows[kDioramaEditorRowMax];
  const int level =
      DioramaLayerEditor_LevelIndexOfGroup(kActRaiserMapGroup_Aitos);
  int n = DioramaLayerEditor_BuildRows(
      &table, &ctx, level, rows, kDioramaEditorRowMax);
  CHECK(strstr(rows[0].label, "waterfall") != NULL);
  const DioramaEditorRow *source = FindRow(
      rows, n, kDioramaPlane_Backdrop, kDioramaEditorParam_Source);
  CHECK(source != NULL);
  CHECK(source && source->kind == kDioramaEditorRow_ParamEnum);
  CHECK(source && !strcmp(source->label, "skybox source"));
  CHECK(source && !strcmp(source->value, "ROM-04-01-BG2"));
  /* Other planes cannot offer a source row the manifest would reject. */
  ctx.selected_plane = kPpuOverlaySource_Bg1;
  n = DioramaLayerEditor_BuildRows(
      &table, &ctx, level, rows, kDioramaEditorRowMax);
  CHECK(!FindRow(rows, n, kPpuOverlaySource_Bg1,
                 kDioramaEditorParam_Source));

  /* A scoped record with no local source displays the renderer's inherited
   * base-room source, not a misleading raw zero/Captured value. */
  DioramaRoomOverride *base = DioramaLayerOrder_FindOrAdd(
      &table, kActRaiserMapGroup_Aitos, 0x02);
  CHECK(base != NULL);
  if (base) {
    waterfall->planes[kDioramaPlane_Backdrop].set_source = false;
    base->planes[kDioramaPlane_Backdrop].set_source = true;
    base->planes[kDioramaPlane_Backdrop].source =
        DioramaLayerOrder_ActionBgSource(0x06, 0x08, 1);
    ctx.selected_plane = kDioramaPlane_Backdrop;
    n = DioramaLayerEditor_BuildRows(
        &table, &ctx, level, rows, kDioramaEditorRowMax);
    source = FindRow(rows, n, kDioramaPlane_Backdrop,
                     kDioramaEditorParam_Source);
    CHECK(source && !strcmp(source->value, "ROM-06-08-BG1"));
    CHECK(source && source->effective_source ==
                        DioramaLayerOrder_ActionBgSource(0x06, 0x08, 1));
  }
}

/* The row array must never overflow, whatever the shape. The header promises a
 * caller can stack-allocate kDioramaEditorRowMax and be safe. */
static void TestRowsFitTheDocumentedMaximum(void) {
  DioramaLayerOrderTable table;
  memset(&table, 0, sizeof(table));
  DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
  CHECK(room != NULL);
  if (!room) return;

  DioramaEditorContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.room_live = true;
  ctx.map_group = kActRaiserMapGroup_Fillmore;
  ctx.map_number = 0x02;
  const int level =
      DioramaLayerEditor_LevelIndexOfGroup(kActRaiserMapGroup_Fillmore);

  /* Worst case: every plane authored with the shape that expands into the most
   * rows, and the cursor on each in turn. */
  for (int p = 0; p < kDioramaPlane_Count; p++) {
    if (!DioramaLayerOrder_PlaneToken(p)) continue;
    DioramaLayerEditor_SetStrategy(&room->planes[p], kDioramaDepth_Stack);
    DioramaLayerEditor_StepParam(&room->planes[p],
                                 kDioramaEditorParam_Density, +1);
  }
  for (int p = 0; p < kDioramaPlane_Count; p++) {
    ctx.selected_plane = p;
    DioramaEditorRow rows[kDioramaEditorRowMax];
    int n = DioramaLayerEditor_BuildRows(&table, &ctx, level, rows,
                                        kDioramaEditorRowMax);
    CHECK(n > 0);
    CHECK(n <= kDioramaEditorRowMax);
  }
}

/* Level tabs cover exactly the action groups: Diorama_IsActiveThisFrame rejects
 * groups $00 and $08, so those tabs could never show a
 * live room. */
static void TestLevelTabsAreTheActionGroups(void) {
  CHECK(kDioramaEditorLevelCount ==
        kActRaiserActionMapGroup_Last - kActRaiserActionMapGroup_First + 1);
  for (int i = 0; i < kDioramaEditorLevelCount; i++) {
    uint8_t group = DioramaLayerEditor_LevelGroup(i);
    CHECK(ActRaiser_IsActionMapGroup(group));
    CHECK(DioramaLayerEditor_LevelIndexOfGroup(group) == i);
    CHECK(DioramaLayerEditor_LevelName(i)[0] != '\0');
  }
  CHECK(DioramaLayerEditor_LevelIndexOfGroup(
            kActRaiserMapGroup_NonAction) < 0);
  CHECK(DioramaLayerEditor_LevelIndexOfGroup(kActRaiserMapGroup_Ending) < 0);
}

/* The menu's all-caps conversion. Its bounds matter more than its letters: it
 * writes into fixed row buffers, so a missing terminator or an overrun would
 * corrupt whatever follows in the row struct. */
static void TestUpperIsBoundedAndTerminated(void) {
  char out[8];
  DioramaLayerEditor_Upper(out, sizeof(out), "stack");
  CHECK(!strcmp(out, "STACK"));

  /* Truncation must still terminate: "backward" is 8 characters and the buffer
   * holds 7 plus a NUL. */
  DioramaLayerEditor_Upper(out, sizeof(out), "backward");
  CHECK(!strcmp(out, "BACKWAR"));
  CHECK(out[sizeof(out) - 1] == '\0');

  /* Degenerate inputs: NULL text yields an empty string rather than reading it,
   * and a zero-sized buffer is not written at all. */
  DioramaLayerEditor_Upper(out, sizeof(out), NULL);
  CHECK(out[0] == '\0');
  char guard = 'x';
  DioramaLayerEditor_Upper(&guard, 0, "stack");
  CHECK(guard == 'x');

  /* Non-letters pass through, since a value like "0.29" shares this path. */
  DioramaLayerEditor_Upper(out, sizeof(out), "b2h.9");
  CHECK(!strcmp(out, "B2H.9"));

  /* Every token the grammar can produce fits the row buffers it is written into,
   * so no live value is ever truncated -- the case above is deliberately a
   * smaller buffer than any real one. */
  char room[24];
  for (int s = 0; s < kDioramaDepth_StrategyCount; s++) {
    const char *name = DioramaLayerOrder_StrategyName((DioramaDepthStrategy)s);
    DioramaLayerEditor_Upper(room, sizeof(room), name);
    CHECK(strlen(room) == strlen(name));
  }
  for (int d = 0; d < kDioramaStack_DirectionCount; d++) {
    const char *token = DioramaLayerOrder_StackDirectionToken(d);
    DioramaLayerEditor_Upper(room, sizeof(room), token);
    CHECK(strlen(room) == strlen(token));
  }
}

/* A fully authored room must FIT the buffer diorama.c formats it into, or
 * Diorama_SaveLayerManifest skips the room and the author's work is gone on the
 * next save. That buffer was 1024 and the true worst case is 1488, so this pins
 * the number rather than trusting a guess.
 *
 * Every key at its widest rendering, which is what a HAND-EDITED manifest can
 * hold -- the editor itself authors one shape per plane and stays near 640. */
static void TestWorstCaseRoomFitsTheSaveBuffer(void) {
  /* Must track the `char text[]` in Diorama_SaveLayerManifest. Not shared as a
   * constant because that file is in no test binary; the comment there names this
   * test, so the two cannot drift silently. */
  enum { kSaveBufferBytes = 2048 };

  DioramaRoomOverride room;
  memset(&room, 0, sizeof(room));
  room.used = true;
  room.map_group = 0x01;
  room.map_number = 0x02;
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    DioramaPlaneOverride *p = &room.planes[plane];
    p->set_order = true;      p->order = kDioramaPlane_Count * 4 - 1;
    p->set_z = true;          p->z = -0.123456f;   /* widest %.4g */
    p->set_alpha = true;      p->alpha = kDioramaLayerAlphaOpaque;
    p->set_rake = true;       p->rake = -0.987654f;
    p->set_bow = true;        p->bow = -0.987654f;
    p->set_thickness = true;  p->thickness = 0.987654f;
    p->set_stack = true;      p->stack = 0.987654f;
    p->set_stack_copies = true;  p->stack_copies = kDioramaStackMax;
    /* Widest density the GRAMMAR accepts: positive (the parser rejects <= 0, and
     * the editor clamps to >= 1, so a negative one is unreachable and would make
     * this a test of an impossible state) and as many characters as %.4g emits. */
    p->set_stack_density = true; p->stack_density = 99.9876f;
    /* "backward" is the longest direction token. */
    p->set_stack_direction = true; p->stack_direction = kDioramaStack_Backward;
    p->set_voxel = true;      p->voxel = 0.987654f;
    p->set_voxel_copies = true;  p->voxel_copies = kDioramaVoxelMax;
  }

  char text[4096];
  size_t need = DioramaLayerOrder_FormatRoom(&room, text, sizeof(text));
  CHECK(need > 0);
  CHECK(need < sizeof(text));          /* the probe's own buffer sufficed */
  CHECK(need < (size_t)kSaveBufferBytes);   /* THE assertion */

  /* And it must reload: a room that fits but does not parse back is no better. */
  DioramaRoomOverride reloaded;
  memset(&reloaded, 0, sizeof(reloaded));
  reloaded.used = true;
  char *save = NULL;
  for (char *line = strtok_r(text, "\n", &save); line;
       line = strtok_r(NULL, "\n", &save)) {
    if (line[0] == '[' || !line[0]) continue;
    const char *error = NULL;
    CHECK(DioramaLayerOrder_ParseLine(&reloaded, line, &error));
  }
  CHECK(DioramaLayerOrder_RoomIsActive(&reloaded));
}

/* THE INVARIANT THIS MODULE EXISTS FOR, asserted after STEPPING rather than only
 * after authoring: what the editor's row says must be what the renderer draws.
 *
 * Two HIGH findings from the adversarial review both violated it, and both were
 * reachable by holding one key:
 *
 *   1. Copies stepped down to 1. The renderer gates the stack pass on
 *      `copies > 1` because one copy coincides with the plane's
 *      own draw, so `copies:1` rendered NOTHING while the row still said STACK.
 *   2. Depth stepped to exactly 0. The flag stayed set, so RoomIsActive (which
 *      tests the flag, not the value) kept the room authored and wrote `rake:0`
 *      to the manifest -- an entry with no effect and no visible row.
 *
 * The existing tests missed both because they only ever asserted on FRESHLY
 * AUTHORED planes, or asserted only that stepped values re-parse (and both
 * `copies:1` and `rake:0` parse fine). So this drives every parameter to both
 * bounds and checks the invariant at every step. */
static void TestSteppingNeverDisagreesWithTheRenderer(void) {
  static const DioramaEditorParam kParams[] = {
    kDioramaEditorParam_Depth, kDioramaEditorParam_Copies,
    kDioramaEditorParam_Density,
  };
  for (int s = kDioramaDepth_Rake; s < kDioramaDepth_StrategyCount; s++) {
    for (size_t k = 0; k < sizeof(kParams) / sizeof(kParams[0]); k++) {
      for (int dir = -1; dir <= 1; dir += 2) {
        DioramaLayerOrderTable table;
        memset(&table, 0, sizeof(table));
        DioramaRoomOverride *room =
            DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
        CHECK(room != NULL);
        if (!room) continue;
        DioramaPlaneOverride *plane = &room->planes[kDioramaPlane_Bg2Hi];
        DioramaLayerEditor_SetStrategy(plane, (DioramaDepthStrategy)s);

        /* Far more presses than any range needs, checking after EVERY one. */
        for (int press = 0; press < 200; press++) {
          DioramaLayerEditor_StepParam(plane, kParams[k], dir);

          DioramaResolvedLayer out[16];
          int n = DioramaLayerOrder_Resolve(&table, 0x01, 0x02, kDefaults,
                                           kDefaultCount, out, 16);
          const DioramaResolvedLayer *layer =
              FindResolved(out, n, kDioramaPlane_Bg2Hi);
          CHECK(layer != NULL);
          if (!layer) break;

          /* (1) The two verdicts must agree, always. */
          CHECK(DioramaLayerEditor_StrategyOfPlane(plane) ==
                DioramaLayerOrder_StrategyOf(layer));

          /* (2) Whatever shape is reported must actually DRAW. These mirror
           * Diorama_Composite's shape gates. */
          switch (DioramaLayerEditor_StrategyOfPlane(plane)) {
            case kDioramaDepth_Rake:  CHECK(layer->rake != 0.0f); break;
            case kDioramaDepth_Bow:   CHECK(layer->bow != 0.0f); break;
            case kDioramaDepth_Thick: CHECK(layer->thickness > 0.0f); break;
            case kDioramaDepth_Stack:
            case kDioramaDepth_Voxel:
              CHECK(layer->stack > 0.0f);
              CHECK(layer->stack_copies > 1);   /* the gate copies:1 failed */
              break;
            case kDioramaDepth_Flat:
              /* (3) And flat must be GENUINELY flat: not one shape key left
               * behind, or the room stays authored while drawing nothing. */
              CHECK(!DioramaLayerOrder_RoomIsActive(room) ||
                    plane->set_z || plane->set_alpha || plane->set_order);
              CHECK(layer->rake == 0.0f);
              CHECK(layer->bow == 0.0f);
              CHECK(layer->thickness == 0.0f);
              CHECK(layer->stack == 0.0f);
              break;
            default: break;
          }
        }
      }
    }
  }
}

/* The specific sequences from the review, kept as named cases so a failure says
 * which one broke rather than just "some step disagreed". */
static void TestSteppingToZeroClearsTheShape(void) {
  for (int s = kDioramaDepth_Rake; s < kDioramaDepth_StrategyCount; s++) {
    DioramaLayerOrderTable table;
    memset(&table, 0, sizeof(table));
    DioramaRoomOverride *room = DioramaLayerOrder_FindOrAdd(&table, 0x01, 0x02);
    CHECK(room != NULL);
    if (!room) continue;
    DioramaPlaneOverride *plane = &room->planes[kPpuOverlaySource_Bg1];
    DioramaLayerEditor_SetStrategy(plane, (DioramaDepthStrategy)s);

    /* Hold Left through zero. */
    for (int press = 0; press < 40; press++)
      DioramaLayerEditor_StepParam(plane, kDioramaEditorParam_Depth, -1);

    /* A rake and a bow are SIGNED -- a negative one tilts the bottom edge away,
     * the right shape for a ceiling -- so stepping down must pass THROUGH zero
     * into the negative half rather than stopping there. If it cleared at zero
     * instead, half of their authorable range would be unreachable from the UI
     * while still being authorable by hand. */
    if (s == kDioramaDepth_Rake || s == kDioramaDepth_Bow) {
      CHECK(DioramaLayerEditor_StrategyOfPlane(plane) ==
            (DioramaDepthStrategy)s);
      /* And it is genuinely on the far side of zero, not parked at it. */
      const float magnitude = (s == kDioramaDepth_Rake) ? plane->rake
                                                        : plane->bow;
      CHECK(magnitude < 0.0f);
    } else {
      CHECK(DioramaLayerEditor_StrategyOfPlane(plane) == kDioramaDepth_Flat);
      CHECK(!DioramaLayerOrder_RoomIsActive(room));
      /* And nothing is written for it -- the manifest gains no dead entry. */
      char text[512];
      CHECK(DioramaLayerOrder_FormatRoom(room, text, sizeof(text)) == 0);
    }
  }
}

static void TestStackCopiesNeverStepsBelowTwo(void) {
  DioramaPlaneOverride plane;
  memset(&plane, 0, sizeof(plane));
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Stack);
  for (int i = 0; i < 50; i++)
    DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Copies, -1);
  /* Two, not the parser's floor of one: one copy renders nothing. */
  CHECK(plane.stack_copies == 2);
  CHECK(!DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Copies, -1));

  /* The voxel arm always had this right; assert it so the two stay in step. */
  DioramaLayerEditor_SetStrategy(&plane, kDioramaDepth_Voxel);
  for (int i = 0; i < 50; i++)
    DioramaLayerEditor_StepParam(&plane, kDioramaEditorParam_Copies, -1);
  CHECK(plane.voxel_copies == 2);
}

int main(void) {
  TestSteppingNeverDisagreesWithTheRenderer();
  TestSteppingToZeroClearsTheShape();
  TestStackCopiesNeverStepsBelowTwo();
  TestWorstCaseRoomFitsTheSaveBuffer();
  TestUpperIsBoundedAndTerminated();
  TestEachStrategyResolvesToItself();
  TestCycleWrapsThroughEveryStrategy();
  TestAuthoredStrategyIsNonZero();
  TestMagnitudeCarriesBetweenRelatedShapes();
  TestStrategyRoundTripsThroughManifest();
  TestUndoLeavesRoomInactive();
  TestClearingDepthRemovesTheShape();
  TestSteppingStaysWithinParserBounds();
  TestStepReportsNoChangeAtBound();
  TestCopiesAndDensityAreMutuallyExclusive();
  TestStepSnapsToGrid();
  TestForeignLevelTabExplainsItself();
  TestLiveTabListsEveryPlane();
  TestParamRowsFollowTheActiveShape();
  TestUnauthoredKnobsShowDash();
  TestBackdropSourceIsScopedAndEditable();
  TestRowsFitTheDocumentedMaximum();
  TestLevelTabsAreTheActionGroups();

  if (g_failures) {
    printf("diorama layer editor: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("diorama layer editor: all checks passed\n");
  return 0;
}
