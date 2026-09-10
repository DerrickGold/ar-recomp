#include "localization/language_keyboard.h"
#include "localization/unicode_grapheme.h"

#include <stdio.h>
#include <string.h>

typedef struct KeyboardPage {
  char text[kArLanguageKeyboardMaximumPageBytes + 1];
  size_t bytes;
  bool pending_space;
  /* The three semantic fields are fixed by the native input consumer. */
  size_t marker[3]; /* name, backspace, finish; byte offset + 1, zero = absent */
} KeyboardPage;

static bool AppendByte(KeyboardPage *page, char byte) {
  if (byte == ' ' || byte == '\t' || byte == '\r') {
    page->pending_space = page->bytes && page->text[page->bytes - 1] != '\n';
    return true;
  }
  if (byte == '\n') {
    page->pending_space = false;
    if (!page->bytes || page->text[page->bytes - 1] == '\n') return true;
  } else if (page->pending_space) {
    if (page->bytes == kArLanguageKeyboardMaximumPageBytes) return false;
    page->text[page->bytes++] = ' ';
    page->pending_space = false;
  }
  if (page->bytes == kArLanguageKeyboardMaximumPageBytes) return false;
  page->text[page->bytes++] = byte;
  return true;
}

static bool Lines(const char *text, size_t bytes, size_t *starts, size_t *ends,
                   unsigned capacity, unsigned *count) {
  *count = 0;
  size_t start = 0;
  for (size_t i = 0; i <= bytes; ++i) {
    if (i < bytes && text[i] != '\n') continue;
    if (*count == capacity) return false;
    starts[*count] = start;
    ends[(*count)++] = i;
    start = i + 1;
  }
  return true;
}

static bool RowKey(const char *text, size_t bytes, size_t first, size_t last,
                    unsigned column, uint32_t *start, uint32_t *end) {
  unsigned keys = 0;
  uint32_t selected_start = 0, selected_end = 0;
  for (size_t offset = first; offset < last;) {
    size_t next;
    if (!ArUnicodeGrapheme_Next(text, bytes, offset, NULL, &next) || next > last)
      return false;
    if (next != offset + 1 || (text[offset] != ' ' && text[offset] != '\t')) {
      /* The renderer groups runs between gutters into key cells. A pair of
       * adjacent graphemes is not one key and must not disagree with input. */
      if (next < last && text[next] != ' ' && text[next] != '\t') return false;
      if (keys == column) { selected_start = (uint32_t)offset; selected_end = (uint32_t)next; }
      ++keys;
    }
    offset = next;
  }
  if (keys != kArLanguageKeyboardColumns) return false;
  *start = selected_start;
  *end = selected_end;
  return true;
}

bool ArLanguageKeyboard_KeyRange(const char *utf8, size_t bytes,
                                  unsigned row, unsigned column,
                                  uint32_t *start, uint32_t *end) {
  if (start) *start = 0;
  if (end) *end = 0;
  if (!utf8 || !bytes || bytes > UINT32_MAX || !start || !end ||
      row >= kArLanguageKeyboardRows || column >= kArLanguageKeyboardColumns)
    return false;
  size_t starts[kArLanguageKeyboardMaximumLines + 1];
  size_t ends[kArLanguageKeyboardMaximumLines + 1];
  unsigned count;
  if (!Lines(utf8, bytes, starts, ends, kArLanguageKeyboardMaximumLines + 1,
             &count) || count < kArLanguageKeyboardRows) return false;
  const unsigned line = count - kArLanguageKeyboardRows + row;
  return RowKey(utf8, bytes, starts[line], ends[line], column, start, end);
}

static bool CheckPage(KeyboardPage *page, unsigned index, bool allow_empty,
                       const char *id, ArLanguagePackError *error) {
  while (page->bytes && page->text[page->bytes - 1] == '\n') --page->bytes;
  if (!page->bytes && allow_empty) return true;
  size_t starts[kArLanguageKeyboardMaximumLines], ends[kArLanguageKeyboardMaximumLines];
  unsigned count;
  const char *reason = NULL;
  if (!Lines(page->text, page->bytes, starts, ends,
             kArLanguageKeyboardMaximumLines, &count) ||
      count < kArLanguageKeyboardRows + 2) {
    reason = "requires a name field, underline slot and five keyboard rows (within the documented line limit)";
  } else {
    const unsigned first = count - kArLanguageKeyboardRows;
    if (!page->marker[0] || page->marker[0] - 1 != starts[first - 2] ||
        ends[first - 2] != starts[first - 2] + 3) {
      reason = "put {master_name} alone on the line above the underline slot";
    }
    for (size_t i = starts[first - 1]; !reason && i < ends[first - 1]; ++i) {
      if (page->text[i] != '-') reason = "keep a dash-only underline slot immediately before the keys";
    }
    for (unsigned row = 0; !reason && row < kArLanguageKeyboardRows; ++row) {
      uint32_t start = 0, end = 0;
      if (!RowKey(page->text, page->bytes, starts[first + row], ends[first + row],
                   0, &start, &end)) {
        if (error) snprintf(error->message, sizeof(error->message),
            "%s: keyboard page %u row %u requires exactly %u grapheme keys separated by ASCII spaces",
            id, index, row + 1, kArLanguageKeyboardColumns);
        return false;
      }
    }
    for (unsigned action = 1; !reason && action <= 2; ++action) {
      uint32_t start = 0, end = 0;
      const unsigned last = count - 1;
      if (!RowKey(page->text, page->bytes, starts[last], ends[last],
                    kArLanguageKeyboardColumns - 3 + action, &start, &end) ||
          page->marker[action] != start + 1 || end != start + 3)
        reason = "keep {icon.name_entry.backspace} and {icon.name_entry.finish} in the final two key positions";
    }
  }
  if (reason && error) snprintf(error->message, sizeof(error->message),
      "%s: keyboard page %u: %s", id, index, reason);
  return reason == NULL;
}

bool ArLanguageKeyboard_ValidateMessage(const ArLanguagePack *pack,
                                        const ArLanguageMessage *body,
                                        const char *semantic_id,
                                        ArLanguagePackError *error) {
  if (!pack || !body || !semantic_id) return false;
  if (strcmp(semantic_id, AR_LANGUAGE_KEYBOARD_ROUTE)) return true;
  KeyboardPage page = {0};
  unsigned index = 1;
  for (uint32_t i = 0; i < body->operation_count; ++i) {
    const ArLanguageOperation *op = ArLanguagePack_GetOperation(pack, body, i);
    bool appended = true;
    if (op->kind == kArLanguageOperation_Text) {
      for (const char *p = ArLanguagePack_GetString(pack, op->value.text); p && *p; ++p) {
        if (!AppendByte(&page, *p)) { appended = false; break; }
      }
    } else if (op->kind == kArLanguageOperation_LineBreak ||
               op->kind == kArLanguageOperation_ParagraphBreak) {
      appended = AppendByte(&page, '\n'); // keyboard normalization collapses blanks
    } else if (op->kind == kArLanguageOperation_Placeholder) {
      const char *name = ArLanguagePack_GetString(pack, op->value.placeholder);
      const unsigned marker = !strcmp(name, "master_name") ? 0
          : !strcmp(name, "icon.name_entry.backspace") ? 1 : 2;
      if (page.marker[marker]) {
        if (error) snprintf(error->message, sizeof(error->message),
            "%s: keyboard page %u repeats {%s}", semantic_id, index, name);
        return false;
      }
      const size_t start = page.bytes + (page.pending_space ? 1 : 0);
      appended = AppendByte(&page, '\xEF') && AppendByte(&page, '\xBF') && AppendByte(&page, '\xBC');
      page.marker[marker] = start + 1;
    } else if (op->kind == kArLanguageOperation_PageBreak) {
      if (!CheckPage(&page, index++, false, semantic_id, error)) return false;
      memset(&page, 0, sizeof(page));
    }
    if (!appended) {
      if (error) snprintf(error->message, sizeof(error->message),
          "%s: keyboard page %u exceeds %u normalized UTF-8 bytes",
          semantic_id, index, kArLanguageKeyboardMaximumPageBytes);
      return false;
    }
  }
  return CheckPage(&page, index, index == 1, semantic_id, error);
}
