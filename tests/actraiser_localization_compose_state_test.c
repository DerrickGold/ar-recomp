#include "actraiser/actraiser_localization_compose_state.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

static int failures;

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
    char *error, size_t error_capacity) {
  (void)inline_objects;
  (void)inline_object_capacity;
  *inline_object_count = 0;
  const char *rejected = (const char *)context;
  if (rejected && !strcmp(rejected, semantic_id)) {
    if (error && error_capacity)
      snprintf(error, error_capacity, "fixture rejected %s", semantic_id);
    return false;
  }
  const size_t length = strlen(semantic_id);
  if (length >= utf8_capacity) return false;
  memcpy(utf8, semantic_id, length + 1u);
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
    char *error, size_t error_capacity) {
  (void)inline_objects;
  (void)inline_object_capacity;
  *inline_object_count = 0;
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
    char *error, size_t error_capacity) {
  (void)context;
  (void)semantic_id;
  (void)error;
  (void)error_capacity;
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
          kArTextDirection_LeftToRight));
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

int main(void) {
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
  CHECK(snapshot && snapshot->layout ==
        kArLocalizationTextLayout_StatusCities);
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
  CHECK(snapshot && snapshot->layout ==
        kArLocalizationTextLayout_StatusScore);
  ArLocalizationFrame report_frame;
  ArLocalizationFrame_Reset(&report_frame);
  CHECK(ArLocalizationFrame_SetFont(
      &report_frame, "en-US", "test.font", "/tmp/test.ttf", 1,
      &report_frame.settings));
  CHECK(ActRaiserLocalizationComposeState_AppendFrame(
      &state, &report_frame,
      (ArTextCellDestination){3, kArTextCellScreen_Composited, 0x5800},
      kArTextDirection_LeftToRight));
  CHECK(report_frame.snapshot_count == 1);
  CHECK(report_frame.snapshots[0].native_preserve_count == 1);
  CHECK(report_frame.snapshots[0].native_preserves[0].row == 11);
  CHECK(report_frame.snapshots[0].native_preserves[0].columns == 26);

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
      &frame, "en-US", "test.font", "/tmp/test.ttf", 1,
      &frame.settings));
  CHECK(ActRaiserLocalizationComposeState_AppendFrame(
      &state, &frame,
      (ArTextCellDestination){3, kArTextCellScreen_Composited, 0x5800},
      kArTextDirection_LeftToRight));
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
