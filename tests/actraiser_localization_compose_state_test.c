#include "actraiser/actraiser_localization_compose_state.h"
#include "actraiser/actraiser_localization_hud.h"
#include "actraiser/actraiser_localization_style.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

static int failures;
static const uint16_t kTextPalette[4] = {0, 0, 0x7f33, 0x7fff};

#define CHECK(expression) do {                                             \
  if (!(expression)) {                                                     \
    fprintf(stderr, "%s:%d: check failed: %s\n",                          \
            __FILE__, __LINE__, #expression);                              \
    ++failures;                                                            \
  }                                                                        \
} while (0)

static bool ResolveSemanticId(
    void *context, const char *semantic_id,
    char *utf8, size_t utf8_capacity, size_t *utf8_bytes,
    uint32_t *cluster_count, uint64_t *source_revision,
    ArLocalizationInlineObjectSnapshot *inline_objects,
    size_t inline_object_capacity, uint8_t *inline_object_count,
    uint8_t *structural_boundaries, ArLocalizationTextLanguage *language, ArTextBidiSpans *bidi,
    char *error, size_t error_capacity) {
  (void)inline_objects;
  (void)inline_object_capacity;
  *inline_object_count = 0;
  bidi->count = 0;
  *language = (ArLocalizationTextLanguage){.locale = "en-US",
      .direction = kArTextDirection_LeftToRight};
  const char *rejected = (const char *)context;
  if (rejected && !strcmp(rejected, semantic_id)) {
    if (error && error_capacity)
      snprintf(error, error_capacity, "fixture rejected %s", semantic_id);
    return false;
  }
  const size_t length = strlen(semantic_id);
  if (length >= utf8_capacity) return false;
  memcpy(utf8, semantic_id, length + 1u);
  if (structural_boundaries) {
    memset(structural_boundaries, 0, AR_TEXT_BOUNDARY_BYTES(utf8_capacity));
    for (size_t i = 0; i < length; ++i)
      if (utf8[i] == '|' || utf8[i] == '\n')
        ArTextBoundary_Set(structural_boundaries, i, true);
  }
  *utf8_bytes = length;
  *cluster_count = (uint32_t)length;
  *source_revision = 1;
  return true;
}

typedef struct RevisionResolver {
  uint64_t revision;
  const char *suffix;
} RevisionResolver;

static bool ResolveRevision(
    void *context, const char *semantic_id,
    char *utf8, size_t utf8_capacity, size_t *utf8_bytes,
    uint32_t *cluster_count, uint64_t *source_revision,
    ArLocalizationInlineObjectSnapshot *inline_objects,
    size_t inline_object_capacity, uint8_t *inline_object_count,
    uint8_t *structural_boundaries, ArLocalizationTextLanguage *language, ArTextBidiSpans *bidi,
    char *error, size_t error_capacity) {
  (void)inline_objects;
  (void)structural_boundaries;
  (void)inline_object_capacity;
  *inline_object_count = 0;
  bidi->count = 0;
  *language = (ArLocalizationTextLanguage){.locale = "en-US",
      .direction = kArTextDirection_LeftToRight};
  (void)error;
  (void)error_capacity;
  const RevisionResolver *resolver = (const RevisionResolver *)context;
  const int written = snprintf(utf8, utf8_capacity, "%s:%s", semantic_id,
                               resolver->suffix);
  if (written <= 0 || (size_t)written >= utf8_capacity) return false;
  *utf8_bytes = (size_t)written;
  *cluster_count = (uint32_t)written;
  *source_revision = resolver->revision;
  return true;
}

static bool ResolveInlineObject(
    void *context, const char *semantic_id,
    char *utf8, size_t utf8_capacity, size_t *utf8_bytes,
    uint32_t *cluster_count, uint64_t *source_revision,
    ArLocalizationInlineObjectSnapshot *inline_objects,
    size_t inline_object_capacity, uint8_t *inline_object_count,
    uint8_t *structural_boundaries, ArLocalizationTextLanguage *language, ArTextBidiSpans *bidi,
    char *error, size_t error_capacity) {
  (void)context;
  (void)structural_boundaries;
  (void)semantic_id;
  (void)error;
  (void)error_capacity;
  bidi->count = 0;
  *language = (ArLocalizationTextLanguage){.locale = "en-US",
      .direction = kArTextDirection_LeftToRight};
  static const char text[] = "A\xE2\x80\x87" "B";
  if (sizeof(text) > utf8_capacity || !inline_object_capacity) return false;
  memcpy(utf8, text, sizeof(text));
  *utf8_bytes = sizeof(text) - 1u;
  *cluster_count = 3;
  *source_revision = 9;
  inline_objects[0] = (ArLocalizationInlineObjectSnapshot){
      kArLocalizationInlineObject_StatusLife, 4};
  *inline_object_count = 1;
  return true;
}

static ActRaiserLocalizationComposeObservation Compose(
    uint64_t serial, uint32_t source, uint16_t destination) {
  return (ActRaiserLocalizationComposeObservation){
      .struct_size = sizeof(ActRaiserLocalizationComposeObservation),
      .abi_version =
          ACTRAISER_LOCALIZATION_COMPOSE_OBSERVATION_ABI_VERSION,
      .serial = serial,
      .source_pc24 = source,
      .destination = destination,
      .map_number = kActRaiserNonActionMap_SkyPalace,
  };
}

static bool ResolveEmpty(
    void *context, const char *semantic_id,
    char *utf8, size_t utf8_capacity, size_t *utf8_bytes,
    uint32_t *cluster_count, uint64_t *source_revision,
    ArLocalizationInlineObjectSnapshot *inline_objects,
    size_t inline_object_capacity, uint8_t *inline_object_count,
    uint8_t *structural_boundaries, ArLocalizationTextLanguage *language, ArTextBidiSpans *bidi,
    char *error, size_t error_capacity) {
  if (!ResolveSemanticId(NULL, semantic_id, utf8, utf8_capacity, utf8_bytes,
                         cluster_count, source_revision, inline_objects,
                         inline_object_capacity, inline_object_count, structural_boundaries, language, bidi,
                         error, error_capacity))
    return false;
  utf8[0] = 0;
  *utf8_bytes = 0;
  *cluster_count = context ? 1 : 0; /* Malformed-empty fixture. */
  return true;
}

static void TestEmptyMenuLifecycle(void) {
  ActRaiserLocalizationComposeState state;
  ActRaiserLocalizationComposeState_Init(&state);
  ActRaiserLocalizationComposeState_SetScene(&state, 0, kActRaiserNonActionMap_SkyPalace);
  ActRaiserLocalizationComposeObservation event = Compose(10, 0x01F6C8, 0x0B17);
  char error[256] = {0};
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveEmpty, NULL, error, sizeof(error)));
  const ActRaiserLocalizationComposeSnapshot *slot =
      ActRaiserLocalizationComposeState_Find(&state, 8);
  CHECK(slot && slot->utf8_bytes == 0 && slot->cluster_count == 0);
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(
      &frame, "en", "test", UINT64_C(1), 1, &frame.settings));
  CHECK(ActRaiserLocalizationComposeState_AppendFrame(
      &state, &frame, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      kTextPalette));
  CHECK(frame.snapshot_count == 1 && frame.snapshots[0].utf8_bytes == 0);
  /* Empty, visible, and failed replacements share the same lifetime. */
  CHECK(ActRaiserLocalizationComposeState_RefreshLatest(
      &state, 8, ResolveSemanticId, NULL, error, sizeof(error)));
  CHECK(ActRaiserLocalizationComposeState_Find(&state, 8)->utf8_bytes > 0);
  CHECK(!ActRaiserLocalizationComposeState_RefreshLatest(
      &state, 8, ResolveEmpty, &state, error, sizeof(error)));
  CHECK(!ActRaiserLocalizationComposeState_Find(&state, 8));
  CHECK(ActRaiserLocalizationComposeState_RefreshLatest(
      &state, 8, ResolveEmpty, NULL, error, sizeof(error)));
  event = Compose(11, 0, 0);
  event.clear_first_column = frame.cells.records[0].region.column;
  event.clear_first_row = frame.cells.records[0].region.row;
  event.clear_column_count = event.clear_row_count = 1;
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, NULL, NULL, error, sizeof(error)));
  CHECK(!ActRaiserLocalizationComposeState_FindObserved(&state, 8));
  CHECK(!ActRaiserLocalizationComposeState_RefreshLatest(
      &state, 8, ResolveEmpty, NULL, error, sizeof(error)));
}

static void TestPartialMenuErases(void) {
  const struct {
    uint32_t source, surface, table;
    uint16_t destination;
  } menus[] = {
      {0x01F298, 2, 0, 0x0512},       /* Heading. */
      {0x01F058, 3, 0x01F04F, 0x0A12}, /* Magic selection. */
      {0x01F0B7, 3, 0x01F08E, 0x0A12}, /* Offering selection. */
      {0x01EF3B, 5, 0, 0x0703},       /* Keyboard. */
      {0x01F484, 6, 0, 0x060A},       /* Master. */
      {0x01F4DC, 7, 0, 0x0603},       /* Cities. */
      {0x01F5BC, 7, 0, 0x0603},       /* Score. */
      {0x01F6C8, 8, 0, 0x0B17},      /* Shared YES/NO. */
      {0x01FA9A, 9, 0, 0x0C12},      /* Message speed. */
  };
  for (size_t i = 0; i < sizeof(menus) / sizeof(menus[0]); ++i) {
    for (unsigned dormant = 0; dormant < 2; ++dormant) {
      ActRaiserLocalizationComposeState state;
      ActRaiserLocalizationComposeState_Init(&state);
      ActRaiserLocalizationComposeState_SetScene(&state, 0,
          kActRaiserNonActionMap_SkyPalace);
      char error[256];
      ActRaiserLocalizationComposeObservation event =
          Compose(10, menus[i].source, menus[i].destination);
      event.source_table_pc24 = menus[i].table;
      CHECK(ActRaiserLocalizationComposeState_Process(
          &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
      const ActRaiserLocalizationComposeSnapshot *snapshot =
          ActRaiserLocalizationComposeState_Find(&state, menus[i].surface);
      CHECK(snapshot != NULL);
      if (!snapshot) continue;
      const ArTextCellRegion region = snapshot->region;
      if (dormant) {
        const char *semantic_id = snapshot->semantic_id;
        CHECK(!ActRaiserLocalizationComposeState_RefreshLatest(
            &state, menus[i].surface, ResolveSemanticId, (void *)semantic_id,
            error, sizeof(error)));
        CHECK(ActRaiserLocalizationComposeState_FindObserved(
            &state, menus[i].surface));
      }
      /* A sibling/HUD clear outside the owned columns is not a menu close. */
      event = Compose(11, 0, 0);
      event.clear_first_row = region.row;
      event.clear_row_count = 1;
      event.clear_first_column = 0;
      event.clear_column_count = 1;
      CHECK(ActRaiserLocalizationComposeState_Process(
          &state, &event, NULL, NULL, error, sizeof(error)));
      CHECK(ActRaiserLocalizationComposeState_FindObserved(
          &state, menus[i].surface));
      /* Even a partial native erase retires the whole enhanced generation.
       * Dormant/fallback text must not reappear on a later language change. */
      event.serial = 12;
      event.clear_first_column = region.column;
      CHECK(ActRaiserLocalizationComposeState_Process(
          &state, &event, NULL, NULL, error, sizeof(error)));
      CHECK(!ActRaiserLocalizationComposeState_FindObserved(
          &state, menus[i].surface));
      CHECK(!ActRaiserLocalizationComposeState_RefreshLatest(
          &state, menus[i].surface, ResolveSemanticId, NULL, error, sizeof(error)));
      CHECK(!ActRaiserLocalizationComposeState_DialogueWasReplaced(&state, 10));
      ArLocalizationFrame frame;
      ArLocalizationFrame_Reset(&frame);
      CHECK(ActRaiserLocalizationComposeState_AppendFrame(
          &state, &frame,
          (ArTextCellDestination){3, kArTextCellScreen_Composited, 0x5800},
          kTextPalette));
      CHECK(frame.snapshot_count == 0);
      /* Erase followed by redraw in the same game frame creates a fresh owner. */
      event = Compose(13, menus[i].source, menus[i].destination);
      event.source_table_pc24 = menus[i].table;
      CHECK(ActRaiserLocalizationComposeState_Process(
          &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
      snapshot = ActRaiserLocalizationComposeState_Find(&state, menus[i].surface);
      CHECK(snapshot && snapshot->generation_serial == 13);
    }
  }
  /* A YES/NO close must keep its parent's heading and HUD, and must not
   * invalidate the completed save dialogue still waiting for acknowledgement. */
  ActRaiserLocalizationComposeState state;
  ActRaiserLocalizationComposeState_Init(&state);
  ActRaiserLocalizationComposeState_SetScene(&state, 0,
      kActRaiserNonActionMap_SkyPalace);
  char error[256];
  ActRaiserLocalizationComposeObservation event = Compose(20, 0x01F319, 0x0512);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  event = Compose(21, 0x01F1CB, 0x0106);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  event = Compose(22, 0x01F6C8, 0x0B17);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  event = Compose(23, 0, 0);
  event.clear_first_column = 23;
  event.clear_column_count = 3;
  event.clear_first_row = 10;
  event.clear_row_count = 2;
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, NULL, NULL, error, sizeof(error)));
  CHECK(ActRaiserLocalizationComposeState_Find(&state, 2));
  CHECK(ActRaiserLocalizationComposeState_Find(&state, 4));
  CHECK(!ActRaiserLocalizationComposeState_FindObserved(&state, 8));
  CHECK(!ActRaiserLocalizationComposeState_DialogueWasReplaced(&state, 22));
  event.clear_first_column = 31;
  CHECK(!ActRaiserLocalizationComposeState_Process(
      &state, &event, NULL, NULL, error, sizeof(error)));
}

static bool ResolveLiteral(
    void *context, const char *id, char *utf8, size_t capacity, size_t *bytes,
    uint32_t *clusters, uint64_t *revision,
    ArLocalizationInlineObjectSnapshot *objects, size_t object_capacity,
    uint8_t *object_count, uint8_t *structural_boundaries, ArLocalizationTextLanguage *language, ArTextBidiSpans *bidi,
    char *error, size_t error_capacity) {
  (void)id;
  return ResolveSemanticId(NULL, context, utf8, capacity, bytes, clusters,
      revision, objects, object_capacity, object_count, structural_boundaries, language, bidi,
      error, error_capacity);
}

static void TestActionAndTitle(void) {
  ActRaiserLocalizationComposeState state;
  ActRaiserLocalizationComposeState_Init(&state);
  ActRaiserLocalizationComposeState_SetScene(&state, 1, 1);
  const struct { uint32_t source; uint16_t dest; unsigned surface; } cards[] = {
    {0x00a851, 0x080b, 10}, {0x00a8cb, 0x0a0d, 11},
    {0x00a8d8, 0x0c0d, 12}, {0x00a8df, 0x090d, 13},
    {0x00a8e6, 0x090c, 13}, {0x00a8ef, 0x0b0d, 13},
  };
  char error[256];
  for (unsigned i = 0; i < sizeof(cards) / sizeof(cards[0]); ++i) {
    ActRaiserLocalizationComposeObservation event = Compose(i * 2 + 1, cards[i].source, cards[i].dest);
    event.map_group = event.map_number = 1;
    CHECK(ActRaiserLocalizationComposeState_Process(&state, &event,
        ResolveSemanticId, NULL, error, sizeof(error)));
    const ActRaiserLocalizationComposeSnapshot *slot =
        ActRaiserLocalizationComposeState_Find(&state, cards[i].surface);
    CHECK(slot && slot->layout == kArLocalizationTextLayout_CenteredLabel);
    if (!slot) continue;
    event.serial++;
    event.clear_first_row = slot->region.row;
    event.clear_first_column = slot->region.column;
    event.clear_row_count = event.clear_column_count = 1;
    CHECK(ActRaiserLocalizationComposeState_Process(&state, &event, NULL, NULL, error, sizeof(error)));
    CHECK(!ActRaiserLocalizationComposeState_FindObserved(&state, cards[i].surface));
  }
  ActRaiserLocalizationComposeState_SetScene(&state, 0, 0);
  const char *labels[] = {"  CONTINUE\n\n  NEW GAME", "Continuer\nNouvelle partie", "Continue", ""};
  for (unsigned i = 0; i < 4; ++i) {
    ActRaiserLocalizationComposeObservation event = Compose(20 + i, 0x02a9a7, 0x1100);
    event.map_number = 0;
    CHECK(ActRaiserLocalizationComposeState_Process(&state, &event,
        ResolveLiteral, (void *)labels[i], error, sizeof(error)));
    ArLocalizationFrame frame;
    ArLocalizationFrame_Reset(&frame);
    CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", UINT64_C(1), 1, &frame.settings));
    CHECK(ActRaiserLocalizationComposeState_AppendFrame(&state, &frame,
        (ArTextCellDestination){3, kArTextCellScreen_Composited, 0x5800}, kTextPalette));
    CHECK(frame.snapshot_count == 2);
    CHECK(frame.cells.records[0].region.row == 17 && frame.cells.records[1].region.row == 19);
    CHECK(frame.cells.records[0].region.column == 14 && frame.cells.records[0].region.rows == 1);
    CHECK((frame.snapshots[1].utf8_bytes == 0) == (i >= 2));
    if (i == 3) CHECK(!frame.snapshots[0].utf8_bytes);
  }
  ActRaiserLocalizationComposeObservation event = Compose(30, 0x02aa60, 0x110c);
  event.map_number = 0;
  CHECK(ActRaiserLocalizationComposeState_Process(&state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  CHECK(ActRaiserLocalizationComposeState_Find(&state, 15));
  event.source_pc24 = 0x02aa34;
  event.serial++;
  (void)ActRaiserLocalizationComposeState_Process(&state, &event, ResolveSemanticId, NULL, error, sizeof(error));
  CHECK(!ActRaiserLocalizationComposeState_FindObserved(&state, 15));
  CHECK(ActRaiserLocalizationComposeState_Find(&state, 14));
  ActRaiserLocalizationComposeState_SetScene(&state, 0, kActRaiserNonActionMap_SkyPalace);
  CHECK(!ActRaiserLocalizationComposeState_ActiveCount(&state));
}

static void TestActionHud(void) {
  uint16_t vram[0x8000] = {0}, cgram[256] = {0};
  const uint16_t base = 0x5800;
  const struct { unsigned row, col, count, first; } fields[] = {
    {1, 0, 6, 34}, {1, 11, 4, 1}, {1, 21, 4, 5},
    {2, 0, 6, 9}, {3, 0, 6, 15},
  };
  for (unsigned i = 0; i < 5; ++i)
    for (unsigned j = 0; j < fields[i].count; ++j)
      vram[base + fields[i].row * 32 + fields[i].col + j] = 0x2400 | (fields[i].first + j);
  vram[base + 32 + 25] = 0x2404;
  const unsigned digitcols[] = {8, 9, 15, 16, 17, 26, 27, 28, 29, 30};
  for (unsigned i = 0; i < 10; ++i) vram[base + 32 + digitcols[i]] = 0x2430 + i;
  cgram[6] = 0x001f; /* Palette one: exact red ends and green body. */
  cgram[7] = 0x03e0;
  /* Synthetic source pixels on both sides of each tile boundary. Adjacent
   * letter pixels immediately outside the ornament crop must not leak. */
  vram[0x22 * 8] = 0x0404;
  vram[0x23 * 8 + 1] = 0x8080;
  vram[0x23 * 8 + 2] = 0x0404; /* x=13, not left ornament */
  vram[0x26 * 8] = 0x0404;
  vram[0x27 * 8 + 1] = 0x1010;
  vram[0x26 * 8 + 2] = 0x0808; /* x=36, not right ornament */
  vram[0x27 * 8 + 2] = 0x0808; /* x=44, not right ornament */
  ActRaiserLocalizationHud hud = {0};
  ArLocalizationFrame frame;
  const ArTextCellDestination destination = {3, kArTextCellScreen_Composited, base};
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "ar", "test", UINT64_C(1), 1, &frame.settings));
  ActRaiserLocalizationHud_Append(&hud, &frame, destination,
      0, vram, 0x8000, cgram, 256, ResolveSemanticId, NULL);
  CHECK(frame.snapshot_count == 8 && hud.resolved);
  for (uint8_t i = 0; i < frame.snapshot_count; ++i) {
    CHECK(!strcmp(frame.snapshots[i].language.locale, "en-US"));
    CHECK(frame.snapshots[i].language.direction == kArTextDirection_LeftToRight);
  }
  CHECK(frame.snapshots[1].band_rgb == 0xff0000 && frame.snapshots[1].body_rgb == 0x00ff00);
  CHECK(frame.snapshots[1].style_id == kArTextStyle_RetailPaletteBands);
  CHECK(!frame.snapshots[1].italic && frame.snapshots[5].italic);
  CHECK(frame.snapshots[1].native_font_pixels == 7 && frame.snapshots[5].native_font_pixels == 8);
  CHECK(frame.snapshots[1].top_inset_pixels == 1 && frame.snapshots[5].top_inset_pixels == 0);
  CHECK(frame.snapshots[3].left_inset_pixels == 5 && frame.snapshots[3].right_inset_pixels == 4);
  CHECK(frame.snapshots[3].top_inset_pixels == 1);
  CHECK(frame.snapshots[5].language.direction == kArTextDirection_LeftToRight);
  CHECK(frame.snapshots[1].layout == kArLocalizationTextLayout_RightAlignedLabel);
  CHECK(frame.snapshots[1].right_inset_pixels == 6);
  CHECK(frame.snapshots[3].layout == kArLocalizationTextLayout_RightAlignedLabel);
  CHECK(frame.snapshots[5].layout == kArLocalizationTextLayout_SingleLineLabel);
  CHECK(frame.snapshots[6].layout == kArLocalizationTextLayout_SingleLineLabel);
  CHECK(frame.snapshots[6].left_inset_pixels == 1);
  CHECK(frame.snapshots[7].layout == kArLocalizationTextLayout_RightAlignedLabel);
  CHECK(frame.snapshots[7].right_inset_pixels == 1);
  CHECK(!strncmp(frame.text + frame.snapshots[7].utf8_offset, "56789", 5));
  CHECK(frame.cells.records[0].region.column == 0 && frame.cells.records[0].region.columns == 6);
  CHECK(frame.snapshots[0].layout == kArLocalizationTextLayout_FramedLabel);
  CHECK(frame.artwork[kArLocalizationArtwork_LabelFrameLeft].width == 8);
  CHECK(frame.artwork[kArLocalizationArtwork_LabelFrameRight].width == 7);
  const ArLocalizationArtwork *left = &frame.artwork[kArLocalizationArtwork_LabelFrameLeft];
  const ArLocalizationArtwork *right = &frame.artwork[kArLocalizationArtwork_LabelFrameRight];
  CHECK(left->argb[0] == 0xff00ff00 && left->argb[8 + 3] == 0xff00ff00);
  CHECK(right->argb[0] == 0xff00ff00 && right->argb[7 + 6] == 0xff00ff00);
  for (unsigned x = 0; x < 8; ++x) CHECK(!left->argb[16 + x]);
  for (unsigned x = 0; x < 7; ++x) CHECK(!right->argb[14 + x]);
  CHECK(frame.snapshots[2].layout == kArLocalizationTextLayout_LeftAlignedLabel);
  CHECK(!frame.snapshots[2].left_inset_pixels && !frame.snapshots[2].right_inset_pixels);
  CHECK(frame.cells.records[3].region.columns == 6); /* Bars, heart and magic stay native. */
  vram[base + 32 + 15] = 0x2499; /* Bad number retains native pixels. */
  cgram[6] = 0x7c00;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", UINT64_C(1), 1, &frame.settings));
  ActRaiserLocalizationHud_Append(&hud, &frame, destination,
      0, vram, 0x8000, cgram, 256, ResolveSemanticId, "action.hud.time_label");
  CHECK(frame.snapshot_count == 7); /* Resolver was not called again. */
  CHECK(frame.snapshots[1].band_rgb == 0x0000ff);
  memset(vram, 0, sizeof(vram));
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", UINT64_C(1), 1, &frame.settings));
  ActRaiserLocalizationHud_Append(&hud, &frame, destination,
      0, vram, 0x8000, cgram, 256, ResolveSemanticId, NULL);
  CHECK(!frame.snapshot_count); /* Clear/fade transition cannot leave stale HUD. */
}

static void TestSoundTestLifecycle(void) {
  ActRaiserLocalizationComposeState state;
  ActRaiserLocalizationComposeState_Init(&state);
  ActRaiserLocalizationComposeState_SetScene(&state, 1, 1);
  ActRaiserLocalizationComposeObservation event = Compose(30, 0x029871, 0x080B);
  event.map_group = event.map_number = 1;
  event.caller_pc24 = 0x0297F0;
  char error[256];
  RevisionResolver resolver = {1, "Music 01"};
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveRevision, &resolver, error, sizeof(error)));
  const uint32_t surface = kActRaiserLocalizationSoundTestSurface;
  const ActRaiserLocalizationComposeSnapshot *snapshot =
      ActRaiserLocalizationComposeState_Find(&state, surface);
  CHECK(snapshot && snapshot->layout == kArLocalizationTextLayout_Grid &&
        snapshot->grid.row_height == 2 && snapshot->region.columns == 10);
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "fr", "test", UINT64_C(1), 1,
                                  &frame.settings));
  CHECK(ActRaiserLocalizationComposeState_AppendFrame(&state, &frame,
      (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      kTextPalette));
  CHECK(frame.snapshot_count == 1 && frame.cells.records[0].region.row == 8);
  /* Numeric changes are native redraws. The cached immutable copy must not
   * continue displaying the previous counter. */
  resolver.suffix = "Music 22";
  ++event.serial;
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveRevision, &resolver, error, sizeof(error)));
  snapshot = ActRaiserLocalizationComposeState_Find(&state, surface);
  /* A live value changes without a pack content-revision change. */
  CHECK(snapshot && snapshot->source_revision == 1 && strstr(snapshot->utf8, "22"));
  for (unsigned native_only = 0; native_only < 2; ++native_only) {
    event.source_pc24 = 0x029871;
    event.caller_pc24 = 0x0297F0;
    ++event.serial;
    CHECK(ActRaiserLocalizationComposeState_Process(&state, &event,
        ResolveSemanticId, native_only ? "sound_test.menu.labels" : NULL,
        error, sizeof(error)) == !native_only);
    CHECK(ActRaiserLocalizationComposeState_FindObserved(&state, surface));
    event.source_pc24 = 0x029896;
    event.caller_pc24 = 0x029860;
    ++event.serial;
    CHECK(ActRaiserLocalizationComposeState_Process(
        &state, &event, NULL, NULL, error, sizeof(error)));
    CHECK(!ActRaiserLocalizationComposeState_FindObserved(&state, surface));
    CHECK(!ActRaiserLocalizationComposeState_RefreshLatest(
        &state, surface, ResolveSemanticId, NULL, error, sizeof(error)));
  }
  /* A scene boundary also retires the modal without depending on a close. */
  event.source_pc24 = 0x029871;
  event.caller_pc24 = 0x0297F0;
  ++event.serial;
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  ActRaiserLocalizationComposeState_SetScene(&state, 0, 7);
  CHECK(!ActRaiserLocalizationComposeState_FindObserved(&state, surface));
}

static void TestAppearance(void) {
  const char *const ids[] = {"city.fillmore.name", "sky.menu.magic.fire",
      "name_entry.keyboard", "action.hud.act_1", "action.hud.pause",
      "action.stage_name.fillmore", "title.save_choice.labels"};
  const uint16_t palettes[][4] = {{0, 0, 0x7f33, 0x7fff}, {0, 0x001f, 0x03e0, 0x7c00}};
  for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
    ActRaiserLocalizationComposeState state;
    ActRaiserLocalizationComposeState_Init(&state);
    ActRaiserLocalizationComposeSnapshot *slot = &state.surfaces[0];
    slot->active = true;
    slot->surface_id = kActRaiserLocalizationComposeSurfaceFirst;
    slot->region = (ArTextCellRegion){2, 4, 20, 6};
    slot->layout = kArLocalizationTextLayout_SingleLineLabel;
    slot->native_font_pixels = 8;
    slot->source_revision = 1;
    slot->language = (ArLocalizationTextLanguage){.locale = "en-US",
        .direction = kArTextDirection_LeftToRight};
    snprintf(slot->semantic_id, sizeof(slot->semantic_id), "%s", ids[i]);
    snprintf(slot->utf8, sizeof(slot->utf8), "Test 1");
    slot->utf8_bytes = slot->cluster_count = 6;
    for (unsigned p = 0; p < 2; ++p) {
      ArLocalizationFrame frame;
      ArLocalizationFrame_Reset(&frame);
      CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", 1, 1, &frame.settings));
      CHECK(ActRaiserLocalizationComposeState_AppendFrame(&state, &frame,
          (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
          palettes[p]));
      CHECK(frame.snapshot_count > 0);
      for (uint8_t s = 0; s < frame.snapshot_count; ++s) {
        const ArLocalizationTextSnapshot *text = &frame.snapshots[s];
        CHECK(text->shadow_enabled && text->slant_ascii_numerals);
        CHECK(text->style_id == kArTextStyle_RetailPaletteBands);
        CHECK(text->shadow_rgb == (p ? 0xff0000 : 0));
        CHECK(text->band_rgb == (p ? 0x00ff00 : 0x9cceff));
        CHECK(text->body_rgb == (p ? 0x0000ff : 0xffffff));
        CHECK(text->top_inset_pixels == (i == 0 ? 1 : 0));
      }
      CHECK(ArLocalizationFrame_AddDialogueWindow(&frame, 99,
          (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
          (ArTextCellRegion){2, 18, 28, 5}, "Test", 4, 2, 4, 1,
          kArTextDirection_LeftToRight, 8));
      ArLocalizationTextSnapshot *dialogue = &frame.snapshots[frame.snapshot_count - 1];
      CHECK(!dialogue->shadow_enabled);
      ActRaiserLocalizationStyle_Ordinary(dialogue, palettes[p]);
      CHECK(dialogue->shadow_enabled && dialogue->shadow_rgb == (p ? 0xff0000 : 0));
    }
  }
}

int main(void) {
  TestAppearance();
  TestSoundTestLifecycle();
  TestActionAndTitle();
  TestActionHud();
  TestEmptyMenuLifecycle();
  TestPartialMenuErases();
  ActRaiserLocalizationComposeState state;
  ActRaiserLocalizationComposeState_Init(&state);
  ActRaiserLocalizationComposeState_SetScene(
      &state, 0, kActRaiserNonActionMap_SkyPalace);
  char error[256];

  ActRaiserLocalizationComposeObservation event =
      Compose(10, 0x01F298, 0x0512);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  const ActRaiserLocalizationComposeSnapshot *snapshot =
      ActRaiserLocalizationComposeState_Find(&state, 2);
  CHECK(snapshot && !strcmp(snapshot->utf8,
                            "sky.menu.choice.movement"));
  CHECK(ActRaiserLocalizationComposeState_ActiveCount(&state) == 1);
  CHECK(ActRaiserLocalizationComposeState_ReleaseSurface(&state, 2));
  CHECK(!ActRaiserLocalizationComposeState_Find(&state, 2));
  CHECK(ActRaiserLocalizationComposeState_ActiveCount(&state) == 0);
  CHECK(!ActRaiserLocalizationComposeState_ReleaseSurface(&state, 1));
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  CHECK(ActRaiserLocalizationComposeState_DialogueWasReplaced(&state, 9));
  CHECK(!ActRaiserLocalizationComposeState_DialogueWasReplaced(&state, 10));

  event = Compose(11, 0x01F158, 0x0A12);
  event.source_table_pc24 = 0x01F08E;
  event.source_selector = 12;
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  snapshot = ActRaiserLocalizationComposeState_Find(&state, 3);
  CHECK(snapshot && !strcmp(snapshot->utf8,
                            "sim.menu.possession.slot_12"));
  CHECK(ActRaiserLocalizationComposeState_ActiveCount(&state) == 2);

  /* An unrouted native generation still clears the previous enhanced owner. */
  event = Compose(12, 0x01FFFF, 0x0A12);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  CHECK(!ActRaiserLocalizationComposeState_Find(&state, 3));
  CHECK(ActRaiserLocalizationComposeState_ActiveCount(&state) == 1);

  event = Compose(13, 0x01F4DC, 0x0603);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  snapshot = ActRaiserLocalizationComposeState_Find(&state, 7);
  CHECK(snapshot && !strcmp(snapshot->semantic_id,
                            "status.report.cities_report"));
  CHECK(snapshot && snapshot->menu ==
        kActRaiserLocalizationMenu_StatusCities);
  CHECK(snapshot && snapshot->layout == kArLocalizationTextLayout_Grid);
  CHECK(!ActRaiserLocalizationComposeState_Find(&state, 2));
  CHECK(ActRaiserLocalizationComposeState_ActiveCount(&state) == 1);

  RevisionResolver revision = {.revision = 2, .suffix = "updated"};
  CHECK(ActRaiserLocalizationComposeState_Refresh(
      &state, 7, 2, ResolveRevision, &revision, error, sizeof(error)));
  snapshot = ActRaiserLocalizationComposeState_Find(&state, 7);
  CHECK(snapshot && snapshot->source_revision == 2);
  CHECK(snapshot && !strcmp(snapshot->utf8,
                            "status.report.cities_report:updated"));
  const uint64_t generation = snapshot ? snapshot->generation_serial : 0;
  CHECK(ActRaiserLocalizationComposeState_Refresh(
      &state, 7, 2, ResolveRevision, &revision, error, sizeof(error)));
  snapshot = ActRaiserLocalizationComposeState_Find(&state, 7);
  CHECK(snapshot && snapshot->generation_serial == generation);

  revision.revision = 3;
  revision.suffix = "latest";
  CHECK(ActRaiserLocalizationComposeState_RefreshLatest(
      &state, 7, ResolveRevision, &revision, error, sizeof(error)));
  snapshot = ActRaiserLocalizationComposeState_Find(&state, 7);
  CHECK(snapshot && snapshot->source_revision == 3);
  CHECK(snapshot && !strcmp(snapshot->utf8,
                            "status.report.cities_report:latest"));

  event = Compose(14, 0x01F5BC, 0x0603);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  snapshot = ActRaiserLocalizationComposeState_Find(&state, 7);
  CHECK(snapshot && snapshot->menu ==
        kActRaiserLocalizationMenu_StatusScore);
  ArLocalizationFrame report_frame;
  ArLocalizationFrame_Reset(&report_frame);
  CHECK(ArLocalizationFrame_SetFont(
      &report_frame, "en-US", "test.font", UINT64_C(1), 1,
      &report_frame.settings));
  CHECK(ActRaiserLocalizationComposeState_AppendFrame(
      &state, &report_frame,
      (ArTextCellDestination){3, kArTextCellScreen_Composited, 0x5800},
      kTextPalette));
  CHECK(report_frame.snapshot_count == 1);
  CHECK(report_frame.snapshots[0].native_preserve_count == 1);
  CHECK(report_frame.snapshots[0].native_preserves[0].row == 11);
  CHECK(report_frame.snapshots[0].native_preserves[0].columns == 26);
  /* The divider comes from the grid's reserved rule, not a second copy of
   * the row number in the adapter. */
  CHECK(report_frame.snapshots[0].native_preserves[0].rows == 1);

  revision.revision = 4;
  CHECK(!ActRaiserLocalizationComposeState_Refresh(
      &state, 7, 3, ResolveRevision, &revision, error, sizeof(error)));
  CHECK(!ActRaiserLocalizationComposeState_Find(&state, 7));
  CHECK(strstr(error, "changed while") != NULL);

  event = Compose(15, 0x01F484, 0x060A);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveInlineObject, NULL, error, sizeof(error)));
  snapshot = ActRaiserLocalizationComposeState_Find(&state, 6);
  CHECK(snapshot && snapshot->inline_object_count == 1);
  CHECK(snapshot && snapshot->inline_objects[0].kind ==
        kArLocalizationInlineObject_StatusLife);
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(
      &frame, "en-US", "test.font", UINT64_C(1), 1,
      &frame.settings));
  CHECK(ActRaiserLocalizationComposeState_AppendFrame(
      &state, &frame,
      (ArTextCellDestination){3, kArTextCellScreen_Composited, 0x5800},
      kTextPalette));
  CHECK(frame.inline_object_count == 1);
  CHECK(frame.snapshots[0].inline_object_count == 1);

  /* Destination $0512 is a global menu generation boundary, not merely a
   * spatial replacement of the ten-cell heading. */
  event = Compose(16, 0x01F2CA, 0x0512);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  CHECK(ActRaiserLocalizationComposeState_Find(&state, 2));
  CHECK(!ActRaiserLocalizationComposeState_Find(&state, 7));
  CHECK(ActRaiserLocalizationComposeState_ActiveCount(&state) == 1);

  /* Failed replacement resolution remains visibly native and cannot leave a
   * stale enhanced label behind. */
  event = Compose(17, 0x01F298, 0x0512);
  CHECK(!ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId,
      "sky.menu.choice.movement", error, sizeof(error)));
  CHECK(!ActRaiserLocalizationComposeState_Find(&state, 2));
  CHECK(strstr(error, "fixture rejected") != NULL);

  /* Native presentation keeps no enhanced claim, but a live source identity
   * survives so enabling/recovering a pack needs no native composer redraw. */
  CHECK(ActRaiserLocalizationComposeState_FindObserved(&state, 2));
  CHECK(ActRaiserLocalizationComposeState_RefreshLatest(
      &state, 2, ResolveSemanticId, NULL, error, sizeof(error)));
  CHECK(ActRaiserLocalizationComposeState_Find(&state, 2));
  CHECK(!ActRaiserLocalizationComposeState_RefreshLatest(
      &state, 2, ResolveSemanticId, "sky.menu.choice.movement",
      error, sizeof(error)));
  CHECK(!ActRaiserLocalizationComposeState_Find(&state, 2));
  CHECK(ActRaiserLocalizationComposeState_FindObserved(&state, 2));
  CHECK(ActRaiserLocalizationComposeState_RefreshLatest(
      &state, 2, ResolveSemanticId, NULL, error, sizeof(error)));

  /* Native menu clear keeps the status strip but releases all intersecting
   * live and dormant generations, in order with later composition. */
  event = Compose(18, 0x01F1CB, 0x0106);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  CHECK(ActRaiserLocalizationComposeState_Find(&state, 4));
  snapshot = ActRaiserLocalizationComposeState_Find(&state, 4);
  CHECK(snapshot && snapshot->layout == kArLocalizationTextLayout_SingleLineLabel);
  CHECK(snapshot && snapshot->region.column == 6 && snapshot->region.row == 1 &&
        snapshot->region.columns == 12 && snapshot->region.rows == 1);
  event = Compose(19, 0, 0);
  event.clear_first_row = 4;
  event.clear_row_count = 28;
  event.clear_column_count = 32;
  event.clears_dialogue = true;
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, NULL, NULL, error, sizeof(error)));
  CHECK(ActRaiserLocalizationComposeState_Find(&state, 4));
  CHECK(!ActRaiserLocalizationComposeState_FindObserved(&state, 2));
  CHECK(ActRaiserLocalizationComposeState_DialogueWasReplaced(&state, 18));
  event = Compose(20, 0x01F298, 0x0512);
  CHECK(ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));
  CHECK(ActRaiserLocalizationComposeState_Find(&state, 2));

  ActRaiserLocalizationComposeState_SetScene(&state, 0, 1);
  CHECK(ActRaiserLocalizationComposeState_ActiveCount(&state) == 0);
  event = Compose(18, 0x01F298, 0x0512);
  CHECK(!ActRaiserLocalizationComposeState_FindObserved(&state, 2));
  CHECK(!ActRaiserLocalizationComposeState_Process(
      &state, &event, ResolveSemanticId, NULL, error, sizeof(error)));

  puts("localization fixed-composer lifecycle checks passed");
  return failures ? 1 : 0;
}
