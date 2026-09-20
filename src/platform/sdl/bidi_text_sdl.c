#if defined(AR_HAS_SDL3_TTF) && AR_HAS_SDL3_TTF
#include "platform/sdl/bidi_text_sdl.h"
#include "localization/text_boundaries.h"
#include "localization/unicode_grapheme.h"
#include "platform/sdl/styled_run_sdl.h"

#include <SheenBidi/SheenBidi.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Bound both layout work and allocation for direct backend clients, not only
 * cached game requests. The game's largest accepted dialogue is much smaller. */
enum { kMaximumLayoutBytes = 65536, kMaximumLayoutPixels = 16 * 1024 * 1024 };

typedef struct ShapedRun {
  TTF_Text *text;
  ArSdlStyledRun styled;
  size_t offset, length, logical_offset;
  int x, y, width, height, line;
  bool rtl, align_right;
} ShapedRun;

struct ArSdlBidiLayout {
  TTF_TextEngine *engine;
  ShapedRun *runs;
  size_t count, capacity, utf8_bytes;
  int width, height, line_count, line_advance;
  int next_line_y;
  bool styled;
  ArTextLineMetrics *lines;
  size_t line_capacity;
  ArTextDirection paragraph_direction;
  char *virtual_text;
  size_t *logical_offsets;
  size_t layout_bytes;
};

typedef struct ScriptRange {
  size_t start, end;
  Uint32 tag;
} ScriptRange;

typedef struct LayoutSource {
  const char *text;
  const ArTextRasterRequest *request;
  TTF_Font *font;
  ScriptRange *scripts;
  size_t script_count;
  /* Indexed by original UTF-8 end byte, including zero-width controls.
   * Negative means not a legal shaped-cluster boundary. */
  int *advances;
  const size_t *logical_offsets;
  size_t bytes;
  ArSdlTextFonts *fonts;
  int base_pixels;
  ArTextRasterFailure *failure;
} LayoutSource;

bool ArSdlBidiText_NeedsLayout(const char *text, size_t length,
                              ArTextDirection direction) {
  if (direction == kArTextDirection_RightToLeft) return true;
  size_t cursor = 0;
  while (cursor < length) {
    uint32_t scalar;
    if (!ArUnicode_DecodeScalar(text, length, cursor, &scalar, &cursor)) return false;
    if (scalar == '\r' || scalar == 0x85 || scalar == 0x2028 || scalar == 0x2029) return true;
    if (scalar < 0x80) continue;
    const SBBidiType type = SBCodepointGetBidiType(scalar);
    if (type == SBBidiTypeR || type == SBBidiTypeAL || type == SBBidiTypeAN ||
        SBBidiTypeIsFormat(type)) return true;
  }
  return false;
}

static void TruncateRuns(ArSdlBidiLayout *layout, size_t count) {
  while (layout->count > count) {
    ShapedRun *run = &layout->runs[--layout->count];
    TTF_DestroyText(run->text);
    ArSdlStyledRun_Destroy(&run->styled);
  }
}

void ArSdlBidiText_Destroy(ArSdlBidiLayout *layout) {
  if (!layout) return;
  TruncateRuns(layout, 0);
  free(layout->runs);
  free(layout->virtual_text);
  free(layout->logical_offsets);
  free(layout->lines);
  TTF_DestroySurfaceTextEngine(layout->engine);
  free(layout);
}

static bool AddRun(ArSdlBidiLayout *layout, const LayoutSource *source,
                    size_t start, size_t end, SBLevel level, Uint32 script,
                    int *x, int y, int line) {
  if (layout->count == layout->capacity) {
    size_t capacity = layout->capacity ? layout->capacity * 2 : 16;
    if (capacity > layout->layout_bytes) capacity = layout->layout_bytes;
    if (capacity <= layout->count) return SDL_SetError("too many bidi runs");
    ShapedRun *runs = realloc(layout->runs, capacity * sizeof(*runs));
    if (!runs) return SDL_SetError("out of memory for bidi runs");
    layout->runs = runs;
    layout->capacity = capacity;
  }
  if (source->fonts) {
    ArSdlStyledRun styled = {0};
    if (!ArSdlStyledRun_Create(&styled, source->fonts, source->text + start,
                               end - start, source->logical_offsets + start,
                               source->request, source->base_pixels,
                               (level & 1) != 0, script, source->failure))
      return false;
    if (styled.width > INT_MAX - *x) {
      ArSdlStyledRun_Destroy(&styled);
      return SDL_SetError("styled text is too wide");
    }
    layout->runs[layout->count++] =
        (ShapedRun){.styled = styled,
                    .offset = start,
                    .length = end - start,
                    .logical_offset = source->logical_offsets[start],
                    .x = *x,
                    .y = y,
                    .width = styled.width,
                    .height = styled.ascent + styled.descent,
                    .line = line,
                    .rtl = (level & 1) != 0};
    *x += styled.width;
    return true;
  }
  TTF_Text *text = TTF_CreateText(layout->engine, source->font,
                                 source->text + start, end - start);
  if (!text) return false;
  int width, height;
  if (!TTF_SetTextDirection(text, level & 1 ? TTF_DIRECTION_RTL : TTF_DIRECTION_LTR) ||
      !TTF_SetTextScript(text, script) || !TTF_SetTextWrapWhitespaceVisible(text, true) ||
      !TTF_GetTextSize(text, &width, &height) ||
      width < 0 || height < 0 || width > INT_MAX - *x) {
    TTF_DestroyText(text);
    return false;
  }
  layout->runs[layout->count++] = (ShapedRun){
      .text = text, .offset = start, .length = end - start,
      .logical_offset = source->logical_offsets[start],
      .x = *x, .y = y, .width = width, .height = height, .line = line,
      .rtl = (level & 1) != 0};
  *x += width;
  return true;
}

/* Binary search, not a paragraph-wide script scan for every line/run. */
static size_t ScriptAt(const LayoutSource *source, size_t offset) {
  size_t low = 0, high = source->script_count;
  while (low + 1 < high) {
    size_t middle = low + (high - low) / 2;
    if (source->scripts[middle].start <= offset) low = middle;
    else high = middle;
  }
  return low;
}

static bool ShapingByte(const LayoutSource *source, size_t offset) {
  if (source->logical_offsets[offset] == source->logical_offsets[offset + 1]) return false;
  if ((uint8_t)source->text[offset] < 0x80) return true;
  size_t first = offset;
  while (first && ((uint8_t)source->text[first] & 0xc0u) == 0x80u) --first;
  uint32_t scalar; size_t next;
  if (!ArUnicode_DecodeScalar(source->text, source->bytes, first, &scalar, &next)) return true;
  /* Explicit embedding/isolate controls affect bidi, not glyph layout. Keep
   * joining controls (ZWJ/ZWNJ) inside the shaped text. Applying the same rule
   * to authored and virtual controls avoids control-adjacent space trimming. */
  return !(scalar >= 0x202a && scalar <= 0x202e) &&
         !(scalar >= 0x2066 && scalar <= 0x2069);
}

static bool ShapeLine(ArSdlBidiLayout *layout, const LayoutSource *source,
                       SBParagraphRef paragraph, size_t start, size_t end,
                       int y, int line, int *width) {
  *width = 0;
  if (end == start) return true;
  SBLineRef bidi = SBParagraphCreateLine(paragraph, start, end - start);
  if (!bidi) return SDL_SetError("cannot resolve bidi line");
  const SBRun *runs = SBLineGetRunsPtr(bidi);
  const size_t count = SBLineGetRunCount(bidi);
  bool ok = true;
  for (size_t i = 0; ok && i < count; ++i) {
    const SBRun *run = &runs[i];
    const size_t stop = run->offset + run->length;
    size_t cursor = run->level & 1 ? stop : run->offset;
    while (ok && ((run->level & 1) ? cursor > run->offset : cursor < stop)) {
      const ScriptRange *script = &source->scripts[ScriptAt(source,
          run->level & 1 ? cursor - 1 : cursor)];
      size_t a = run->level & 1 ? script->start : cursor;
      size_t b = run->level & 1 ? cursor : script->end;
      if (a < run->offset) a = run->offset;
      if (b > stop) b = stop;
      /* Isolation controls participate in UAX #9, never in glyph rasterization
       * or the caller's logical cluster/reveal clock. Split around virtual
       * bytes while retaining the directional run's visual traversal order. */
      size_t part = run->level & 1 ? b : a;
      while (ok && ((run->level & 1) ? part > a : part < b)) {
        size_t p = part, q = part;
        if (run->level & 1) {
          while (q > a && !ShapingByte(source, q-1)) --q;
          p = q;
          while (p > a && ShapingByte(source, p-1)) --p;
          part = p;
        } else {
          while (p < b && !ShapingByte(source, p)) ++p;
          q = p;
          while (q < b && ShapingByte(source, q)) ++q;
          part = q;
        }
        if (q > p) ok = AddRun(layout, source, p, q, run->level, script->tag, width, y, line);
      }
      cursor = run->level & 1 ? a : b;
    }
  }
  SBLineRelease(bidi);
  return ok;
}

static bool RecordAdvances(const ArSdlBidiLayout *layout, LayoutSource *source,
                            size_t first) {
  for (size_t r = first; r < layout->count; ++r) {
    const ShapedRun *run = &layout->runs[r];
    if (source->fonts) {
      for (size_t i = 0; i < run->styled.cluster_count; ++i) {
        const ArSdlStyledCluster *cluster = &run->styled.clusters[i];
        source->advances[run->offset + cluster->end] = cluster->advance;
      }
      continue;
    }
    if (!run->width) continue; /* Authored bidi controls can form an empty run. */
    size_t offset = 0;
    size_t previous_end = 0;
    int previous_x = run->rtl ? run->width : 0;
    while (offset < run->length) {
      TTF_SubString cluster;
      if (!TTF_GetTextSubString(run->text, (int)offset, &cluster) ||
          cluster.offset < 0 || cluster.length <= 0) return false;
      const size_t end = (size_t)cluster.offset + cluster.length;
      if (end <= offset || end > run->length) return SDL_SetError("invalid bidi cluster");
      /* Ink widths are not advances (bearings and joining overlap). Account
       * for the distance between shaped origins, retaining the exact total
       * run width. No glyph is re-shaped just to estimate a wrap boundary. */
      int advance = run->rtl ? previous_x - cluster.rect.x
                             : cluster.rect.x - previous_x;
      if (advance < 0) advance = 0;
      if (run->rtl) source->advances[run->offset + end] = advance;
      else if (previous_end) source->advances[run->offset + previous_end] += advance;
      else source->advances[run->offset + end] = advance;
      if (!run->rtl && previous_end) source->advances[run->offset + end] = 0;
      previous_x = cluster.rect.x;
      previous_end = end;
      offset = end;
    }
    if (previous_end) {
      const int remainder = run->rtl ? previous_x : run->width - previous_x;
      if (remainder > 0) source->advances[run->offset + previous_end] += remainder;
    }
  }
  return true;
}

static bool BreakableSpace(uint32_t scalar) {
  /* Unicode White_Space values whose line-break class permits an ordinary
   * break. NBSP, figure space and narrow NBSP deliberately remain glue. */
  return scalar == '\t' || scalar == ' ' || scalar == 0x1680 ||
      (scalar >= 0x2000 && scalar <= 0x2006) ||
      (scalar >= 0x2008 && scalar <= 0x200a) || scalar == 0x205f ||
      scalar == 0x3000;
}

static size_t BreakableSpaceBefore(const LayoutSource *source, size_t end) {
  if (!end) return end;
  size_t start = end - 1u;
  while (start && ((uint8_t)source->text[start] & 0xc0u) == 0x80u) --start;
  uint32_t scalar = 0;
  size_t next = start;
  return ArUnicode_DecodeScalar(source->text, source->bytes, start,
                                &scalar, &next) && next == end &&
                 BreakableSpace(scalar)
      ? start : end;
}

static size_t SkipBreakableSpaces(const LayoutSource *source, size_t start,
                                  size_t end) {
  while (start < end) {
    uint32_t scalar = 0;
    size_t next = start;
    if (!ArUnicode_DecodeScalar(source->text, end, start, &scalar, &next) ||
        !BreakableSpace(scalar))
      break;
    start = next;
  }
  return start;
}

static size_t TrimBreakableSpaces(const LayoutSource *source, size_t start,
                                  size_t end) {
  while (end > start) {
    const size_t space = BreakableSpaceBefore(source, end);
    if (space == end) break;
    end = space;
  }
  return end;
}

/* Retain the existing word-first policy: break at a space when possible,
 * otherwise only at complete shaped clusters. Nonbreaking spaces remain glue.
 * Line fragments are reshaped after wrapping, so joining never crosses a line. */
static size_t WrapEnd(const LayoutSource *source, size_t start, size_t end, int width) {
  int64_t advance = 0;
  size_t last = start, word = start;
  for (size_t i = start + 1; i <= end; ++i) {
    if (source->advances[i] < 0) continue;
    const bool space = BreakableSpaceBefore(source, i) != i;
    advance += source->advances[i];
    if (advance > width) {
      if (space) return i; /* discard the break space, not the preceding word */
      return word > start ? word : last > start ? last : i;
    }
    last = i;
    if (space) word = i;
  }
  return end;
}

static bool LayoutLineContent(ArSdlBidiLayout *layout, LayoutSource *source,
                              SBParagraphRef paragraph, size_t start,
                              size_t content_end) {
  const ArTextRasterRequest *request = source->request;
  const bool wrap = (request->flags & kArTextRasterFlag_WrapWords) != 0;
  size_t first = layout->count;
  int width = 0;
  if (wrap && content_end > start) {
    if (!ShapeLine(layout, source, paragraph, start, content_end, 0, 0, &width) ||
        !RecordAdvances(layout, source, first)) return false;
    TruncateRuns(layout, first);
  }
  size_t cursor = start;
  do {
    size_t stop = wrap ? WrapEnd(source, cursor, content_end, request->maximum_width)
                       : content_end;
    size_t ink_end = stop;
    if (wrap && stop < content_end)
      ink_end = TrimBreakableSpaces(source, cursor, ink_end);
    const int y = layout->styled ? layout->next_line_y
                                 : layout->line_count * layout->line_advance;
    first = layout->count;
    if (!ShapeLine(layout, source, paragraph, cursor, ink_end, y,
                   layout->line_count, &width)) return false;
    /* Joining/ligatures at the line edge may change the measured width. If a
     * candidate no longer fits, back up to a complete shaped boundary. */
    while (wrap && width > request->maximum_width && ink_end > cursor) {
      size_t earlier = ink_end - 1;
      while (earlier > cursor && source->advances[earlier] < 0) --earlier;
      if (earlier == cursor) break; /* one oversized cluster: caller fits font */
      for (size_t word = earlier; word > cursor; --word)
        if (BreakableSpaceBefore(source, word) != word &&
            source->advances[word] >= 0) { earlier = word; break; }
      stop = ink_end = earlier;
      ink_end = TrimBreakableSpaces(source, cursor, ink_end);
      TruncateRuns(layout, first);
      if (!ShapeLine(layout, source, paragraph, cursor, ink_end, y,
                     layout->line_count, &width)) return false;
    }
    const bool rtl = (SBParagraphGetBaseLevel(paragraph) & 1) != 0;
    const bool align_right =
        (request->alignment == kArTextHorizontalAlignment_Leading && rtl) ||
        (request->alignment == kArTextHorizontalAlignment_Trailing && !rtl) ||
        request->alignment == kArTextHorizontalAlignment_Right;
    if (first < layout->count) layout->runs[first].align_right = align_right;
    int x = 0;
    if (wrap) {
      if (request->alignment == kArTextHorizontalAlignment_Center)
        x = (request->maximum_width - width) / 2;
      else if (align_right)
        x = request->maximum_width - width;
    }
    if (x < 0) x = 0;
    if (width > layout->width) layout->width = width;
    int height = TTF_GetFontHeight(source->font);
    int ascent = TTF_GetFontAscent(source->font);
    int descent = height - ascent;
    if (layout->styled) {
      for (size_t i = first; i < layout->count; ++i) {
        if (layout->runs[i].styled.ascent > ascent)
          ascent = layout->runs[i].styled.ascent;
        if (layout->runs[i].styled.descent > descent)
          descent = layout->runs[i].styled.descent;
      }
      height = ascent + descent;
    }
    for (size_t i = first; i < layout->count; ++i) {
      layout->runs[i].x += x;
      if (layout->styled)
        layout->runs[i].y += ascent - layout->runs[i].styled.ascent;
      if (layout->runs[i].height > height) height = layout->runs[i].height;
    }
    const int advance =
        height > layout->line_advance ? height : layout->line_advance;
    if ((int64_t)y + advance > INT_MAX)
      return SDL_SetError("bidi text too tall");
    if (layout->styled) {
      if ((size_t)layout->line_count == layout->line_capacity) {
        const size_t capacity =
            layout->line_capacity ? layout->line_capacity * 2 : 16;
        ArTextLineMetrics *lines =
            realloc(layout->lines, capacity * sizeof(*lines));
        if (!lines)
          return SDL_SetError("out of memory retaining line metrics");
        layout->lines = lines;
        layout->line_capacity = capacity;
      }
      layout->lines[layout->line_count] =
          (ArTextLineMetrics){y, y + ascent, advance};
    }
    layout->next_line_y = y + advance;
    if (y + height > layout->height) layout->height = y + height;
    ++layout->line_count;
    cursor = stop;
    if (wrap) cursor = SkipBreakableSpaces(source, cursor, content_end);
    if (layout->line_count > kMaximumLayoutBytes ||
        (int64_t)layout->line_count * layout->line_advance > INT_MAX)
      return SDL_SetError("too many bidi lines");
  } while (cursor < content_end);
  if (wrap && request->maximum_width > layout->width) layout->width = request->maximum_width;
  return true;
}

static bool LayoutHardLine(ArSdlBidiLayout *layout, LayoutSource *source,
                           SBParagraphRef paragraph, size_t start, size_t end) {
  const ArTextRasterRequest *request = source->request;
  /* An authored blank row still needs a line strut, even when this dialogue
   * also carries optional native wrapping hints. */
  if (start == end || !source->fonts || !request->preferred_line_breaks)
    return LayoutLineContent(layout, source, paragraph, start, end);
  const size_t first = layout->count;
  int measured = 0;
  if (!ShapeLine(layout, source, paragraph, start, end, 0, 0, &measured) ||
      !RecordAdvances(layout, source, first))
    return false;
  TruncateRuns(layout, first);
  int64_t width = 0;
  size_t segment = start;
  for (size_t i = start; i < end; ++i) {
    if (i > segment && source->advances[i] > 0)
      width += source->advances[i];
    const size_t original = source->logical_offsets[i];
    if (source->text[i] != ' ' || width > request->maximum_width ||
        !ArTextBoundary_Get(request->preferred_line_breaks,
                            request->preferred_line_break_source_offset +
                                original))
      continue;
    /* Recheck the complete prefix at its actual line edge: joining there can
     * differ from the paragraph-wide measurement. A denied break stays soft. */
    const size_t checkpoint = layout->count;
    if (!ShapeLine(layout, source, paragraph, segment, i, 0, 0, &measured))
      return false;
    TruncateRuns(layout, checkpoint);
    if (measured > request->maximum_width)
      continue;
    if (!LayoutLineContent(layout, source, paragraph, segment, i))
      return false;
    segment = i + 1;
    width = 0;
  }
  return segment == end ||
         LayoutLineContent(layout, source, paragraph, segment, end);
}

static bool LineSeparator(uint32_t scalar) {
  return scalar == '\n' || scalar == '\r' || scalar == 0x85 ||
      scalar == 0x2028 || scalar == 0x2029;
}

static bool LayoutParagraph(ArSdlBidiLayout *layout, LayoutSource *source,
                            SBParagraphRef paragraph) {
  size_t start = SBParagraphGetOffset(paragraph);
  const size_t end = start + SBParagraphGetLength(paragraph);
  for (size_t cursor = start; cursor < end;) {
    uint32_t scalar;
    size_t next;
    if (!ArUnicode_DecodeScalar(source->text, end, cursor, &scalar, &next)) return false;
    if (LineSeparator(scalar)) {
      /* U+2028 splits physical lines within the same resolved paragraph.
       * NEL/PS/CR/LF terminate paragraphs in UAX9; none are font glyphs. */
      if (!LayoutHardLine(layout, source, paragraph, start, cursor)) return false;
      if (scalar == '\r' && next < end && source->text[next] == '\n') ++next;
      start = next;
    }
    cursor = next;
  }
  return start == end || LayoutHardLine(layout, source, paragraph, start, end);
}

static void VirtualControl(ArSdlBidiLayout *layout, uint8_t suffix, size_t logical) {
  const uint8_t bytes[] = {0xe2, 0x81, suffix};
  for (size_t j = 0; j < 3; ++j) {
    layout->logical_offsets[layout->layout_bytes] = logical;
    layout->virtual_text[layout->layout_bytes++] = (char)bytes[j];
  }
  layout->logical_offsets[layout->layout_bytes] = logical;
}

static bool VirtualSource(ArSdlBidiLayout *layout, const char *text,
                          const ArTextRasterRequest *request) {
  /* Reserve one outer pair per span, one restart pair per separator, and
   * enough closes for dangling authored isolates. Counting the latter two
   * conservatively over the source avoids a large per-byte worst-case map
   * allocation for ordinary cached menus with just one inserted value. */
  size_t capacity = request->utf8_bytes + request->bidi_span_count * 6 + 1;
  if (request->bidi_span_count) {
    for (size_t i = 0; i < request->utf8_bytes;) {
      uint32_t scalar;
      if (!ArUnicode_DecodeScalar(text, request->utf8_bytes, i, &scalar, &i)) return false;
      if (LineSeparator(scalar)) capacity += 6;
      else if (scalar >= 0x2066 && scalar <= 0x2068) capacity += 3;
    }
  }
  layout->virtual_text = malloc(capacity);
  layout->logical_offsets = malloc(capacity * sizeof(*layout->logical_offsets));
  if (!layout->virtual_text || !layout->logical_offsets) return false;
  size_t span = 0;
  bool open = false;
  unsigned nested = 0;
  for (size_t i = 0; i <= request->utf8_bytes;) {
    size_t absolute = request->bidi_source_offset + i;
    while (span < request->bidi_span_count && request->bidi_spans[span].end <= absolute) {
      if (open) {
        while (nested) { VirtualControl(layout, 0xa9, i); --nested; }
        VirtualControl(layout, 0xa9, i); open = false;
      }
      ++span;
    }
    const bool in_span = i < request->utf8_bytes && span < request->bidi_span_count &&
        request->bidi_spans[span].start <= absolute;
    uint32_t scalar = 0; size_t next = i;
    if (i < request->utf8_bytes &&
        !ArUnicode_DecodeScalar(text, request->utf8_bytes, i, &scalar, &next)) return false;
    const bool newline = LineSeparator(scalar);
    if (open && (!in_span || newline)) {
      while (nested) { VirtualControl(layout, 0xa9, i); --nested; }
      VirtualControl(layout, 0xa9, i); open = false;
    }
    if (in_span && !newline && !open) {
      const ArTextDirection direction = request->bidi_spans[span].direction;
      const uint8_t kind = direction == kArTextDirection_LeftToRight ? 0xa6 :
          direction == kArTextDirection_RightToLeft ? 0xa7 : 0xa8;
      VirtualControl(layout, kind, i); open = true;
    }
    if (i == request->utf8_bytes) break;
    /* A value's unmatched PDI must not close the host-created outer isolate.
     * Replace only its private layout copy with a zero-width word joiner.
     * Balanced authored isolates remain active, and dangling opens are closed
     * before the value's outer boundary. Logical UTF-8 stays untouched. */
    bool unmatched = open && scalar == 0x2069 && !nested;
    if (open && scalar >= 0x2066 && scalar <= 0x2068) ++nested;
    else if (open && scalar == 0x2069 && nested) --nested;
    for (size_t j = i; j < next; ++j) {
      layout->logical_offsets[layout->layout_bytes] = j;
      layout->virtual_text[layout->layout_bytes++] = unmatched && j == next-1 ? (char)0xa0 : text[j];
      layout->logical_offsets[layout->layout_bytes] = j + 1;
    }
    i = next;
  }
  layout->virtual_text[layout->layout_bytes] = 0;
  return true;
}

/* Unwrapped text fits to its widest authored line. Only after measuring that
 * width can shorter lines align within the block without adding empty width
 * to the fitting request. Runs are contiguous by physical line. */
static void AlignUnwrappedLines(ArSdlBidiLayout *layout,
                                ArTextHorizontalAlignment alignment) {
  for (size_t first = 0; first < layout->count;) {
    size_t end = first;
    int width = 0;
    while (end < layout->count &&
           layout->runs[end].line == layout->runs[first].line) {
      const ShapedRun *run = &layout->runs[end++];
      if (run->x + run->width > width) width = run->x + run->width;
    }
    const int shift = alignment == kArTextHorizontalAlignment_Center
                          ? (layout->width - width) / 2
                      : layout->runs[first].align_right
                          ? layout->width - width : 0;
    for (size_t i = first; i < end; ++i) layout->runs[i].x += shift;
    first = end;
  }
}

static ArSdlBidiLayout *CreateLayout(TTF_Font *font, ArSdlTextFonts *fonts,
                                     const char *text,
                                     const ArTextRasterRequest *request,
                                     int base_pixels,
                                     ArTextRasterFailure *failure) {
  *failure = kArTextRasterFailure_Retryable;
  if (!request->utf8_bytes || request->utf8_bytes > kMaximumLayoutBytes) {
    *failure = kArTextRasterFailure_Deterministic;
    SDL_SetError("bidi text exceeds the layout byte limit");
    return NULL;
  }
  ArSdlBidiLayout *layout = calloc(1, sizeof(*layout));
  LayoutSource source = {.text = text,
                         .request = request,
                         .font = font,
                         .fonts = fonts,
                         .base_pixels = base_pixels,
                         .failure = failure};
  SBAlgorithmRef algorithm = NULL;
  SBScriptLocatorRef locator = NULL;
  if (!layout) goto fail;
  layout->styled = fonts != NULL;
  layout->utf8_bytes = request->utf8_bytes;
  if (!VirtualSource(layout, text, request)) goto fail;
  source.text = layout->virtual_text;
  source.logical_offsets = layout->logical_offsets;
  const size_t bytes = layout->layout_bytes;
  source.bytes = bytes;
  layout->line_advance = TTF_GetFontLineSkip(font);
  if (layout->line_advance <= 0) goto fail;
  layout->engine = TTF_CreateSurfaceTextEngine();
  source.advances = malloc((bytes + 1) * sizeof(*source.advances));
  source.scripts = calloc(bytes, sizeof(*source.scripts));
  if (!layout->engine || !source.advances || !source.scripts) goto fail;
  for (size_t i = 0; i <= bytes; ++i) source.advances[i] = -1;
  const SBCodepointSequence sequence = {SBStringEncodingUTF8, source.text, bytes};
  algorithm = SBAlgorithmCreate(&sequence);
  locator = SBScriptLocatorCreate();
  if (!algorithm || !locator) goto fail;
  SBScriptLocatorLoadCodepoints(locator, &sequence);
  while (SBScriptLocatorMoveNext(locator)) {
    const SBScriptAgent *script = SBScriptLocatorGetAgent(locator);
    source.scripts[source.script_count++] = (ScriptRange){
        script->offset, script->offset + script->length,
        SBScriptGetUnicodeTag(script->script)};
  }
  if (!source.script_count) goto fail;
  for (size_t offset = 0; offset < bytes;) {
    SBLevel base = request->direction == kArTextDirection_RightToLeft ? 1 :
        request->direction == kArTextDirection_LeftToRight ? 0 : SBLevelDefaultLTR;
    SBParagraphRef paragraph = SBAlgorithmCreateParagraph(
        algorithm, offset, bytes - offset, base);
    if (!paragraph) goto fail;
    if (!offset) layout->paragraph_direction = SBParagraphGetBaseLevel(paragraph) & 1
        ? kArTextDirection_RightToLeft : kArTextDirection_LeftToRight;
    const size_t length = SBParagraphGetLength(paragraph);
    const bool ok = length && LayoutParagraph(layout, &source, paragraph);
    SBParagraphRelease(paragraph);
    if (!ok) goto fail;
    offset += length;
  }
  if (!(request->flags & kArTextRasterFlag_WrapWords))
    AlignUnwrappedLines(layout, request->alignment);
  /* SDL's ordinary single-line LTR path returns its natural width even when
   * wrapping is enabled. Keep migrated labels identical; a multiline box or
   * a bidi paragraph still needs the complete alignment area. */
  if (fonts && layout->line_count == 1 && layout->count &&
      !request->bidi_span_count &&
      !ArSdlBidiText_NeedsLayout(text, request->utf8_bytes,
                                 request->direction)) {
    const int left = layout->runs[0].x;
    layout->width = 0;
    for (size_t i = 0; i < layout->count; ++i) {
      ShapedRun *run = &layout->runs[i];
      run->x -= left;
      if (run->x + run->width > layout->width)
        layout->width = run->x + run->width;
    }
  }
  SBScriptLocatorRelease(locator);
  SBAlgorithmRelease(algorithm);
  free(source.scripts);
  free(source.advances);
  return layout;
fail:
  if (locator) SBScriptLocatorRelease(locator);
  if (algorithm) SBAlgorithmRelease(algorithm);
  free(source.scripts);
  free(source.advances);
  ArSdlBidiText_Destroy(layout);
  if (!SDL_GetError()[0]) SDL_SetError("cannot allocate bidi layout");
  return NULL;
}

ArSdlBidiLayout *ArSdlBidiText_Create(TTF_Font *font, const char *text,
                                      const ArTextRasterRequest *request,
                                      ArTextRasterFailure *failure) {
  return CreateLayout(font, NULL, text, request, 0, failure);
}

ArSdlBidiLayout *ArSdlBidiText_CreateStyled(ArSdlTextFonts *fonts,
                                            TTF_Font *strut, const char *text,
                                            const ArTextRasterRequest *request,
                                            int base_pixels,
                                            ArTextRasterFailure *failure) {
  size_t cursor = 0;
  const size_t first = request->appearance_source_offset;
  for (size_t i = 0; i < request->appearance_span_count; ++i) {
    const ArTextAppearanceSpan *span = &request->appearance_spans[i];
    const size_t boundaries[] = {span->start, span->end};
    for (size_t b = 0; b < 2; ++b) {
      if (boundaries[b] <= first ||
          boundaries[b] >= first + request->utf8_bytes)
        continue;
      const size_t at = boundaries[b] - first;
      while (cursor < at)
        if (!ArUnicodeGrapheme_Next(text, request->utf8_bytes, cursor, NULL,
                                    &cursor))
          return NULL;
      if (cursor != at) {
        *failure = kArTextRasterFailure_Deterministic;
        SDL_SetError("style boundary at byte %zu divides a grapheme", at);
        return NULL;
      }
    }
  }
  return CreateLayout(strut, fonts, text, request, base_pixels, failure);
}

void ArSdlBidiText_GetSize(const ArSdlBidiLayout *layout, int *width, int *height) {
  *width = layout->width;
  *height = layout->height;
}

ArTextDirection ArSdlBidiText_GetDirection(const ArSdlBidiLayout *layout) {
  return layout->paragraph_direction;
}

ArTextLineMetrics *ArSdlBidiText_CopyLines(const ArSdlBidiLayout *layout,
                                           size_t *count) {
  *count = 0;
  if (!layout->lines || !layout->line_count)
    return NULL;
  ArTextLineMetrics *copy = malloc((size_t)layout->line_count * sizeof(*copy));
  if (!copy)
    return NULL;
  *count = (size_t)layout->line_count;
  memcpy(copy, layout->lines, *count * sizeof(*copy));
  return copy;
}

static int CompareFontUses(const void *a, const void *b) {
  const ArTextFontUse *x = a, *y = b;
  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
  if (x->end != y->end)
    return x->end < y->end ? -1 : 1;
  return (x->resource > y->resource) - (x->resource < y->resource);
}

bool ArSdlBidiText_CopyFontUses(const ArSdlBidiLayout *layout,
                                ArTextFontUse **uses, size_t *count) {
  *uses = NULL;
  *count = 0;
  size_t capacity = 0;
  for (size_t r = 0; r < layout->count; ++r) {
    const ShapedRun *run = &layout->runs[r];
    for (size_t c = 0; c < run->styled.cluster_count; ++c) {
      const ArSdlStyledCluster *cluster = &run->styled.clusters[c];
      for (unsigned f = 0; f <= cluster->role->fallback_count; ++f) {
        if (!(cluster->font_mask & (1u << f)))
          continue;
        if (*count == capacity) {
          const size_t next = capacity ? capacity * 2 : 64;
          if (next > 65536)
            goto failed;
          ArTextFontUse *expanded = realloc(*uses, next * sizeof(**uses));
          if (!expanded)
            goto failed;
          *uses = expanded;
          capacity = next;
        }
        (*uses)[(*count)++] = (ArTextFontUse){
            .start = (uint32_t)(run->logical_offset + cluster->start),
            .end = (uint32_t)(run->logical_offset + cluster->end),
            .resource = cluster->role->ids[f],
            .font_pixels = (uint16_t)cluster->pixels,
            .missing = (cluster->missing_font_mask & (1u << f)) != 0};
      }
    }
  }
  if (*count)
    qsort(*uses, *count, sizeof(**uses), CompareFontUses);
  size_t kept = 0;
  for (size_t i = 0; i < *count; ++i) {
    const ArTextFontUse use = (*uses)[i];
    if (kept && (*uses)[kept - 1].end == use.start &&
        (*uses)[kept - 1].resource == use.resource &&
        (*uses)[kept - 1].font_pixels == use.font_pixels &&
        (*uses)[kept - 1].missing == use.missing) {
      (*uses)[kept - 1].end = use.end;
    } else
      (*uses)[kept++] = use;
  }
  *count = kept;
  return true;
failed:
  free(*uses);
  *uses = NULL;
  *count = 0;
  return SDL_SetError("cannot retain bounded font selection metadata");
}

void ArSdlBidiText_DescribePaint(const ArSdlBidiLayout *layout,
                                 const ArTextRevealCluster *clusters,
                                 size_t count, ArSdlClusterPaint *paint) {
  for (size_t r = 0; r < layout->count; ++r) {
    const ShapedRun *run = &layout->runs[r];
    for (size_t c = 0; c < run->styled.cluster_count; ++c) {
      const ArSdlStyledCluster *cluster = &run->styled.clusters[c];
      const size_t end = run->logical_offset + cluster->end;
      size_t low = 0, high = count;
      while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (clusters[middle].end_utf8_byte < end)
          low = middle + 1;
        else
          high = middle;
      }
      if (low < count && clusters[low].end_utf8_byte == end)
        paint[low] = (ArSdlClusterPaint){.appearance = cluster->appearance,
                                         .source_start = run->logical_offset +
                                                         cluster->start,
                                         .pixels = cluster->pixels};
    }
  }
}

static int CompareClusters(const void *a, const void *b) {
  const size_t x = ((const ArTextRevealCluster *)a)->end_utf8_byte;
  const size_t y = ((const ArTextRevealCluster *)b)->end_utf8_byte;
  return (x > y) - (x < y);
}

SDL_Surface *ArSdlBidiText_Render(const ArSdlBidiLayout *layout,
    ArTextRevealCluster **clusters, size_t *cluster_count,
    ArTextRasterFailure *failure) {
  *clusters = NULL;
  *cluster_count = 0;
  if (layout->width <= 0 || layout->height <= 0) {
    *failure = kArTextRasterFailure_Deterministic;
    SDL_SetError("bidi text contains no rasterizable ink");
    return NULL;
  }
  if ((int64_t)layout->width * layout->height > kMaximumLayoutPixels) {
    *failure = kArTextRasterFailure_Deterministic;
    SDL_SetError("bidi text exceeds the raster size ceiling");
    return NULL;
  }
  SDL_Surface *surface = SDL_CreateSurface(layout->width, layout->height,
                                           SDL_PIXELFORMAT_RGBA8888);
  ArTextRevealCluster *result = calloc(layout->utf8_bytes, sizeof(*result));
  if (!surface || !result) goto fail;
  for (size_t i = 0; i < layout->count; ++i) {
    const ShapedRun *run = &layout->runs[i];
    if (!run->width) continue;
    if (layout->styled) {
      if (!ArSdlStyledRun_Draw(&run->styled, surface, run->x, run->y))
        goto fail;
      for (size_t c = 0; c < run->styled.cluster_count; ++c) {
        const ArSdlStyledCluster *cluster = &run->styled.clusters[c];
        if (cluster->rect.w <= 0 || cluster->rect.h <= 0)
          continue;
        result[(*cluster_count)++] = (ArTextRevealCluster){
            .end_utf8_byte = run->logical_offset + cluster->end,
            .line_index = run->line,
            .x = run->x + cluster->rect.x,
            .y = run->y + cluster->rect.y,
            .width = cluster->rect.w,
            .height = cluster->rect.h};
      }
      continue;
    }
    if (!TTF_DrawSurfaceText(run->text, run->x, run->y, surface)) goto fail;
    size_t offset = 0;
    while (offset < run->length) {
      TTF_SubString cluster;
      if (!TTF_GetTextSubString(run->text, (int)offset, &cluster) ||
          cluster.offset < 0 || cluster.length <= 0) goto fail;
      size_t end = (size_t)cluster.offset + cluster.length;
      if (end <= offset || end > run->length) goto fail;
      if (cluster.rect.w > 0 && cluster.rect.h > 0)
        result[(*cluster_count)++] = (ArTextRevealCluster){
            .end_utf8_byte = run->logical_offset + end, .line_index = run->line,
            .x = run->x + cluster.rect.x, .y = run->y + cluster.rect.y,
            .width = cluster.rect.w, .height = cluster.rect.h};
      offset = end;
    }
  }
  qsort(result, *cluster_count, sizeof(*result), CompareClusters);
  *clusters = result;
  *failure = kArTextRasterFailure_None;
  return surface;
fail:
  free(result);
  SDL_DestroySurface(surface);
  *cluster_count = 0;
  return NULL;
}
#endif
