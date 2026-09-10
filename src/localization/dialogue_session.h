#ifndef AR_LOCALIZATION_DIALOGUE_SESSION_H
#define AR_LOCALIZATION_DIALOGUE_SESSION_H

#include "localization/language_contract.h"
#include "localization/text_boundaries.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AR_DIALOGUE_SESSION_ABI_VERSION UINT32_C(2)
#define AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION UINT32_C(2)

enum {
  kArDialogueMessageIdCapacity = 256,
  kArDialogueValueNameCapacity = 128,
  kArDialogueValueTextCapacity = 1024,
  kArDialogueMaximumValues = 32,
};

typedef enum ArDialoguePresentation {
  kArDialoguePresentation_NativeRetail = 0,
  kArDialoguePresentation_Enhanced,
} ArDialoguePresentation;

typedef enum ArDialogueResolvedSource {
  kArDialogueResolvedSource_NativeRom = 0,
  kArDialogueResolvedSource_NativeEnhanced,
  kArDialogueResolvedSource_SelectedPack,
} ArDialogueResolvedSource;

typedef struct ArDialogueValue {
  char name[kArDialogueValueNameCapacity];
  ArLanguagePlaceholderKind kind;
  int64_t number;
  /* LocalizedText stores snapshotted UTF-8; LocalizedTerm stores a stable
   * semantic term ID; Icon stores a renderer-neutral icon ID. */
  char text[kArDialogueValueTextCapacity];
} ArDialogueValue;

typedef bool (*ArDialogueResolveValue)(
    void *context, const char *name, ArLanguagePlaceholderKind expected_kind,
    ArDialogueValue *value, char *error, size_t error_capacity);

typedef bool (*ArDialogueFormatNumber)(
    void *context, const char *locale, int64_t number, unsigned minimum_digits,
    char *utf8,
    size_t utf8_capacity);

typedef struct ArDialogueValueResolver {
  size_t struct_size;
  uint32_t abi_version;
  void *context;
  ArDialogueResolveValue resolve;
  ArDialogueFormatNumber format_number;
} ArDialogueValueResolver;

typedef struct ArDialogueContentSelection {
  size_t struct_size;
  uint32_t abi_version;
  ArDialoguePresentation presentation;
  /* NULL selected_pack means native USA wording. Community packs must use
   * enhanced presentation. Both packs must already have passed the v1 loader;
   * this boundary re-runs semantic validation before activation. */
  const ArLanguagePack *selected_pack;
  const ArLanguagePack *native_us_enhanced_pack;
} ArDialogueContentSelection;

/* A portable host serializer must encode these named fields; do not persist a
 * raw struct image because size_t/bool layout and padding are platform ABI
 * details. */
typedef struct ArDialogueStableState {
  size_t struct_size;
  uint32_t abi_version;
  char message_id[kArDialogueMessageIdCapacity];
  char package_id[kArLanguagePackageIdCapacity];
  uint64_t source_revision;
  ArDialoguePresentation presentation;
  ArDialogueResolvedSource resolved_source;
  uint32_t authored_page_index;
  uint32_t revealed_cluster_count;
  uint32_t page_cluster_count;
  uint32_t completed_control_count;
  uint32_t completed_wait_count;
  uint32_t wait_frames_remaining;
  uint32_t wait_frames_total;
  bool control_pending;
  bool awaiting_input;
  bool awaiting_page_advance;
  bool terminal;
  uint32_t value_count;
  ArDialogueValue values[kArDialogueMaximumValues];
} ArDialogueStableState;

typedef struct ArDialogueInlineObject {
  size_t end_utf8_byte;
  const char *id;
} ArDialogueInlineObject;

typedef struct ArDialoguePageSnapshot {
  const char *utf8;
  /* Borrowed alongside utf8. Only authored literals/breaks can set these
   * bits; resolving a name, number, term or icon never creates structure. */
  const uint8_t *structural_boundaries;
  size_t utf8_bytes;
  size_t revealed_utf8_bytes;
  uint32_t page_index;
  uint32_t page_count;
  uint32_t revealed_cluster_count;
  uint32_t cluster_count;
  const ArDialogueInlineObject *inline_objects;
  size_t inline_object_count;
  uint64_t source_revision;
  const char *package_id;
  const char *locale;
  ArLanguageDirection direction;
} ArDialoguePageSnapshot;

/* Progress observed while the untouched ROM presenter owns the text box.
 * Native glyph counts are deliberately a numerator/denominator rather than a
 * Unicode index; switching maps that proportion onto complete enhanced
 * grapheme clusters. The native adapter reports game-visible controls only
 * after their effects have run. */
typedef struct ArDialogueNativeProgress {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t authored_page_index;
  uint32_t revealed_unit_count;
  uint32_t page_unit_count;
  uint32_t completed_control_count;
  uint32_t completed_wait_count;
  uint32_t wait_frames_remaining;
  uint32_t wait_frames_total;
  bool control_pending;
  bool awaiting_input;
  bool awaiting_page_advance;
  bool terminal;
} ArDialogueNativeProgress;

typedef enum ArDialogueTokenKind {
  kArDialogueToken_Blocked = 0,
  kArDialogueToken_NativeAdapter,
  kArDialogueToken_Grapheme,
  kArDialogueToken_WaitStarted,
  kArDialogueToken_Control,
  kArDialogueToken_PageComplete,
  kArDialogueToken_End,
} ArDialogueTokenKind;

typedef struct ArDialogueToken {
  ArDialogueTokenKind kind;
  uint32_t first_scalar;
  size_t end_utf8_byte;
  uint32_t wait_frames;
  const char *control_id;
  uint32_t control_ordinal;
} ArDialogueToken;

typedef struct ArDialogueSession {
  ArDialogueStableState state;
  void *private_program;
  uint32_t private_cue_index;
  size_t private_revealed_utf8_bytes;
  ArDialogueValueResolver private_resolver;
  bool private_has_resolver;
  bool private_native_observation_seen;
  uint32_t private_magic;
} ArDialogueSession;

void ArDialogueSession_Init(ArDialogueSession *session);
void ArDialogueSession_Destroy(ArDialogueSession *session);

bool ArDialogueSession_Begin(ArDialogueSession *session,
                             const ArDialogueContentSelection *selection,
                             const char *semantic_id,
                             const ArDialogueValueResolver *resolver,
                             ArLanguagePackError *error);

/* Transactional live switch. Existing stable state and active wait/input are
 * retained; derived pages are rebuilt and reveal/page progress is clamped. */
bool ArDialogueSession_Switch(ArDialogueSession *session,
                              const ArDialogueContentSelection *selection,
                              ArLanguagePackError *error);

/* Production adapters may impose a smaller presentation budget than the
 * portable compiler. Count resolved UTF-8 plus one separator per authored
 * page (including captured dynamic values). Zero keeps the portable limits.
 * Both operations reject over-budget candidates before mutating the session. */
bool ArDialogueSession_BeginBounded(
    ArDialogueSession *session, const ArDialogueContentSelection *selection,
    const char *semantic_id, const ArDialogueValueResolver *resolver,
    size_t maximum_text_bytes, ArLanguagePackError *error);
bool ArDialogueSession_SwitchBounded(
    ArDialogueSession *session, const ArDialogueContentSelection *selection,
    size_t maximum_text_bytes, ArLanguagePackError *error);

bool ArDialogueSession_Next(ArDialogueSession *session,
                            ArDialogueToken *token,
                            ArLanguagePackError *error);
/* Locked controls use two-phase delivery: Next exposes the stable control,
 * then the native adapter acknowledges it only after applying the effect.
 * Switching before acknowledgement presents the same control in the new
 * source instead of incorrectly treating it as completed. Page/token string
 * pointers remain owned by the session and expire on its next switch, begin,
 * restore, or destruction. */
bool ArDialogueSession_CompleteControl(ArDialogueSession *session,
                                       uint32_t control_ordinal);
void ArDialogueSession_TickWait(ArDialogueSession *session, uint32_t frames);
bool ArDialogueSession_ResumeInput(ArDialogueSession *session);
bool ArDialogueSession_AdvancePage(ArDialogueSession *session);
bool ArDialogueSession_GetPage(const ArDialogueSession *session,
                               ArDialoguePageSnapshot *page);
/* Read one complete authored page without mutating reveal or dialogue state.
 * Fixed semantic UIs such as the name-entry keyboard use this to select among
 * pack-defined pages while the untouched native loop remains authoritative. */
bool ArDialogueSession_GetAuthoredPage(const ArDialogueSession *session,
                                       uint32_t page_index,
                                       ArDialoguePageSnapshot *page);
/* Resolve a locked control to an exact UTF-8 boundary in the selected source.
 * Read-only and address-free; outputs are unchanged when unavailable. */
bool ArDialogueSession_GetControlPosition(const ArDialogueSession *session,
                                          uint32_t control_ordinal,
                                          uint32_t *page_index,
                                          size_t *utf8_offset);

/* Valid only while NativeRom is the resolved source. This is transactional:
 * malformed or regressive observations leave the prior state untouched. */
bool ArDialogueSession_ObserveNativeProgress(
    ArDialogueSession *session, const ArDialogueNativeProgress *progress,
    ArLanguagePackError *error);

/* Synchronize an enhanced presentation to the untouched native interpreter.
 * Locked controls bound reveal and take precedence over page ratios.
 * Authored pages are mapped by index and clamped when pack/native page counts
 * differ. Native decoder progress is a ratio, never a Unicode byte index. */
bool ArDialogueSession_SynchronizeNativeProgress(
    ArDialogueSession *session, const ArDialogueNativeProgress *progress,
    ArLanguagePackError *error);

bool ArDialogueSession_ExportState(const ArDialogueSession *session,
                                   ArDialogueStableState *state);
bool ArDialogueSession_Restore(ArDialogueSession *session,
                               const ArDialogueContentSelection *selection,
                               const ArDialogueStableState *state,
                               const ArDialogueValueResolver *resolver,
                               ArLanguagePackError *error);

#endif /* AR_LOCALIZATION_DIALOGUE_SESSION_H */
