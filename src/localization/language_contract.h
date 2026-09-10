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

/* How the game presents a route. This is the authoring-time half of what the
 * runtime actually does with a message, so a pack cannot pass validation with
 * content the game will never display.
 *   Flow     -- paginated dialogue: the game advances through every page.
 *   Fixed    -- one non-scrolling field, card, label or menu row; the composer
 *               reads the first page only.
 *   Keyboard -- the paged name-entry alphabet; the game selects pages itself.
 *   Inline   -- a term substituted into another message; never presented on
 *               its own line. */
typedef enum ArLanguagePresentationShape {
  kArLanguagePresentation_Flow = 0,
  kArLanguagePresentation_Fixed,
  kArLanguagePresentation_Keyboard,
  kArLanguagePresentation_Inline,
} ArLanguagePresentationShape;

/* Zero means "not constrained by the catalog"; it is never a limit of zero. */
typedef struct ArLanguagePresentationContract {
  ArLanguagePresentationShape shape;
  uint8_t maximum_pages;
  uint16_t maximum_lines;
  uint8_t required_nonempty_lines;
} ArLanguagePresentationContract;

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
/* False for an unknown route. */
bool ArLanguageContract_Presentation(const char *semantic_id,
                                     ArLanguagePresentationContract *out);

#endif /* AR_LOCALIZATION_LANGUAGE_CONTRACT_H */
