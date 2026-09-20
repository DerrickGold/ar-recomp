#include "actraiser/actraiser_dialogue_adapter.h"
#include "localization/language_contract.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define AR_MEMBER_END(type, member)                                            \
  (offsetof(type, member) + sizeof(((type *)0)->member))

static void SetError(ArLanguagePackError *error, const char *format, ...) {
  if (!error)
    return;
  va_list args;
  va_start(args, format);
  vsnprintf(error->message, sizeof(error->message), format, args);
  va_end(args);
}

static bool RouteContract(const char *id, ArDialogueContract *contract,
                          ArLanguagePackError *error) {
  if (!id ||
      !ArLanguageContract_RouteAvailable(id, kArLanguageSourceProfile_Us)) {
    SetError(error, "unknown or unavailable U.S. dialogue route");
    return false;
  }
  memset(contract, 0, sizeof(*contract));
  contract->value_count = ArLanguageContract_AllowedPlaceholderCount(id);
  contract->control_count =
      ArLanguageContract_RequiredAnchorCount(id, kArLanguageSourceProfile_Us);
  if (contract->value_count > kArDialogueMaximumValues) {
    SetError(error, "%s requires too many dynamic values", id);
    return false;
  }
  for (uint32_t i = 0; i < contract->value_count; ++i) {
    const char *name = ArLanguageContract_AllowedPlaceholder(id, i);
    ArDialogueValueSpec *value = &contract->values[i];
    if (!name || strlen(name) >= sizeof(value->name)) {
      SetError(error, "%s has an invalid value contract", id);
      return false;
    }
    strcpy(value->name, name);
    value->kind = ArLanguageContract_PlaceholderKind(name);
  }
  return true;
}

static bool ValidatePackMetadataForRuntime(const ArLanguagePack *pack,
                                           bool required,
                                           ArLanguagePackError *error) {
  if (!pack)
    return !required;
  const ArLanguagePackMetadata *metadata = ArLanguagePack_GetMetadata(pack);
  if (!metadata || metadata->target != kArLanguagePackTarget_UsRuntime ||
      metadata->source_profile != kArLanguageSourceProfile_Us) {
    if (required)
      SetError(error, "runtime language pack must target the U.S. contract");
    return false;
  }
  return true;
}

static bool ValidateMessageForRuntime(const ArLanguagePack *pack,
                                      const char *semantic_id, bool required,
                                      ArLanguagePackError *error) {
  if (!ValidatePackMetadataForRuntime(pack, required, error))
    return false;
  ArLanguagePackError detail;
  if (!ArLanguageContract_ValidateMessage(pack, semantic_id, &detail)) {
    if (required)
      SetError(error, "%s", detail.message);
    return false;
  }
  return true;
}

static bool SelectSource(const ArDialogueContentSelection *selection,
                         const char *semantic_id, ArDialogueSource *source,
                         ArLanguagePackError *error) {
  memset(source, 0, sizeof(*source));
  if (!selection ||
      selection->struct_size <
          AR_MEMBER_END(ArDialogueContentSelection, native_us_enhanced_pack) ||
      selection->abi_version != AR_DIALOGUE_SESSION_ABI_VERSION ||
      selection->presentation < kArDialoguePresentation_NativeRetail ||
      selection->presentation > kArDialoguePresentation_Enhanced) {
    SetError(error, "invalid dialogue content selection");
    return false;
  }
  source->presentation = selection->presentation;
  if (selection->selected_pack &&
      selection->presentation != kArDialoguePresentation_Enhanced) {
    SetError(error, "community language packs require enhanced presentation");
    return false;
  }
  if (selection->selected_pack) {
    source->message =
        ArLanguagePack_FindMessage(selection->selected_pack, semantic_id);
    if (source->message) {
      if (!ValidateMessageForRuntime(selection->selected_pack, semantic_id,
                                     true, error))
        return false;
      source->effective_pack = selection->selected_pack;
      source->term_fallback_pack =
          ValidatePackMetadataForRuntime(selection->native_us_enhanced_pack,
                                         false, NULL)
              ? selection->native_us_enhanced_pack
              : NULL;
      source->resolved_source = kArDialogueResolvedSource_SelectedPack;
      return true;
    }
  }
  if ((!selection->selected_pack &&
       selection->presentation == kArDialoguePresentation_Enhanced) ||
      selection->selected_pack) {
    if (ValidateMessageForRuntime(selection->native_us_enhanced_pack,
                                  semantic_id, false, NULL)) {
      source->message = ArLanguagePack_FindMessage(
          selection->native_us_enhanced_pack, semantic_id);
      if (source->message) {
        source->effective_pack = selection->native_us_enhanced_pack;
        source->term_fallback_pack = selection->native_us_enhanced_pack;
        source->resolved_source = kArDialogueResolvedSource_NativeEnhanced;
        return true;
      }
    }
  }
  source->resolved_source = kArDialogueResolvedSource_NativeRom;
  return true;
}

bool ArDialogueSession_Begin(ArDialogueSession *session,
                             const ArDialogueContentSelection *selection,
                             const char *id,
                             const ArDialogueValueResolver *resolver,
                             ArLanguagePackError *error) {
  return ArDialogueSession_BeginBounded(session, selection, id, resolver, 0,
                                        error);
}

bool ArDialogueSession_BeginBounded(ArDialogueSession *session,
                                    const ArDialogueContentSelection *selection,
                                    const char *id,
                                    const ArDialogueValueResolver *resolver,
                                    size_t budget, ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
  ArDialogueContract contract;
  ArDialogueSource source;
  return RouteContract(id, &contract, error) &&
         SelectSource(selection, id, &source, error) &&
         ArDialogueSession_BeginSource(session, &source, &contract, id,
                                       resolver, budget, error);
}

bool ArDialogueSession_Switch(ArDialogueSession *session,
                              const ArDialogueContentSelection *selection,
                              ArLanguagePackError *error) {
  return ArDialogueSession_SwitchBounded(session, selection, 0, error);
}

bool ArDialogueSession_SwitchBounded(
    ArDialogueSession *session, const ArDialogueContentSelection *selection,
    size_t budget, ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
  ArDialogueStableState state;
  if (!ArDialogueSession_ExportState(session, &state)) {
    SetError(error, "cannot switch an inactive dialogue session");
    return false;
  }
  ArDialogueSource source;
  return SelectSource(selection, state.message_id, &source, error) &&
         ArDialogueSession_SwitchSource(session, &source, budget, error);
}

bool ArDialogueSession_Restore(ArDialogueSession *session,
                               const ArDialogueContentSelection *selection,
                               const ArDialogueStableState *state,
                               const ArDialogueValueResolver *resolver,
                               ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
  if (!state ||
      state->struct_size < AR_MEMBER_END(ArDialogueStableState, values) ||
      state->abi_version != AR_DIALOGUE_SESSION_ABI_VERSION ||
      !memchr(state->message_id, 0, sizeof(state->message_id))) {
    SetError(error, "saved dialogue state is invalid");
    return false;
  }
  ArDialogueContract contract;
  ArDialogueSource source;
  return RouteContract(state->message_id, &contract, error) &&
         SelectSource(selection, state->message_id, &source, error) &&
         ArDialogueSession_RestoreSource(session, &source, &contract, state,
                                         resolver, error);
}
