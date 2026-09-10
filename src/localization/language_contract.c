#include "localization/language_contract.h"
#include "localization/language_row_shape.h"
#include "localization/language_keyboard.h"

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
  uint16_t maximum_lines;
  uint8_t profile_mask;
  uint8_t canonical_profile;
  bool optional;
  ArLanguagePresentationShape shape;
  uint8_t maximum_pages;
  uint8_t required_nonempty_lines;
} ArGeneratedRoute;

#include "localization/language_contract_data.inc"

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

_Static_assert(ARRAY_COUNT(kGeneratedRoutes) == 558,
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

/* Authored shape as the game will read it: a paragraph break advances two
 * lines, a page break starts a page, and a line counts as content when it
 * holds anything other than spaces. */
typedef struct PresentationScan {
  uint32_t pages;
  uint32_t line;       /* current 1-based line within the current page */
  uint32_t lines_used; /* highest line index that carries content */
  uint32_t nonempty_lines;
  bool line_has_content;
} PresentationScan;

/* Only lines that carry content count towards the field height, so the newline
 * that ends the last authored line is not a second row. */
static void ScanContent(PresentationScan *scan) {
  scan->line_has_content = true;
  if (scan->line > scan->lines_used) scan->lines_used = scan->line;
}

static void ScanBreak(PresentationScan *scan, uint32_t advance) {
  for (uint32_t i = 0; i < advance; ++i) {
    if (scan->line_has_content) scan->nonempty_lines++;
    scan->line_has_content = false;
    scan->line++;
  }
}

static void ScanText(PresentationScan *scan, const char *text) {
  for (const char *cursor = text; cursor && *cursor; ++cursor) {
    if (*cursor == '\n')
      ScanBreak(scan, 1);
    else if (*cursor != ' ' && *cursor != '\t')
      ScanContent(scan);
  }
}

static const char *ShapeName(ArLanguagePresentationShape shape) {
  return shape == kArLanguagePresentation_Fixed ? "fixed field"
      : shape == kArLanguagePresentation_Keyboard ? "name-entry keyboard"
      : shape == kArLanguagePresentation_Inline ? "inline term"
      : "dialogue";
}

/* Rejects content the game would silently never display: a page the fixed
 * composer never advances to, a row past the native field, or a choice count
 * the native menu cannot show. */
static bool ValidatePresentation(const ArGeneratedRoute *route,
                                 PresentationScan *scan,
                                 const char *diagnostic_id,
                                 ArLanguagePackError *error) {
  if (scan->line_has_content) scan->nonempty_lines++;
  scan->line_has_content = false;
  if (route->maximum_pages && scan->pages > route->maximum_pages) {
    SetError(error,
             "%s: this %s displays %u page(s); pages beyond that are never "
             "shown, so remove the extra page break(s)",
             diagnostic_id, ShapeName(route->shape), route->maximum_pages);
    return false;
  }
  if (route->maximum_lines && scan->lines_used > route->maximum_lines) {
    SetError(error,
             "%s: this %s reserves %u line(s); the message has %u",
             diagnostic_id, ShapeName(route->shape), route->maximum_lines,
             scan->lines_used);
    return false;
  }
  /* An intentional empty replacement hides wording while retaining native
   * controls/artwork; omitting the route requests fallback instead. Only a
   * partly filled choice menu is a shape error here. */
  if (route->required_nonempty_lines && scan->nonempty_lines &&
      scan->nonempty_lines != route->required_nonempty_lines) {
    SetError(error,
             "%s: this menu shows exactly %u choice(s); the message has %u",
             diagnostic_id, route->required_nonempty_lines,
             scan->nonempty_lines);
    return false;
  }
  return true;
}

typedef struct TableScan {
  ArLanguageRowShape shape;
  uint32_t line, fields;
  bool started, content;
} TableScan;

static bool CheckTableRow(TableScan *scan, const char *id,
                           ArLanguagePackError *error) {
  if (scan->content &&
      !ArLanguageRowShape_Allows(scan->shape, scan->line, scan->fields)) {
    SetError(error, "%s: table row %u has %u field(s); this row shape is "
                    "unsupported (keep the template's | separators and "
                    "blank rows)", id, scan->line + 1, scan->fields);
    return false;
  }
  return true;
}

static bool TableBreak(TableScan *scan, const char *id,
                        ArLanguagePackError *error) {
  if (!CheckTableRow(scan, id, error)) return false;
  /* The game's fixed-text normalization discards leading blank lines, keeps
   * internal blank rows and trims trailing blanks. Count content, not tiles. */
  if (scan->started) ++scan->line;
  scan->fields = 1;
  scan->content = false;
  return true;
}

static bool ValidateTable(const ArLanguagePack *pack,
                           const ArLanguageMessage *body, const char *id,
                           ArLanguagePackError *error) {
  TableScan scan = {.shape = ArLanguageRowShape_ForProfile(
      id, ArLanguagePack_GetMetadata(pack)->source_profile), .fields = 1};
  if (scan.shape == kArLanguageRowShape_None) return true;
  for (uint32_t i = 0; i < body->operation_count; ++i) {
    const ArLanguageOperation *op = ArLanguagePack_GetOperation(pack, body, i);
    if (op->kind == kArLanguageOperation_Text) {
      const char *text = ArLanguagePack_GetString(pack, op->value.text);
      for (const char *p = text; p && *p; ++p) {
        if (*p == '\n') {
          if (!TableBreak(&scan, id, error)) return false;
        } else if (*p != ' ' && *p != '\t' && *p != '\r') {
          scan.started = scan.content = true;
          if (*p == '|') ++scan.fields;
        }
      }
    } else if (op->kind == kArLanguageOperation_Placeholder) {
      scan.started = scan.content = true;
    } else if (op->kind == kArLanguageOperation_LineBreak ||
               op->kind == kArLanguageOperation_ParagraphBreak) {
      if (!TableBreak(&scan, id, error)) return false;
      if (op->kind == kArLanguageOperation_ParagraphBreak &&
          !TableBreak(&scan, id, error)) return false;
    }
  }
  return CheckTableRow(&scan, id, error);
}

static bool ValidateBody(const ArLanguagePack *pack,
                         const ArGeneratedRoute *route,
                         const ArGeneratedContract *contract,
                         const ArLanguageMessage *body,
                         const char *diagnostic_id,
                         ArLanguagePackError *error) {
  uint32_t anchor_index = 0;
  bool yielded = false;
  PresentationScan scan = {1, 1, 0, 0, false};
  for (uint32_t i = 0; i < body->operation_count; i++) {
    const ArLanguageOperation *operation =
        ArLanguagePack_GetOperation(pack, body, i);
    if (!operation) {
      SetError(error, "%s: invalid operation range", diagnostic_id);
      return false;
    }
    if (operation->kind == kArLanguageOperation_Text) {
      const char *text = ArLanguagePack_GetString(pack, operation->value.text);
      /* Fixed/native field structure uses explicit authored breaks. A Unicode
       * separator must not bypass its row/choice contract as an opaque glyph. */
      if (route->shape != kArLanguagePresentation_Flow && text &&
          (strstr(text, "\xc2\x85") || strstr(text, "\xe2\x80\xa8") || strstr(text, "\xe2\x80\xa9"))) {
        SetError(error, "%s: use @line or @paragraph instead of Unicode line-separator controls in fixed fields", diagnostic_id);
        return false;
      }
      ScanText(&scan, text);
    }
    else if (operation->kind == kArLanguageOperation_Placeholder)
      ScanContent(&scan);
    else if (operation->kind == kArLanguageOperation_LineBreak)
      ScanBreak(&scan, 1);
    else if (operation->kind == kArLanguageOperation_ParagraphBreak)
      ScanBreak(&scan, 2);
    else if (operation->kind == kArLanguageOperation_PageBreak) {
      if (scan.line_has_content) scan.nonempty_lines++;
      scan.line_has_content = false;
      scan.pages++;
      scan.line = 1;
    }
    if (yielded && operation->kind != kArLanguageOperation_End &&
        operation->kind != kArLanguageOperation_Empty) {
      SetError(error, "%s: content after a menu yield is unreachable; "
                      "place it before the yield anchor", diagnostic_id);
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
      yielded = !strncmp(anchor, "yield.", 6);
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
  return ValidatePresentation(route, &scan, diagnostic_id, error) &&
      ValidateTable(pack, body, diagnostic_id, error) &&
      ArLanguageKeyboard_ValidateMessage(pack, body, diagnostic_id, error);
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
    if (kGeneratedRoutes[i].optional || !(kGeneratedRoutes[i].profile_mask & profile_bit))
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

bool ArLanguageContract_Presentation(const char *semantic_id,
                                     ArLanguagePresentationContract *out) {
  const ArGeneratedRoute *route = FindRoute(semantic_id);
  if (!route || !out)
    return false;
  *out = (ArLanguagePresentationContract){
      .shape = route->shape,
      .maximum_pages = route->maximum_pages,
      .maximum_lines = route->maximum_lines,
      .required_nonempty_lines = route->required_nonempty_lines,
  };
  return true;
}

ArLanguagePlaceholderKind ArLanguageContract_PlaceholderKind(
    const char *placeholder) {
  const int index = FindPlaceholderIndex(placeholder);
  return index >= 0 ? kGeneratedPlaceholders[index].kind
                    : kArLanguagePlaceholder_Unknown;
}
