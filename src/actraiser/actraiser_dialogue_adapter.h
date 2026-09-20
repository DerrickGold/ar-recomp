#ifndef ACTRAISER_DIALOGUE_ADAPTER_H
#define ACTRAISER_DIALOGUE_ADAPTER_H

#include "localization/dialogue_session.h"

/* ActRaiser's US/native/community selection and generated route policy. The
 * session mechanism itself accepts any host-provided source and contract. */
typedef struct ArDialogueContentSelection {
  size_t struct_size;
  uint32_t abi_version;
  ArDialoguePresentation presentation;
  /* NULL selected_pack means native USA wording. Community packs must use
   * enhanced presentation. Both packs must already have passed the pack loader;
   * this boundary re-runs semantic validation before activation. */
  const ArLanguagePack *selected_pack;
  const ArLanguagePack *native_us_enhanced_pack;
} ArDialogueContentSelection;

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
bool ArDialogueSession_BeginBounded(ArDialogueSession *session,
                                    const ArDialogueContentSelection *selection,
                                    const char *semantic_id,
                                    const ArDialogueValueResolver *resolver,
                                    size_t maximum_text_bytes,
                                    ArLanguagePackError *error);
bool ArDialogueSession_SwitchBounded(
    ArDialogueSession *session, const ArDialogueContentSelection *selection,
    size_t maximum_text_bytes, ArLanguagePackError *error);

bool ArDialogueSession_Restore(ArDialogueSession *session,
                               const ArDialogueContentSelection *selection,
                               const ArDialogueStableState *state,
                               const ArDialogueValueResolver *resolver,
                               ArLanguagePackError *error);

#endif
