#ifndef ACTRAISER_LOCALIZATION_VALUES_H
#define ACTRAISER_LOCALIZATION_VALUES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization/dialogue_session.h"
#include "localization/language_pack.h"

#define ACTRAISER_LOCALIZATION_VALUES_ABI_VERSION UINT32_C(1)

enum {
  kActRaiserLocalizationMasterNameCapacity =
      kArDialogueValueTextCapacity,
};

/* Immutable resolver inputs for one game-thread capture. Pack and WRAM remain
 * adapter-owned; dialogue sessions copy every resolved value during Begin. */
typedef struct ActRaiserLocalizationValues {
  size_t struct_size;
  uint32_t abi_version;
  const uint8_t *wram;
  size_t wram_bytes;
  const ArLanguagePack *pack;
  char master_name[kActRaiserLocalizationMasterNameCapacity];
} ActRaiserLocalizationValues;

bool ActRaiserLocalizationValues_Capture(
    ActRaiserLocalizationValues *values,
    const uint8_t *wram, size_t wram_bytes,
    const ArLanguagePack *pack, const char *master_name);

/* Matches ArDialogueResolveValue. */
bool ActRaiserLocalizationValues_Resolve(
    void *context, const char *name,
    ArLanguagePlaceholderKind expected_kind,
    ArDialogueValue *value, char *error, size_t error_capacity);

/* Hash of every live input used by the three scoped status reports. This lets
 * their persistent surfaces refresh only when presentation content changes. */
uint64_t ActRaiserLocalizationValues_ReportRevision(
    const ActRaiserLocalizationValues *values);

#endif /* ACTRAISER_LOCALIZATION_VALUES_H */
