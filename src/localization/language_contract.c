#include "localization/language_contract.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct ArGeneratedPlaceholder {
  const char *name;
  ArLanguagePlaceholderKind kind;
} ArGeneratedPlaceholder;

typedef struct ArGeneratedContract {
  uint8_t profile;
  uint16_t anchor_first;
  uint16_t anchor_count;
} ArGeneratedContract;

typedef struct ArGeneratedRoute {
  const char *id;
  uint16_t placeholder_first;
  uint16_t placeholder_count;
  uint16_t contract_first;
  uint16_t contract_count;
  uint8_t profile_mask;
  uint8_t canonical_profile;
} ArGeneratedRoute;

#include "localization/language_contract_data.inc"

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

_Static_assert(ARRAY_COUNT(kGeneratedRoutes) == 531,
               "v1 semantic route count changed");
_Static_assert(ARRAY_COUNT(kGeneratedPlaceholders) == 60,
               "v1 placeholder count changed");

static void SetError(ArLanguagePackError *error, const char *format, ...) {
  if (!error)
    return;
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(error->message, sizeof(error->message), format, arguments);
  va_end(arguments);
}

static const ArGeneratedRoute *FindRoute(const char *semantic_id) {
  if (!semantic_id)
    return NULL;
  size_t low = 0;
  size_t high = ARRAY_COUNT(kGeneratedRoutes);
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int comparison = strcmp(semantic_id, kGeneratedRoutes[middle].id);
    if (comparison == 0)
      return &kGeneratedRoutes[middle];
    if (comparison < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return NULL;
}

static int FindPlaceholderIndex(const char *placeholder) {
  if (!placeholder)
    return -1;
  size_t low = 0;
  size_t high = ARRAY_COUNT(kGeneratedPlaceholders);
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int comparison =
        strcmp(placeholder, kGeneratedPlaceholders[middle].name);
    if (comparison == 0)
      return (int)middle;
    if (comparison < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return -1;
}

static bool PlaceholderAllowed(const ArGeneratedRoute *route,
                               const char *placeholder) {
  const int placeholder_index = FindPlaceholderIndex(placeholder);
  if (placeholder_index < 0)
    return false;
  for (uint32_t i = 0; i < route->placeholder_count; i++) {
    if (kGeneratedRoutePlaceholders[route->placeholder_first + i] ==
        (uint16_t)placeholder_index)
      return true;
  }
  return false;
}

static const ArGeneratedContract *FindContract(
    const ArGeneratedRoute *route, ArLanguageSourceProfile profile) {
  for (uint32_t i = 0; i < route->contract_count; i++) {
    const ArGeneratedContract *contract =
        &kGeneratedContracts[route->contract_first + i];
    if (contract->profile == (uint8_t)profile)
      return contract;
  }
  for (uint32_t i = 0; i < route->contract_count; i++) {
    const ArGeneratedContract *contract =
        &kGeneratedContracts[route->contract_first + i];
    if (contract->profile == route->canonical_profile)
      return contract;
  }
  return NULL;
}

static const ArLanguageMessage *ResolveAlias(const ArLanguagePack *pack,
                                             const ArLanguageMessage *message) {
  const ArLanguageMessage *cursor = message;
  for (uint32_t depth = 0; cursor && cursor->is_alias &&
                           depth <= ArLanguagePack_MessageCount(pack);
       depth++) {
    cursor = ArLanguagePack_FindMessage(
        pack, ArLanguagePack_GetString(pack, cursor->alias));
  }
  return cursor && !cursor->is_alias ? cursor : NULL;
}

static bool ValidateBody(const ArLanguagePack *pack,
                         const ArGeneratedRoute *route,
                         const ArGeneratedContract *contract,
                         const ArLanguageMessage *body,
                         const char *diagnostic_id,
                         ArLanguagePackError *error) {
  uint32_t anchor_index = 0;
  for (uint32_t i = 0; i < body->operation_count; i++) {
    const ArLanguageOperation *operation =
        ArLanguagePack_GetOperation(pack, body, i);
    if (!operation) {
      SetError(error, "%s: invalid operation range", diagnostic_id);
      return false;
    }
    if (operation->kind == kArLanguageOperation_Placeholder) {
      const char *placeholder =
          ArLanguagePack_GetString(pack, operation->value.placeholder);
      if (!PlaceholderAllowed(route, placeholder)) {
        SetError(error, "%s: placeholder {%s} is unavailable on this route",
                 diagnostic_id, placeholder ? placeholder : "");
        return false;
      }
      if (operation->minimum_digits &&
          (operation->minimum_digits > 9 ||
           ArLanguageContract_PlaceholderKind(placeholder) !=
               kArLanguagePlaceholder_Number)) {
        SetError(error, "%s: number format requires a numeric placeholder",
                 diagnostic_id);
        return false;
      }
    } else if (operation->kind == kArLanguageOperation_Anchor) {
      const char *anchor =
          ArLanguagePack_GetString(pack, operation->value.anchor);
      if (anchor_index >= contract->anchor_count ||
          strcmp(anchor ? anchor : "",
                 kGeneratedAnchorNames[contract->anchor_first + anchor_index]) !=
              0) {
        SetError(error, "%s: locked anchors changed at anchor %u",
                 diagnostic_id, anchor_index);
        return false;
      }
      anchor_index++;
    } else if (operation->kind == kArLanguageOperation_Event) {
      const char *event =
          ArLanguagePack_GetString(pack, operation->value.event.id);
      SetError(error, "%s: event '%s' is not allow-listed", diagnostic_id,
               event ? event : "");
      return false;
    }
  }
  if (anchor_index != contract->anchor_count) {
    SetError(error, "%s: locked anchors changed; expected %u, found %u",
             diagnostic_id, contract->anchor_count, anchor_index);
    return false;
  }
  return true;
}

bool ArLanguageContract_ValidatePack(const ArLanguagePack *pack,
                                     ArLanguageContractStats *stats,
                                     ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
  ArLanguageContractStats result = {0};
  const ArLanguagePackMetadata *metadata = ArLanguagePack_GetMetadata(pack);
  if (!metadata) {
    SetError(error, "language pack is not initialized");
    return false;
  }
  if ((uint32_t)metadata->source_profile >
      (uint32_t)kArLanguageSourceProfile_Japanese) {
    SetError(error, "language pack has an invalid source profile");
    return false;
  }

  for (uint32_t i = 0; i < ArLanguagePack_MessageCount(pack); i++) {
    const ArLanguageMessage *message = ArLanguagePack_GetMessage(pack, i);
    const char *semantic_id = ArLanguagePack_GetString(pack, message->id);
    const ArGeneratedRoute *route = FindRoute(semantic_id);
    if (!route) {
      SetError(error, "unknown semantic message '%s'",
               semantic_id ? semantic_id : "");
      return false;
    }
    const ArGeneratedContract *contract =
        FindContract(route, metadata->source_profile);
    if (!contract) {
      SetError(error, "%s: semantic route has no usable control contract",
               semantic_id);
      return false;
    }
    const ArLanguageMessage *body = ResolveAlias(pack, message);
    if (!body) {
      SetError(error, "%s: alias could not be resolved", semantic_id);
      return false;
    }
    if (!ValidateBody(pack, route, contract, body, semantic_id, error))
      return false;
    result.validated_messages++;
    if (message->is_alias)
      result.aliases++;
  }

  const uint8_t profile_bit = (uint8_t)(1u << metadata->source_profile);
  for (uint32_t i = 0; i < ARRAY_COUNT(kGeneratedRoutes); i++) {
    if (!(kGeneratedRoutes[i].profile_mask & profile_bit))
      continue;
    result.required_messages++;
    if (metadata->coverage == kArLanguagePackCoverage_Complete &&
        !ArLanguagePack_FindMessage(pack, kGeneratedRoutes[i].id)) {
      SetError(error,
               "complete pack is missing a required message; first is %s",
               kGeneratedRoutes[i].id);
      return false;
    }
  }
  if (stats)
    *stats = result;
  return true;
}

bool ArLanguageContract_ValidateMessage(const ArLanguagePack *pack,
                                        const char *semantic_id,
                                        ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
  const ArLanguagePackMetadata *metadata = ArLanguagePack_GetMetadata(pack);
  if (!metadata || !semantic_id ||
      (uint32_t)metadata->source_profile >
          (uint32_t)kArLanguageSourceProfile_Japanese) {
    SetError(error, "language pack or semantic message is invalid");
    return false;
  }
  const ArGeneratedRoute *route = FindRoute(semantic_id);
  const ArLanguageMessage *message =
      ArLanguagePack_FindMessage(pack, semantic_id);
  if (!route || !message) {
    SetError(error, "semantic message '%s' is unavailable",
             semantic_id ? semantic_id : "");
    return false;
  }
  const ArGeneratedContract *contract =
      FindContract(route, metadata->source_profile);
  const ArLanguageMessage *body = ResolveAlias(pack, message);
  if (!contract || !body) {
    SetError(error, "%s: semantic contract or alias is unavailable",
             semantic_id);
    return false;
  }
  return ValidateBody(pack, route, contract, body, semantic_id, error);
}

uint32_t ArLanguageContract_RouteCount(void) {
  return (uint32_t)ARRAY_COUNT(kGeneratedRoutes);
}

const char *ArLanguageContract_RouteId(uint32_t index) {
  return index < ARRAY_COUNT(kGeneratedRoutes) ? kGeneratedRoutes[index].id
                                               : NULL;
}

bool ArLanguageContract_RouteAvailable(const char *semantic_id,
                                       ArLanguageSourceProfile profile) {
  const ArGeneratedRoute *route = FindRoute(semantic_id);
  if (!route || (uint32_t)profile > (uint32_t)kArLanguageSourceProfile_Japanese)
    return false;
  return (route->profile_mask & (uint8_t)(1u << profile)) != 0;
}

uint32_t ArLanguageContract_AllowedPlaceholderCount(const char *semantic_id) {
  const ArGeneratedRoute *route = FindRoute(semantic_id);
  return route ? route->placeholder_count : 0;
}

const char *ArLanguageContract_AllowedPlaceholder(const char *semantic_id,
                                                  uint32_t index) {
  const ArGeneratedRoute *route = FindRoute(semantic_id);
  if (!route || index >= route->placeholder_count)
    return NULL;
  const uint16_t placeholder =
      kGeneratedRoutePlaceholders[route->placeholder_first + index];
  return placeholder < ARRAY_COUNT(kGeneratedPlaceholders)
             ? kGeneratedPlaceholders[placeholder].name
             : NULL;
}

uint32_t ArLanguageContract_RequiredAnchorCount(
    const char *semantic_id, ArLanguageSourceProfile profile) {
  const ArGeneratedRoute *route = FindRoute(semantic_id);
  if (!route || (uint32_t)profile > (uint32_t)kArLanguageSourceProfile_Japanese)
    return 0;
  const ArGeneratedContract *contract = FindContract(route, profile);
  return contract ? contract->anchor_count : 0;
}

const char *ArLanguageContract_RequiredAnchor(
    const char *semantic_id, ArLanguageSourceProfile profile, uint32_t index) {
  const ArGeneratedRoute *route = FindRoute(semantic_id);
  if (!route || (uint32_t)profile > (uint32_t)kArLanguageSourceProfile_Japanese)
    return NULL;
  const ArGeneratedContract *contract = FindContract(route, profile);
  return contract && index < contract->anchor_count
             ? kGeneratedAnchorNames[contract->anchor_first + index]
             : NULL;
}

ArLanguagePlaceholderKind ArLanguageContract_PlaceholderKind(
    const char *placeholder) {
  const int index = FindPlaceholderIndex(placeholder);
  return index >= 0 ? kGeneratedPlaceholders[index].kind
                    : kArLanguagePlaceholder_Unknown;
}
