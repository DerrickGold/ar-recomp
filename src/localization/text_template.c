#include "localization/text_template.h"

#include <stdio.h>
#include <string.h>

typedef struct Scope {
  bool italic_tag;
  ArTextTemplateStyle style;
} Scope;

typedef struct Parser {
  const char *text;
  size_t bytes, at;
  Scope scopes[kArTextTemplateMaximumDepth + 1];
  unsigned depth, runs;
  bool boundary, previous_value;
  ArTextTemplateEmit emit;
  void *context;
  ArTextTemplateError *error;
} Parser;

static bool Fail(ArTextTemplateError *error, size_t at, const char *message) {
  if (error) {
    error->offset = at;
    snprintf(error->message, sizeof(error->message), "%s", message);
  }
  return false;
}

static bool Letter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool Identifier(const char *text, size_t bytes) {
  if (!bytes || !Letter(text[0]))
    return false;
  for (size_t i = 1; i < bytes; ++i) {
    const char c = text[i];
    if (!Letter(c) && !(c >= '0' && c <= '9') && c != '_' && c != '-' &&
        c != '.')
      return false;
  }
  return true;
}

static int Hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool ArTextTemplate_StyleEqual(const ArTextTemplateStyle *a,
                               const ArTextTemplateStyle *b) {
  return !strcmp(a->font, b->font) && !strcmp(a->treatment, b->treatment) &&
         a->color_rgb == b->color_rgb && a->has_color == b->has_color &&
         a->scale_percent == b->scale_percent && a->italic == b->italic;
}

bool ArTextTemplate_SetProperty(ArTextTemplateStyle *style, const char *name,
                                const char *value, ArTextTemplateError *error) {
  if (!style || !name || !value)
    return Fail(error, 0, "invalid style property");
  const size_t bytes = strlen(value);
  if (bytes >= kArTextTemplateRoleCapacity)
    return Fail(error, 0, "style attribute is too long");
  if (!strcmp(name, "font") || !strcmp(name, "style")) {
    if (bytes >= kArTextTemplateRoleCapacity || !Identifier(value, bytes))
      return Fail(error, 0, "style role requires a stable identifier");
    char *destination = !strcmp(name, "font") ? style->font : style->treatment;
    memset(destination, 0, kArTextTemplateRoleCapacity);
    memcpy(destination, value, bytes);
  } else if (!strcmp(name, "color")) {
    if (bytes != 7 || value[0] != '#')
      return Fail(error, 0, "color must be #RRGGBB");
    uint32_t rgb = 0;
    for (size_t i = 1; i < bytes; ++i) {
      const int digit = Hex(value[i]);
      if (digit < 0)
        return Fail(error, 0, "color must be #RRGGBB");
      rgb = rgb * 16 + (uint32_t)digit;
    }
    style->has_color = true;
    style->color_rgb = rgb;
  } else if (!strcmp(name, "scale")) {
    if (bytes < 2 || value[bytes - 1] != '%')
      return Fail(error, 0, "scale must be an integer percentage");
    unsigned scale = 0;
    for (size_t i = 0; i + 1 < bytes; ++i) {
      if (value[i] < '0' || value[i] > '9')
        return Fail(error, 0, "scale must be an integer percentage");
      if (scale > kArTextTemplateMaximumScale)
        return Fail(error, 0, "scale must be 25% through 400%");
      scale = scale * 10 + (unsigned)(value[i] - '0');
    }
    if (scale < kArTextTemplateMinimumScale ||
        scale > kArTextTemplateMaximumScale)
      return Fail(error, 0, "scale must be 25% through 400%");
    style->scale_percent = (uint16_t)scale;
  } else if (!strcmp(name, "italic")) {
    if (strcmp(value, "true") && strcmp(value, "false"))
      return Fail(error, 0, "italic must be true or false");
    style->italic = !strcmp(value, "true") ? 2 : 1;
  } else {
    return Fail(error, 0, "unknown style property");
  }
  return true;
}

bool ArTextTreatment_SetProperty(ArTextTreatment *treatment, const char *name,
                                 const char *value,
                                 ArTextTemplateError *error) {
  if (!treatment || !name || !value)
    return Fail(error, 0, "invalid treatment property");
  if (!strcmp(name, "shape")) {
    if (strcmp(value, "diagonal") && strcmp(value, "keyline"))
      return Fail(error, 0, "shadow shape must be diagonal or keyline");
    treatment->keyline_shadow = !strcmp(value, "keyline");
    return true;
  }
  ArTextInk *destination = NULL;
  if (!strcmp(name, "band"))
    destination = &treatment->band;
  else if (!strcmp(name, "body"))
    destination = &treatment->body;
  else if (!strcmp(name, "shadow"))
    destination = &treatment->shadow;
  if (!destination)
    return Fail(error, 0, "unknown treatment property");
  ArTextInk ink = {0};
  if (!strcmp(name, "shadow") && !strcmp(value, "none")) {
    ink.kind = kArTextInk_None;
  } else if (!strncmp(value, "native:", 7) &&
             strlen(value + 7) < kArTextTemplateRoleCapacity &&
             Identifier(value + 7, strlen(value + 7))) {
    ink.kind = kArTextInk_Binding;
    strcpy(ink.binding, value + 7);
  } else {
    ArTextTemplateStyle solid = {0};
    if (!ArTextTemplate_SetProperty(&solid, "color", value, error))
      return Fail(error, 0, "ink requires #RRGGBB or native:binding");
    ink.kind = kArTextInk_Solid;
    ink.rgb = solid.color_rgb;
  }
  *destination = ink;
  return true;
}

static bool ResolveInk(const ArTextInk *ink,
                       const ArTextAppearanceBindings *bindings, uint32_t *rgb,
                       ArTextTemplateError *error) {
  if (ink->kind == kArTextInk_Solid && ink->rgb <= 0xFFFFFF) {
    *rgb = ink->rgb;
    return true;
  }
  if (ink->kind == kArTextInk_Binding && bindings && bindings->resolve_ink &&
      bindings->resolve_ink(bindings->context, ink->binding, rgb) &&
      *rgb <= 0xFFFFFF)
    return true;
  return Fail(error, 0, "style ink binding is unavailable");
}

static bool ValidStyleOverride(const ArTextTemplateStyle *style) {
  return memchr(style->font, 0, sizeof(style->font)) &&
         memchr(style->treatment, 0, sizeof(style->treatment)) &&
         (!style->font[0] || Identifier(style->font, strlen(style->font))) &&
         (!style->treatment[0] ||
          Identifier(style->treatment, strlen(style->treatment))) &&
         style->color_rgb <= 0xFFFFFF && style->italic <= 2 &&
         (!style->scale_percent ||
          (style->scale_percent >= kArTextTemplateMinimumScale &&
           style->scale_percent <= kArTextTemplateMaximumScale));
}

bool ArTextTemplate_ResolveAppearance(const ArTextTemplateStyle *defaults,
                                      const ArTextTemplateStyle *span,
                                      bool slant_ascii_numerals,
                                      const ArTextAppearanceBindings *bindings,
                                      ArTextRunAppearance *appearance,
                                      ArTextTemplateError *error) {
  const ArTextTemplateStyle inherited = {0};
  if (!defaults)
    defaults = &inherited;
  if (!span)
    span = &inherited;
  if (!appearance)
    return Fail(error, 0, "appearance output is required");
  if (!ValidStyleOverride(defaults) || !ValidStyleOverride(span))
    return Fail(error, 0, "invalid appearance override");
  const bool solid_override = span->has_color || defaults->has_color;
  ArTextRunAppearance resolved = {
      .band_rgb = 0xFFFFFF,
      .body_rgb = 0xFFFFFF,
      .slant_ascii_numerals = slant_ascii_numerals,
  };
  const char *font = span->font[0] ? span->font : defaults->font;
  strcpy(resolved.font_role, font[0] ? font : "body");
  resolved.italic = (span->italic ? span->italic : defaults->italic) == 2;
  if (resolved.italic)
    resolved.slant_ascii_numerals = false;
  resolved.scale_basis =
      (defaults->scale_percent ? defaults->scale_percent : 100u) *
      (span->scale_percent ? span->scale_percent : 100u);
  const char *name = span->treatment[0] ? span->treatment : defaults->treatment;
  if (name[0]) {
    const ArTextTreatment *treatment =
        bindings && bindings->find_treatment
            ? bindings->find_treatment(bindings->context, name)
            : NULL;
    if (!treatment)
      return Fail(error, 0, "named style is unavailable");
    if (!solid_override &&
        (!ResolveInk(&treatment->band, bindings, &resolved.band_rgb, error) ||
         !ResolveInk(&treatment->body, bindings, &resolved.body_rgb, error)))
      return false;
    resolved.shadow_enabled = treatment->shadow.kind != kArTextInk_None;
    resolved.keyline_shadow = treatment->keyline_shadow;
    if (resolved.shadow_enabled &&
        !ResolveInk(&treatment->shadow, bindings, &resolved.shadow_rgb, error))
      return false;
  }
  if (solid_override) {
    resolved.band_rgb = resolved.body_rgb =
        span->has_color ? span->color_rgb : defaults->color_rgb;
  }
  *appearance = resolved;
  return true;
}

static bool ValidSegment(const char *text, size_t bytes) {
  for (size_t i = 0; i < bytes;) {
    const uint8_t first = (uint8_t)text[i++];
    if (!first || first == '\r' || first == '\n')
      return false;
    if (first < 0x80)
      continue;
    unsigned remaining;
    uint32_t scalar, minimum;
    if (first >= 0xc2 && first <= 0xdf) {
      remaining = 1;
      scalar = first & 31;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      remaining = 2;
      scalar = first & 15;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      remaining = 3;
      scalar = first & 7;
      minimum = 0x10000;
    } else
      return false;
    if (remaining > bytes - i)
      return false;
    while (remaining--) {
      const uint8_t next = (uint8_t)text[i++];
      if ((next & 0xc0) != 0x80)
        return false;
      scalar = (scalar << 6) | (next & 63);
    }
    if (scalar < minimum || scalar > 0x10ffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff))
      return false;
  }
  return true;
}

static bool Emit(Parser *p, size_t start, size_t bytes, size_t source,
                 bool value, uint8_t digits) {
  const bool starts = !p->runs || p->boundary || value || p->previous_value;
  if (starts && ++p->runs > kArTextTemplateMaximumRuns)
    return Fail(p->error, source, "too many inline runs");
  const ArTextTemplateRun run = {
      .text = p->text + start,
      .bytes = bytes,
      .source_offset = source,
      .is_value = value,
      .minimum_digits = digits,
      .starts_run = starts,
      .style = p->scopes[p->depth].style,
  };
  if (p->emit && !p->emit(p->context, &run))
    return Fail(p->error, source, "cannot retain inline run");
  p->boundary = false;
  p->previous_value = value;
  return true;
}

static void Spaces(Parser *p) {
  while (p->at < p->bytes && (p->text[p->at] == ' ' || p->text[p->at] == '\t'))
    ++p->at;
}

static bool Name(Parser *p, char *name, size_t capacity) {
  const size_t start = p->at;
  while (p->at < p->bytes && p->text[p->at] >= 'a' && p->text[p->at] <= 'z')
    ++p->at;
  const size_t bytes = p->at - start;
  if (!bytes || bytes >= capacity)
    return false;
  memcpy(name, p->text + start, bytes);
  name[bytes] = 0;
  return true;
}

static unsigned PropertyBit(const char *name) {
  static const char *const names[] = {"font", "style", "color", "scale",
                                      "italic"};
  for (unsigned i = 0; i < sizeof(names) / sizeof(*names); ++i)
    if (!strcmp(name, names[i]))
      return 1u << i;
  return 0;
}

static bool Tag(Parser *p) {
  const size_t start = p->at++;
  const bool closing = p->at < p->bytes && p->text[p->at] == '/';
  if (closing)
    ++p->at;
  char name[16];
  if (!Name(p, name, sizeof(name)) ||
      (strcmp(name, "i") && strcmp(name, "span")))
    return Fail(p->error, start, "unknown style tag (use \\< for literal '<')");
  const bool italic = !strcmp(name, "i");
  if (closing) {
    if (p->at == p->bytes || p->text[p->at] != '>' || !p->depth ||
        p->scopes[p->depth].italic_tag != italic)
      return Fail(p->error, start, "mismatched closing style tag");
    ++p->at;
    --p->depth;
    p->boundary = true;
    return true;
  }
  if (p->depth == kArTextTemplateMaximumDepth)
    return Fail(p->error, start, "style nesting exceeds limit");
  Scope scope = {.italic_tag = italic, .style = p->scopes[p->depth].style};
  if (italic)
    scope.style.italic = 2;
  unsigned seen = 0;
  while (p->at < p->bytes && p->text[p->at] != '>') {
    const size_t before = p->at;
    Spaces(p);
    if (p->at < p->bytes && p->text[p->at] == '>')
      break;
    if (p->at == before || italic)
      return Fail(p->error, p->at, "expected style attribute or '>'");
    const size_t at = p->at;
    char key[16];
    if (!Name(p, key, sizeof(key)))
      return Fail(p->error, at, "missing style attribute");
    const unsigned bit = PropertyBit(key);
    if (!bit || (seen & bit))
      return Fail(p->error, at, "unknown or duplicate style attribute");
    seen |= bit;
    Spaces(p);
    if (p->at == p->bytes || p->text[p->at] != '=')
      return Fail(p->error, p->at, "expected '=' after style attribute");
    ++p->at;
    Spaces(p);
    if (p->at == p->bytes || p->text[p->at] != '"')
      return Fail(p->error, p->at, "style values require double quotes");
    const size_t value_at = ++p->at;
    while (p->at < p->bytes && p->text[p->at] != '"')
      ++p->at;
    if (p->at == p->bytes)
      return Fail(p->error, value_at, "unclosed style attribute");
    const size_t bytes = p->at - value_at;
    char value[kArTextTemplateRoleCapacity];
    if (bytes >= sizeof(value))
      return Fail(p->error, value_at, "style attribute is too long");
    memcpy(value, p->text + value_at, bytes);
    value[bytes] = 0;
    if (!ArTextTemplate_SetProperty(&scope.style, key, value, p->error)) {
      if (p->error)
        p->error->offset = at;
      return false;
    }
    ++p->at;
  }
  if (p->at == p->bytes)
    return Fail(p->error, start, "unclosed style tag");
  if (!italic && !seen)
    return Fail(p->error, start, "span requires at least one style attribute");
  ++p->at;
  p->scopes[++p->depth] = scope;
  p->boundary = true;
  return true;
}

static bool Value(Parser *p) {
  const size_t start = p->at;
  if (p->text[start] == '}')
    return Fail(p->error, start, "unmatched '}' (write '}}' for a literal)");
  const char *end = memchr(p->text + start + 1, '}', p->bytes - start - 1);
  if (!end)
    return Fail(p->error, start, "unclosed placeholder");
  size_t bytes = (size_t)(end - p->text) - start - 1;
  if (bytes >= 256)
    return Fail(p->error, start, "invalid placeholder");
  uint8_t digits = 0;
  const char *colon = memchr(p->text + start + 1, ':', bytes);
  if (colon) {
    if (end - colon != 3 || colon[1] != '0' || colon[2] < '1' || colon[2] > '9')
      return Fail(p->error, start, "number format must be 01 through 09");
    digits = (uint8_t)(colon[2] - '0');
    bytes = (size_t)(colon - p->text) - start - 1;
  }
  if (!Identifier(p->text + start + 1, bytes))
    return Fail(p->error, start, "invalid placeholder");
  p->at = (size_t)(end - p->text) + 1;
  return Emit(p, start + 1, bytes, start, true, digits);
}

bool ArTextTemplate_ParseInline(const char *text, size_t bytes,
                                ArTextTemplateEmit emit, void *context,
                                ArTextTemplateError *error) {
  if (error)
    memset(error, 0, sizeof(*error));
  if (!text || bytes > kArTextTemplateMaximumBytes)
    return Fail(error, 0, "inline text exceeds size limit");
  if (!ValidSegment(text, bytes))
    return Fail(error, 0, "inline text must be one UTF-8 segment");
  Parser p = {.text = text,
              .bytes = bytes,
              .emit = emit,
              .context = context,
              .error = error};
  while (p.at < bytes) {
    const size_t start = p.at;
    const char c = text[start];
    if (c == '<') {
      if (!Tag(&p))
        return false;
    } else if (c == '{' || c == '}') {
      if (p.at + 1 < bytes && text[p.at + 1] == c) {
        p.at += 2;
        if (!Emit(&p, start, 1, start, false, 0))
          return false;
      } else if (!Value(&p))
        return false;
    } else if (c == '\\') {
      if (++p.at == bytes || (text[p.at] != '<' && text[p.at] != '\\'))
        return Fail(error, start, "expected \\< or \\\\ escape");
      ++p.at;
      if (!Emit(&p, start + 1, 1, start, false, 0))
        return false;
    } else {
      while (p.at < bytes && !strchr("<{}\\", text[p.at])) {
        if (text[p.at] == '|' && p.depth)
          return Fail(error, p.at, "close style tags before a cell separator");
        ++p.at;
      }
      if (!Emit(&p, start, p.at - start, start, false, 0))
        return false;
    }
  }
  return p.depth ? Fail(error, p.at, "unclosed style tag") : true;
}
