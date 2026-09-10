#ifndef AR_LOCALIZATION_LANGUAGE_CONTRACT_H
#define AR_LOCALIZATION_LANGUAGE_CONTRACT_H

#include "localization/language_pack.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum ArLanguagePlaceholderKind {
  kArLanguagePlaceholder_Unknown = 0,
  kArLanguagePlaceholder_LocalizedText,
  kArLanguagePlaceholder_LocalizedTerm,
  kArLanguagePlaceholder_Number,
  kArLanguagePlaceholder_Icon,
} ArLanguagePlaceholderKind;

typedef struct ArLanguageContractStats {
  uint32_t validated_messages;
  uint32_t aliases;
  uint32_t required_messages;
} ArLanguageContractStats;

/* Approves parsed content against the generated, address-free semantic
 * catalog. Runtime discovery must not expose a pack until this succeeds. */
bool ArLanguageContract_ValidatePack(const ArLanguagePack *pack,
                                     ArLanguageContractStats *stats,
                                     ArLanguagePackError *error);
/* Lightweight activation boundary for an already discovered pack. This
 * validates one resolved message (including aliases) without rescanning the
 * full catalog every time a dialogue box opens. */
bool ArLanguageContract_ValidateMessage(const ArLanguagePack *pack,
                                        const char *semantic_id,
                                        ArLanguagePackError *error);

uint32_t ArLanguageContract_RouteCount(void);
const char *ArLanguageContract_RouteId(uint32_t index);
bool ArLanguageContract_RouteAvailable(const char *semantic_id,
                                       ArLanguageSourceProfile profile);
uint32_t ArLanguageContract_AllowedPlaceholderCount(const char *semantic_id);
const char *ArLanguageContract_AllowedPlaceholder(const char *semantic_id,
                                                  uint32_t index);
uint32_t ArLanguageContract_RequiredAnchorCount(
    const char *semantic_id, ArLanguageSourceProfile profile);
const char *ArLanguageContract_RequiredAnchor(
    const char *semantic_id, ArLanguageSourceProfile profile, uint32_t index);
ArLanguagePlaceholderKind ArLanguageContract_PlaceholderKind(
    const char *placeholder);

#endif /* AR_LOCALIZATION_LANGUAGE_CONTRACT_H */
