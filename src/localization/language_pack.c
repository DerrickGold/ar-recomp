#include "localization/language_pack.h"

#include "deterministic_hash.h"
#include "text_parse_utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kManifestMaximumBytes = 256 * 1024,
  kScriptMaximumBytes = 16 * 1024 * 1024,
  kFontMaximumBytes = 64 * 1024 * 1024,
  kMaximumSources = 64,
  kMaximumMessages = 16384,
  kMaximumOperationsPerMessage = 4096,
  kMaximumAuthoredPages = 64,
  kMaximumMessageTextBytes = 256 * 1024,
  kMaximumWaitFrames = 600,
  kMaximumMessageWaitFrames = 3600,
  kMaximumEventArgumentBytes = 1024,
  kPackMagic = UINT32_C(0x41524C50), /* ARLP */
};

typedef struct ManifestSources {
  char paths[kMaximumSources][kArLanguageFontPathCapacity];
  uint32_t count;
} ManifestSources;

typedef enum ManifestSection {
  kManifestSection_None = 0,
  kManifestSection_Pack,
  kManifestSection_Fonts,
  kManifestSection_Scripts,
} ManifestSection;

typedef struct ManifestSeen {
  bool pack_section;
  bool fonts_section;
  bool scripts_section;
  bool format;
  bool version;
  bool package_id;
  bool locale;
  bool name;
  bool autonym;
  bool author;
  bool license;
  bool direction;
  bool target;
  bool source_profile;
  bool fallback;
  bool coverage;
  bool description;
  bool primary_font;
} ManifestSeen;

typedef struct ScriptState {
  ArLanguagePack *pack;
  const char *path;
  uint32_t message_index;
  bool has_message;
  bool previous_text_line;
  bool ended;
  uint32_t message_text_bytes;
  uint32_t message_pages;
  uint32_t message_wait_frames;
} ScriptState;

static void SetError(ArLanguagePackError *error, const char *format, ...) {
  if (!error)
    return;
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(error->message, sizeof(error->message), format, arguments);
  va_end(arguments);
}

static void ClearError(ArLanguagePackError *error) {
  if (error)
    error->message[0] = 0;
}

static bool IsValidUtf8(const uint8_t *bytes, size_t size) {
  size_t index = 0;
  while (index < size) {
    const uint8_t first = bytes[index++];
    if (first <= UINT8_C(0x7F))
      continue;
    uint32_t codepoint = 0;
    uint32_t remaining = 0;
    if (first >= UINT8_C(0xC2) && first <= UINT8_C(0xDF)) {
      codepoint = first & UINT8_C(0x1F);
      remaining = 1;
    } else if (first >= UINT8_C(0xE0) && first <= UINT8_C(0xEF)) {
      codepoint = first & UINT8_C(0x0F);
      remaining = 2;
    } else if (first >= UINT8_C(0xF0) && first <= UINT8_C(0xF4)) {
      codepoint = first & UINT8_C(0x07);
      remaining = 3;
    } else {
      return false;
    }
    if (remaining > size - index)
      return false;
    for (uint32_t i = 0; i < remaining; i++) {
      const uint8_t next = bytes[index++];
      if ((next & UINT8_C(0xC0)) != UINT8_C(0x80))
        return false;
      codepoint = (codepoint << 6) | (next & UINT8_C(0x3F));
    }
    if ((remaining == 2 && codepoint < UINT32_C(0x800)) ||
        (remaining == 3 && codepoint < UINT32_C(0x10000)) ||
        codepoint > UINT32_C(0x10FFFF) ||
        (codepoint >= UINT32_C(0xD800) && codepoint <= UINT32_C(0xDFFF)))
      return false;
  }
  return true;
}

static bool ContainsNul(const uint8_t *bytes, size_t size) {
  return memchr(bytes, 0, size) != NULL;
}

static bool ReadBlob(const ArLanguagePackIo *io, const char *path,
                     size_t maximum_bytes, ArLanguagePackBlob *blob,
                     ArLanguagePackError *error) {
  if (!io || io->struct_size < sizeof(*io) ||
      io->abi_version != AR_LANGUAGE_PACK_IO_ABI_VERSION || !io->read_file ||
      !io->release_file) {
    SetError(error, "invalid language-pack I/O adapter");
    return false;
  }
  memset(blob, 0, sizeof(*blob));
  blob->struct_size = sizeof(*blob);
  char detail[kArLanguagePackErrorCapacity] = {0};
  if (!io->read_file(io->context, path, maximum_bytes, blob, detail,
                     sizeof(detail))) {
    SetError(error, "%s: %s", path,
             detail[0] ? detail : "could not read file");
    return false;
  }
  if (blob->struct_size < sizeof(*blob) ||
      (blob->size != 0 && !blob->data) || blob->size > maximum_bytes) {
    io->release_file(io->context, blob);
    memset(blob, 0, sizeof(*blob));
    SetError(error, "%s: invalid or oversized file blob", path);
    return false;
  }
  return true;
}

static char *CopyTextBlob(const ArLanguagePackBlob *blob, const char *path,
                          ArLanguagePackError *error) {
  if (ContainsNul(blob->data, blob->size)) {
    SetError(error, "%s: contains a NUL byte", path);
    return NULL;
  }
  if (!IsValidUtf8(blob->data, blob->size)) {
    SetError(error, "%s: is not valid UTF-8", path);
    return NULL;
  }
  char *text = (char *)malloc(blob->size + 1);
  if (!text) {
    SetError(error, "out of memory while reading %s", path);
    return NULL;
  }
  memcpy(text, blob->data, blob->size);
  text[blob->size] = 0;
  if (blob->size >= 3 && (uint8_t)text[0] == UINT8_C(0xEF) &&
      (uint8_t)text[1] == UINT8_C(0xBB) &&
      (uint8_t)text[2] == UINT8_C(0xBF))
    memmove(text, text + 3, blob->size - 2);
  return text;
}

static bool IsAsciiAlpha(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

static bool IsAsciiDigit(char value) { return value >= '0' && value <= '9'; }

static bool IsIdentifier(const char *value) {
  if (!value || !IsAsciiAlpha(value[0]))
    return false;
  for (const char *cursor = value + 1; *cursor; cursor++) {
    if (!IsAsciiAlpha(*cursor) && !IsAsciiDigit(*cursor) && *cursor != '_' &&
        *cursor != '.' && *cursor != '-')
      return false;
  }
  return true;
}

static bool IsLocale(const char *value) {
  const char *cursor = value;
  size_t part_length = 0;
  while (IsAsciiAlpha(*cursor)) {
    cursor++;
    part_length++;
  }
  if (part_length < 2 || part_length > 8)
    return false;
  while (*cursor) {
    if (*cursor++ != '-')
      return false;
    part_length = 0;
    while (IsAsciiAlpha(*cursor) || IsAsciiDigit(*cursor)) {
      cursor++;
      part_length++;
    }
    if (part_length < 1 || part_length > 8)
      return false;
  }
  return true;
}

static bool IsReservedPathComponent(const char *part, size_t length) {
  size_t base_length = 0;
  while (base_length < length && part[base_length] != '.') base_length++;
  while (base_length && part[base_length - 1] == ' ') base_length--;
  if (base_length > 7) return false;
  char base[8] = {0};
  for (size_t i = 0; i < base_length; i++)
    base[i] = part[i] >= 'a' && part[i] <= 'z'
                  ? (char)(part[i] - 'a' + 'A') : part[i];
  if (!strcmp(base, "CON") || !strcmp(base, "PRN") ||
      !strcmp(base, "AUX") || !strcmp(base, "NUL") ||
      !strcmp(base, "CONIN$") || !strcmp(base, "CONOUT$"))
    return true;
  if (strncmp(base, "COM", 3) && strncmp(base, "LPT", 3)) return false;
  return (base_length == 4 && base[3] >= '1' && base[3] <= '9') ||
         (base_length == 5 && (uint8_t)base[3] == 0xC2 &&
          ((uint8_t)base[4] == 0xB9 || (uint8_t)base[4] == 0xB2 ||
           (uint8_t)base[4] == 0xB3));
}

static bool IsPortableRelativePath(const char *path) {
  if (!path || !path[0] || path[0] == '/' || strchr(path, '\\') ||
      path[strlen(path) - 1] == '/')
    return false;
  const char *part = path;
  for (const char *cursor = path;; cursor++) {
    if (*cursor && ((uint8_t)*cursor < 0x20 || *cursor == 0x7F ||
                    strchr("<>:\"|?*", *cursor)))
      return false;
    if (*cursor == '/' || *cursor == 0) {
      const size_t length = (size_t)(cursor - part);
      if (length == 0 || (length == 1 && part[0] == '.') ||
          (length == 2 && part[0] == '.' && part[1] == '.') ||
          part[length - 1] == '.' || part[length - 1] == ' ' ||
          IsReservedPathComponent(part, length))
        return false;
      if (*cursor == 0)
        return true;
      part = cursor + 1;
    }
  }
}

static bool CopyField(char *destination, size_t capacity, const char *value,
                      const char *key, const char *path, uint32_t line,
                      ArLanguagePackError *error) {
  const size_t length = strlen(value);
  if (length >= capacity) {
    SetError(error, "%s:%u: %s is too long", path, line, key);
    return false;
  }
  memcpy(destination, value, length + 1);
  return true;
}

static bool ParseDirection(const char *value, ArLanguageDirection *direction) {
  if (strcmp(value, "auto") == 0)
    *direction = kArLanguageDirection_Auto;
  else if (strcmp(value, "ltr") == 0)
    *direction = kArLanguageDirection_LeftToRight;
  else if (strcmp(value, "rtl") == 0)
    *direction = kArLanguageDirection_RightToLeft;
  else
    return false;
  return true;
}

static bool ParseTarget(const char *value, ArLanguagePackTarget *target) {
  if (strcmp(value, "us-runtime") == 0)
    *target = kArLanguagePackTarget_UsRuntime;
  else if (strcmp(value, "reference-only") == 0)
    *target = kArLanguagePackTarget_ReferenceOnly;
  else
    return false;
  return true;
}

static bool ParseProfile(const char *value, ArLanguageSourceProfile *profile) {
  static const char *const names[] = {"us", "eu-en", "de", "fr", "jp"};
  for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strcmp(value, names[i]) == 0) {
      *profile = (ArLanguageSourceProfile)i;
      return true;
    }
  }
  return false;
}

static bool ParseCoverage(const char *value,
                          ArLanguagePackCoverage *coverage) {
  if (strcmp(value, "partial") == 0)
    *coverage = kArLanguagePackCoverage_Partial;
  else if (strcmp(value, "complete") == 0)
    *coverage = kArLanguagePackCoverage_Complete;
  else
    return false;
  return true;
}

static bool AddManifestPath(char paths[][kArLanguageFontPathCapacity],
                            uint32_t *count, uint32_t maximum,
                            const char *value, const char *kind,
                            const char *manifest_path, uint32_t line,
                            ArLanguagePackError *error) {
  if (*count >= maximum) {
    SetError(error, "%s:%u: too many %s entries", manifest_path, line, kind);
    return false;
  }
  const bool builtin_font = strcmp(kind, "fallback font") == 0 &&
                            strncmp(value, "builtin:", 8) == 0;
  if (!(builtin_font ? IsIdentifier(value + 8) : IsPortableRelativePath(value))) {
    SetError(error, "%s:%u: %s path must be portable and relative: %s",
             manifest_path, line, kind, value);
    return false;
  }
  for (uint32_t i = 0; i < *count; i++) {
    if (strcmp(paths[i], value) == 0) {
      SetError(error, "%s:%u: duplicate %s path: %s", manifest_path, line,
               kind, value);
      return false;
    }
  }
  if (!CopyField(paths[*count], kArLanguageFontPathCapacity, value, kind,
                 manifest_path, line, error))
    return false;
  (*count)++;
  return true;
}

static bool ParseManifest(char *text, const char *path,
                          ArLanguagePackMetadata *metadata,
                          ManifestSources *sources,
                          ArLanguagePackError *error) {
  ManifestSeen seen = {0};
  ManifestSection section = kManifestSection_None;
  uint32_t line_number = 0;
  memset(metadata, 0, sizeof(*metadata));
  memset(sources, 0, sizeof(*sources));

  for (char *physical = text; physical;) {
    char *next = strpbrk(physical, "\r\n");
    if (next) {
      const char separator = *next;
      *next++ = 0;
      if (separator == '\r' && *next == '\n')
        next++;
    }
    line_number++;
    char *line = TextParse_TrimLeft(physical);
    TextParse_TrimRight(line);
    if (!line[0] || line[0] == '#' || line[0] == ';') {
      physical = next;
      continue;
    }
    const size_t line_length = strlen(line);
    if (line[0] == '[' && line[line_length - 1] == ']') {
      line[line_length - 1] = 0;
      char *name = TextParse_TrimLeft(line + 1);
      TextParse_TrimRight(name);
      bool *section_seen = NULL;
      if (strcmp(name, "pack") == 0) {
        section = kManifestSection_Pack;
        section_seen = &seen.pack_section;
      } else if (strcmp(name, "fonts") == 0) {
        section = kManifestSection_Fonts;
        section_seen = &seen.fonts_section;
      } else if (strcmp(name, "scripts") == 0) {
        section = kManifestSection_Scripts;
        section_seen = &seen.scripts_section;
      } else {
        SetError(error, "%s:%u: unknown manifest section [%s]", path,
                 line_number, name);
        return false;
      }
      if (*section_seen) {
        SetError(error, "%s:%u: duplicate manifest section [%s]", path,
                 line_number, name);
        return false;
      }
      *section_seen = true;
      physical = next;
      continue;
    }
    if (section == kManifestSection_None) {
      SetError(error, "%s:%u: key appears before a section", path,
               line_number);
      return false;
    }
    char *equals = strchr(line, '=');
    if (!equals) {
      SetError(error, "%s:%u: expected key = value", path, line_number);
      return false;
    }
    *equals = 0;
    char *key = TextParse_TrimLeft(line);
    TextParse_TrimRight(key);
    char *value = TextParse_TrimLeft(equals + 1);
    TextParse_TrimRight(value);
    if (!key[0] || !value[0]) {
      SetError(error, "%s:%u: manifest keys and values must not be empty",
               path, line_number);
      return false;
    }

#define UNIQUE_KEY(flag)                                                        \
  do {                                                                          \
    if (seen.flag) {                                                             \
      SetError(error, "%s:%u: duplicate key '%s'", path, line_number, key);    \
      return false;                                                              \
    }                                                                            \
    seen.flag = true;                                                            \
  } while (0)

    if (section == kManifestSection_Pack) {
      if (strcmp(key, "format") == 0) {
        UNIQUE_KEY(format);
        if (strcmp(value, "actraiser-language-pack") != 0) {
          SetError(error, "%s:%u: unsupported language-pack format", path,
                   line_number);
          return false;
        }
      } else if (strcmp(key, "version") == 0) {
        UNIQUE_KEY(version);
        if (strcmp(value, "1") != 0) {
          SetError(error, "%s:%u: unsupported language-pack version", path,
                   line_number);
          return false;
        }
      } else if (strcmp(key, "id") == 0) {
        UNIQUE_KEY(package_id);
        if (!IsIdentifier(value)) {
          SetError(error, "%s:%u: invalid package id '%s'", path, line_number,
                   value);
          return false;
        }
        if (!CopyField(metadata->package_id, sizeof(metadata->package_id),
                       value, "package id", path, line_number, error))
          return false;
      } else if (strcmp(key, "locale") == 0) {
        UNIQUE_KEY(locale);
        if (!IsLocale(value)) {
          SetError(error, "%s:%u: invalid BCP-47 locale '%s'", path,
                   line_number, value);
          return false;
        }
        if (!CopyField(metadata->locale, sizeof(metadata->locale), value,
                       "locale", path, line_number, error))
          return false;
      } else if (strcmp(key, "name") == 0) {
        UNIQUE_KEY(name);
        if (!CopyField(metadata->display_name,
                       sizeof(metadata->display_name), value, "name", path,
                       line_number, error))
          return false;
      } else if (strcmp(key, "autonym") == 0) {
        UNIQUE_KEY(autonym);
        if (!CopyField(metadata->autonym, sizeof(metadata->autonym), value,
                       "autonym", path, line_number, error))
          return false;
      } else if (strcmp(key, "author") == 0) {
        UNIQUE_KEY(author);
        if (!CopyField(metadata->author, sizeof(metadata->author), value,
                       "author", path, line_number, error))
          return false;
      } else if (strcmp(key, "license") == 0) {
        UNIQUE_KEY(license);
        if (!CopyField(metadata->license, sizeof(metadata->license), value,
                       "license", path, line_number, error))
          return false;
      } else if (strcmp(key, "direction") == 0) {
        UNIQUE_KEY(direction);
        if (!ParseDirection(value, &metadata->direction)) {
          SetError(error, "%s:%u: direction must be ltr, rtl, or auto", path,
                   line_number);
          return false;
        }
      } else if (strcmp(key, "target") == 0) {
        UNIQUE_KEY(target);
        if (!ParseTarget(value, &metadata->target)) {
          SetError(error,
                   "%s:%u: target must be us-runtime or reference-only", path,
                   line_number);
          return false;
        }
      } else if (strcmp(key, "source_profile") == 0) {
        UNIQUE_KEY(source_profile);
        if (!ParseProfile(value, &metadata->source_profile)) {
          SetError(error, "%s:%u: unsupported source_profile '%s'", path,
                   line_number, value);
          return false;
        }
      } else if (strcmp(key, "fallback") == 0) {
        UNIQUE_KEY(fallback);
        if (strcmp(value, "native-us") != 0) {
          SetError(error, "%s:%u: v1 fallback must be native-us", path,
                   line_number);
          return false;
        }
        if (!CopyField(metadata->fallback, sizeof(metadata->fallback), value,
                       "fallback", path, line_number, error))
          return false;
      } else if (strcmp(key, "coverage") == 0) {
        UNIQUE_KEY(coverage);
        if (!ParseCoverage(value, &metadata->coverage)) {
          SetError(error, "%s:%u: coverage must be partial or complete", path,
                   line_number);
          return false;
        }
      } else if (strcmp(key, "description") == 0) {
        UNIQUE_KEY(description);
      } else {
        SetError(error, "%s:%u: unknown key '%s' in [pack]", path,
                 line_number, key);
        return false;
      }
    } else if (section == kManifestSection_Fonts) {
      if (strcmp(key, "primary") == 0) {
        UNIQUE_KEY(primary_font);
        if (!CopyField(metadata->primary_font, sizeof(metadata->primary_font),
                       value, "primary font", path, line_number, error))
          return false;
      } else if (strcmp(key, "fallback") == 0) {
        if (!AddManifestPath(metadata->fallback_fonts,
                             &metadata->fallback_font_count,
                             kArLanguageMaximumFallbackFonts, value,
                             "fallback font", path, line_number, error))
          return false;
      } else {
        SetError(error, "%s:%u: unknown key '%s' in [fonts]", path,
                 line_number, key);
        return false;
      }
    } else {
      if (strcmp(key, "source") != 0) {
        SetError(error, "%s:%u: unknown key '%s' in [scripts]", path,
                 line_number, key);
        return false;
      }
      if (!AddManifestPath(sources->paths, &sources->count, kMaximumSources,
                           value, "script source", path, line_number, error))
        return false;
      const size_t value_length = strlen(value);
      if (value_length < 7 || strcmp(value + value_length - 7, ".artext") != 0) {
        SetError(error, "%s:%u: script source must end in .artext: %s", path,
                 line_number, value);
        return false;
      }
    }
#undef UNIQUE_KEY
    physical = next;
  }

  if (!seen.pack_section || !seen.fonts_section || !seen.scripts_section) {
    SetError(error, "%s: missing required manifest section", path);
    return false;
  }
  if (!seen.format || !seen.version || !seen.package_id || !seen.locale ||
      !seen.name || !seen.autonym || !seen.author || !seen.license ||
      !seen.direction || !seen.target || !seen.source_profile || !seen.fallback ||
      !seen.coverage) {
    SetError(error, "%s: missing required [pack] key", path);
    return false;
  }
  if (!seen.primary_font) {
    SetError(error, "%s: missing [fonts] primary", path);
    return false;
  }
  if (sources->count == 0) {
    SetError(error, "%s: missing [scripts] source", path);
    return false;
  }
  if (strncmp(metadata->primary_font, "builtin:", 8) == 0) {
    if (!IsIdentifier(metadata->primary_font + 8)) {
      SetError(error, "%s: invalid built-in font id", path);
      return false;
    }
  } else if (!IsPortableRelativePath(metadata->primary_font)) {
    SetError(error, "%s: primary font path must be portable and relative",
             path);
    return false;
  }
  for (uint32_t i = 0; i < metadata->fallback_font_count; i++) {
    const char *font = metadata->fallback_fonts[i];
    if (strncmp(font, "builtin:", 8) == 0) {
      if (!IsIdentifier(font + 8)) {
        SetError(error, "%s: invalid built-in fallback font id", path);
        return false;
      }
    }
  }
  if (metadata->target == kArLanguagePackTarget_UsRuntime &&
      metadata->source_profile != kArLanguageSourceProfile_Us) {
    SetError(error,
             "%s: us-runtime packs must use the U.S. semantic contract", path);
    return false;
  }
  return true;
}

static uint64_t HashSize(uint64_t hash, uint64_t value) {
  for (uint32_t shift = 0; shift < 64; shift += 8)
    hash = DeterministicHash_Fnv1a64Byte(
        hash, (uint8_t)(value >> (56 - shift)));
  return hash;
}

static uint64_t HashPart(uint64_t hash, const char *kind, const char *name,
                         const uint8_t *data, size_t size) {
  const size_t kind_size = strlen(kind);
  const size_t name_size = strlen(name);
  hash = HashSize(hash, kind_size);
  hash = DeterministicHash_Fnv1a64(hash, kind, kind_size);
  hash = HashSize(hash, name_size);
  hash = DeterministicHash_Fnv1a64(hash, name, name_size);
  hash = HashSize(hash, size);
  return DeterministicHash_Fnv1a64(hash, data, size);
}

static bool Reserve(void **storage, size_t item_size, size_t needed,
                    size_t *capacity, ArLanguagePackError *error) {
  if (needed <= *capacity)
    return true;
  size_t next = *capacity ? *capacity : 16;
  while (next < needed) {
    if (next > SIZE_MAX / 2) {
      SetError(error, "language pack is too large");
      return false;
    }
    next *= 2;
  }
  if (next > SIZE_MAX / item_size) {
    SetError(error, "language pack is too large");
    return false;
  }
  void *grown = realloc(*storage, next * item_size);
  if (!grown) {
    SetError(error, "out of memory while loading language pack");
    return false;
  }
  *storage = grown;
  *capacity = next;
  return true;
}

static bool AddStringN(ArLanguagePack *pack, const char *value, size_t length,
                       ArLanguageString *string, ArLanguagePackError *error) {
  if (length > UINT32_MAX || pack->strings_size > UINT32_MAX - length - 1) {
    SetError(error, "language-pack string table is too large");
    return false;
  }
  if (!Reserve((void **)&pack->strings, 1, pack->strings_size + length + 1,
               &pack->strings_capacity, error))
    return false;
  string->offset = (uint32_t)pack->strings_size;
  string->length = (uint32_t)length;
  memcpy(pack->strings + pack->strings_size, value, length);
  pack->strings_size += length;
  pack->strings[pack->strings_size++] = 0;
  return true;
}

static bool AddString(ArLanguagePack *pack, const char *value,
                      ArLanguageString *string, ArLanguagePackError *error) {
  return AddStringN(pack, value, strlen(value), string, error);
}

static const char *StringAt(const ArLanguagePack *pack,
                            ArLanguageString string) {
  if (!pack || !pack->strings || string.offset >= pack->strings_size ||
      string.length >= pack->strings_size - string.offset)
    return NULL;
  return pack->strings + string.offset;
}

static bool AddOperation(ScriptState *state, ArLanguageOperation operation,
                         ArLanguagePackError *error) {
  ArLanguageMessage *message = &state->pack->messages[state->message_index];
  if (state->pack->operation_count - message->first_operation >=
      kMaximumOperationsPerMessage) {
    SetError(error, "%s:%u: message has too many operations", state->path,
             operation.source_line);
    return false;
  }
  if (!Reserve((void **)&state->pack->operations,
               sizeof(*state->pack->operations),
               (size_t)state->pack->operation_count + 1,
               &state->pack->operation_capacity, error))
    return false;
  state->pack->operations[state->pack->operation_count++] = operation;
  message->operation_count++;
  return true;
}

static bool AddText(ScriptState *state, const char *value, size_t length,
                    uint32_t line, ArLanguagePackError *error) {
  if (!length)
    return true;
  if (length > kMaximumMessageTextBytes - state->message_text_bytes) {
    SetError(error, "%s:%u: message text is too large", state->path, line);
    return false;
  }
  ArLanguageMessage *message = &state->pack->messages[state->message_index];
  if (message->operation_count) {
    ArLanguageOperation *last =
        &state->pack->operations[message->first_operation +
                                 message->operation_count - 1];
    if (last->kind == kArLanguageOperation_Text &&
        last->source_line == line &&
        (size_t)last->value.text.offset + last->value.text.length + 1 ==
            state->pack->strings_size) {
      if (!Reserve((void **)&state->pack->strings, 1,
                   state->pack->strings_size + length,
                   &state->pack->strings_capacity, error))
        return false;
      const size_t terminal = state->pack->strings_size - 1;
      memcpy(state->pack->strings + terminal, value, length);
      state->pack->strings_size += length;
      state->pack->strings[state->pack->strings_size - 1] = 0;
      last->value.text.length += (uint32_t)length;
      state->message_text_bytes += (uint32_t)length;
      return true;
    }
  }
  ArLanguageOperation operation = {0};
  operation.kind = kArLanguageOperation_Text;
  operation.source_line = line;
  if (!AddStringN(state->pack, value, length, &operation.value.text, error))
    return false;
  state->message_text_bytes += (uint32_t)length;
  return AddOperation(state, operation, error);
}

static bool AddSimpleOperation(ScriptState *state, ArLanguageOperationKind kind,
                               uint32_t line, ArLanguagePackError *error) {
  ArLanguageOperation operation = {0};
  operation.kind = kind;
  operation.source_line = line;
  return AddOperation(state, operation, error);
}

static bool AppendInline(ScriptState *state, const char *value, uint32_t line,
                         ArLanguagePackError *error) {
  const char *literal = value;
  const char *cursor = value;
  while (*cursor) {
    if (*cursor == '{') {
      if (cursor[1] == '{') {
        if (!AddText(state, literal, (size_t)(cursor - literal), line, error) ||
            !AddText(state, "{", 1, line, error))
          return false;
        cursor += 2;
        literal = cursor;
        continue;
      }
      const char *end = strchr(cursor + 1, '}');
      if (!end) {
        SetError(error, "%s:%u: unclosed placeholder", state->path, line);
        return false;
      }
      if (!AddText(state, literal, (size_t)(cursor - literal), line, error))
        return false;
      const size_t name_size = (size_t)(end - cursor - 1);
      char name[256];
      if (name_size == 0 || name_size >= sizeof(name)) {
        SetError(error, "%s:%u: invalid placeholder", state->path, line);
        return false;
      }
      memcpy(name, cursor + 1, name_size);
      name[name_size] = 0;
      uint8_t minimum_digits = 0;
      char *format = strchr(name, ':');
      if (format) {
        if (format[1] != '0' || format[2] < '1' || format[2] > '9' ||
            format[3]) {
          SetError(error, "%s:%u: number format must be 01 through 09",
                   state->path, line);
          return false;
        }
        minimum_digits = (uint8_t)(format[2] - '0');
        *format = 0;
      }
      if (!IsIdentifier(name)) {
        SetError(error, "%s:%u: invalid placeholder '%s'", state->path, line,
                 name);
        return false;
      }
      ArLanguageOperation operation = {0};
      operation.kind = kArLanguageOperation_Placeholder;
      operation.source_line = line;
      operation.minimum_digits = minimum_digits;
      if (!AddString(state->pack, name, &operation.value.placeholder, error) ||
          !AddOperation(state, operation, error))
        return false;
      cursor = end + 1;
      literal = cursor;
      continue;
    }
    if (*cursor == '}') {
      if (cursor[1] == '}') {
        if (!AddText(state, literal, (size_t)(cursor - literal), line, error) ||
            !AddText(state, "}", 1, line, error))
          return false;
        cursor += 2;
        literal = cursor;
        continue;
      }
      SetError(error,
               "%s:%u: unmatched '}' (write '}}' for a literal)", state->path,
               line);
      return false;
    }
    cursor++;
  }
  return AddText(state, literal, (size_t)(cursor - literal), line, error);
}

static bool TokenizeCommand(char *line, char **tokens, uint32_t *token_count,
                            uint32_t maximum, const char *path,
                            uint32_t line_number, ArLanguagePackError *error) {
  char *source = line;
  char *write = line;
  *token_count = 0;
  while (*source) {
    while (*source == ' ' || *source == '\t')
      source++;
    if (!*source)
      break;
    if (*token_count >= maximum) {
      SetError(error, "%s:%u: command has too many arguments", path,
               line_number);
      return false;
    }
    tokens[(*token_count)++] = write;
    char quote = 0;
    while (*source) {
      const char current = *source++;
      if (!quote && (current == ' ' || current == '\t'))
        break;
      if (!quote && (current == '\'' || current == '"')) {
        quote = current;
        continue;
      }
      if (quote && current == quote) {
        quote = 0;
        continue;
      }
      if (current == '\\' && quote != '\'') {
        if (!*source) {
          SetError(error, "%s:%u: trailing command escape", path,
                   line_number);
          return false;
        }
        /* Inside double quotes only quote/backslash are escaped. Retaining
         * other backslashes prevents silently turning an invalid identifier
         * into a different, valid native anchor. Match the author grammar. */
        if (quote == '"' && *source != '"' && *source != '\\')
          *write++ = current;
        *write++ = *source++;
        continue;
      }
      *write++ = current;
    }
    if (quote) {
      SetError(error, "%s:%u: unterminated command quote", path, line_number);
      return false;
    }
    *write++ = 0;
  }
  return true;
}

static bool ParseCommand(ScriptState *state, char *line, uint32_t line_number,
                         ArLanguagePackError *error) {
  char *tokens[128];
  uint32_t count = 0;
  if (!TokenizeCommand(line, tokens, &count,
                       sizeof(tokens) / sizeof(tokens[0]), state->path,
                       line_number, error))
    return false;
  if (!count)
    return true;
  const char *command = tokens[0];
  if (strcmp(command, "@line") == 0 ||
      strcmp(command, "@paragraph") == 0 ||
      strcmp(command, "@page") == 0 || strcmp(command, "@empty") == 0 ||
      strcmp(command, "@end") == 0) {
    if (count != 1) {
      SetError(error, "%s:%u: %s takes no arguments", state->path,
               line_number, command);
      return false;
    }
    ArLanguageOperationKind kind = kArLanguageOperation_LineBreak;
    if (strcmp(command, "@paragraph") == 0)
      kind = kArLanguageOperation_ParagraphBreak;
    else if (strcmp(command, "@page") == 0) {
      kind = kArLanguageOperation_PageBreak;
      if (++state->message_pages > kMaximumAuthoredPages) {
        SetError(error, "%s:%u: message has too many authored pages",
                 state->path, line_number);
        return false;
      }
    } else if (strcmp(command, "@empty") == 0)
      kind = kArLanguageOperation_Empty;
    else if (strcmp(command, "@end") == 0) {
      kind = kArLanguageOperation_End;
      state->ended = true;
    }
    return AddSimpleOperation(state, kind, line_number, error);
  }
  if (strcmp(command, "@anchor") == 0) {
    if (count != 2 || !IsIdentifier(tokens[1])) {
      SetError(error, "%s:%u: @anchor requires one stable anchor id",
               state->path, line_number);
      return false;
    }
    ArLanguageOperation operation = {0};
    operation.kind = kArLanguageOperation_Anchor;
    operation.source_line = line_number;
    return AddString(state->pack, tokens[1], &operation.value.anchor, error) &&
           AddOperation(state, operation, error);
  }
  if (strcmp(command, "@wait") == 0) {
    if (count != 2 || !tokens[1][0]) {
      SetError(error, "%s:%u: @wait requires a decimal frame count",
               state->path, line_number);
      return false;
    }
    for (const char *cursor = tokens[1]; *cursor; cursor++) {
      if (!IsAsciiDigit(*cursor)) {
        SetError(error, "%s:%u: @wait requires a decimal frame count",
                 state->path, line_number);
        return false;
      }
    }
    errno = 0;
    const unsigned long value = strtoul(tokens[1], NULL, 10);
    if (errno || value < 1 || value > kMaximumWaitFrames) {
      SetError(error, "%s:%u: @wait must be 1-%u frames", state->path,
               line_number, kMaximumWaitFrames);
      return false;
    }
    if (value > kMaximumMessageWaitFrames - state->message_wait_frames) {
      SetError(error, "%s:%u: cumulative authored wait is too long",
               state->path, line_number);
      return false;
    }
    state->message_wait_frames += (uint32_t)value;
    ArLanguageOperation operation = {0};
    operation.kind = kArLanguageOperation_WaitFrames;
    operation.source_line = line_number;
    operation.value.wait_frames = (uint32_t)value;
    return AddOperation(state, operation, error);
  }
  if (strcmp(command, "@event") == 0) {
    if (count < 2 || !IsIdentifier(tokens[1])) {
      SetError(error, "%s:%u: @event requires a valid event id", state->path,
               line_number);
      return false;
    }
    size_t argument_size = 0;
    for (uint32_t i = 2; i < count; i++)
      argument_size += strlen(tokens[i]) + (i != 2);
    if (argument_size > kMaximumEventArgumentBytes) {
      SetError(error, "%s:%u: @event arguments are too long", state->path,
               line_number);
      return false;
    }
    char arguments[kMaximumEventArgumentBytes + 1];
    size_t used = 0;
    for (uint32_t i = 2; i < count; i++) {
      if (used)
        arguments[used++] = ' ';
      const size_t length = strlen(tokens[i]);
      memcpy(arguments + used, tokens[i], length);
      used += length;
    }
    arguments[used] = 0;
    ArLanguageOperation operation = {0};
    operation.kind = kArLanguageOperation_Event;
    operation.source_line = line_number;
    return AddString(state->pack, tokens[1], &operation.value.event.id,
                     error) &&
           AddStringN(state->pack, arguments, used,
                      &operation.value.event.arguments, error) &&
           AddOperation(state, operation, error);
  }
  if (strcmp(command, "@alias") == 0) {
    ArLanguageMessage *message =
        &state->pack->messages[state->message_index];
    if (count != 2 || !IsIdentifier(tokens[1])) {
      SetError(error, "%s:%u: @alias requires one semantic message id",
               state->path, line_number);
      return false;
    }
    if (message->operation_count || message->is_alias) {
      SetError(error, "%s:%u: @alias must be the only message content",
               state->path, line_number);
      return false;
    }
    if (!AddString(state->pack, tokens[1], &message->alias, error))
      return false;
    message->is_alias = true;
    state->ended = true;
    return true;
  }
  SetError(error, "%s:%u: unknown command '%s'", state->path, line_number,
           command);
  return false;
}

static bool FinalizeMessage(ScriptState *state, ArLanguagePackError *error) {
  if (!state->has_message)
    return true;
  ArLanguageMessage *message = &state->pack->messages[state->message_index];
  while (message->operation_count) {
    ArLanguageOperation *last =
        &state->pack->operations[message->first_operation +
                                 message->operation_count - 1];
    if (last->kind != kArLanguageOperation_ParagraphBreak)
      break;
    message->operation_count--;
    state->pack->operation_count--;
  }
  if (message->is_alias) {
    if (message->operation_count) {
      SetError(error, "%s:%u: @alias message has other operations",
               state->path, message->source_line);
      return false;
    }
    return true;
  }
  uint32_t empty_count = 0;
  const ArLanguageOperation *bad_empty_operation = NULL;
  for (uint32_t i = 0; i < message->operation_count; i++) {
    const ArLanguageOperation *operation =
        &state->pack->operations[message->first_operation + i];
    if (operation->kind == kArLanguageOperation_Empty)
      empty_count++;
    else if (operation->kind != kArLanguageOperation_Anchor &&
             operation->kind != kArLanguageOperation_End)
      bad_empty_operation = operation;
  }
  if (empty_count && (empty_count != 1 || bad_empty_operation)) {
    SetError(error, "%s:%u: @empty permits only required @anchor commands",
             state->path,
             bad_empty_operation ? bad_empty_operation->source_line
                                 : message->source_line);
    return false;
  }
  if (!message->operation_count) {
    SetError(error, "%s:%u: message has no content; use @empty intentionally",
             state->path, message->source_line);
    return false;
  }
  const ArLanguageOperation *last =
      &state->pack->operations[message->first_operation +
                               message->operation_count - 1];
  if (last->kind != kArLanguageOperation_End &&
      !AddSimpleOperation(state, kArLanguageOperation_End,
                          message->source_line, error))
    return false;
  return true;
}

static bool BeginMessage(ScriptState *state, const char *semantic_id,
                         uint32_t line, ArLanguagePackError *error) {
  if (!FinalizeMessage(state, error))
    return false;
  if (!IsIdentifier(semantic_id)) {
    SetError(error, "%s:%u: invalid semantic message id '%s'", state->path,
             line, semantic_id);
    return false;
  }
  if (state->pack->message_count >= kMaximumMessages) {
    SetError(error, "%s:%u: pack has too many messages", state->path, line);
    return false;
  }
  for (uint32_t i = 0; i < state->pack->message_count; i++) {
    const char *existing = StringAt(state->pack, state->pack->messages[i].id);
    if (existing && strcmp(existing, semantic_id) == 0) {
      SetError(error, "%s:%u: duplicate message '%s'", state->path, line,
               semantic_id);
      return false;
    }
  }
  if (!Reserve((void **)&state->pack->messages,
               sizeof(*state->pack->messages),
               (size_t)state->pack->message_count + 1,
               &state->pack->message_capacity, error))
    return false;
  state->message_index = state->pack->message_count++;
  ArLanguageMessage *message = &state->pack->messages[state->message_index];
  memset(message, 0, sizeof(*message));
  message->first_operation = state->pack->operation_count;
  message->source_line = line;
  if (!AddString(state->pack, semantic_id, &message->id, error))
    return false;
  state->has_message = true;
  state->previous_text_line = false;
  state->ended = false;
  state->message_text_bytes = 0;
  state->message_pages = 1;
  state->message_wait_frames = 0;
  return true;
}

static bool ParseScript(ArLanguagePack *pack, char *text, const char *path,
                        ArLanguagePackError *error) {
  ScriptState state = {.pack = pack, .path = path};
  uint32_t line_number = 0;
  for (char *physical = text; physical;) {
    char *next = strpbrk(physical, "\r\n");
    if (next) {
      const char separator = *next;
      *next++ = 0;
      if (separator == '\r' && *next == '\n')
        next++;
    }
    line_number++;
    char *stripped = TextParse_TrimLeft(physical);
    char *trim_end = physical + strlen(physical);
    while (trim_end > stripped &&
           (trim_end[-1] == ' ' || trim_end[-1] == '\t' ||
            trim_end[-1] == '\r' || trim_end[-1] == '\n'))
      trim_end--;
    const char saved_trim_character = *trim_end;
    *trim_end = 0;
    if (strncmp(stripped, "::", 2) == 0) {
      char *semantic_id = TextParse_TrimLeft(stripped + 2);
      TextParse_TrimRight(semantic_id);
      if (!BeginMessage(&state, semantic_id, line_number, error))
        return false;
      physical = next;
      continue;
    }
    if (!state.has_message) {
      if (!stripped[0] || stripped[0] == '#' || stripped[0] == ';') {
        physical = next;
        continue;
      }
      SetError(error, "%s:%u: content appears before first :: message", path,
               line_number);
      return false;
    }
    if (state.ended) {
      if (!stripped[0] || stripped[0] == '#' || stripped[0] == ';') {
        physical = next;
        continue;
      }
      SetError(error, "%s:%u: content appears after @end or @alias", path,
               line_number);
      return false;
    }
    if (!stripped[0]) {
      state.previous_text_line = false;
      ArLanguageMessage *message = &pack->messages[state.message_index];
      if (message->operation_count) {
        const ArLanguageOperationKind last =
            pack->operations[message->first_operation +
                             message->operation_count - 1]
                .kind;
        if (last != kArLanguageOperation_LineBreak &&
            last != kArLanguageOperation_ParagraphBreak &&
            last != kArLanguageOperation_PageBreak &&
            !AddSimpleOperation(&state, kArLanguageOperation_ParagraphBreak,
                                line_number, error))
          return false;
      }
      physical = next;
      continue;
    }
    if (stripped[0] == '#' || stripped[0] == ';') {
      physical = next;
      continue;
    }
    *trim_end = saved_trim_character;
    char *line = physical;
    if (line[0] == '@' && line[1] == '@') {
      line++;
    } else if (line[0] == '\\' && (line[1] == '#' || line[1] == ';')) {
      line++;
    } else if (stripped[0] == '@') {
      if (!ParseCommand(&state, stripped, line_number, error))
        return false;
      state.previous_text_line = false;
      physical = next;
      continue;
    }
    if (state.previous_text_line &&
        !AddText(&state, " ", 1, line_number, error))
      return false;
    if (!AppendInline(&state, line, line_number, error))
      return false;
    state.previous_text_line = true;
    physical = next;
  }
  if (!state.has_message) {
    SetError(error, "%s: script contains no messages", path);
    return false;
  }
  return FinalizeMessage(&state, error);
}

static const ArLanguageMessage *FindMessageInternal(const ArLanguagePack *pack,
                                                    const char *id) {
  if (!pack || !id)
    return NULL;
  for (uint32_t i = 0; i < pack->message_count; i++) {
    const char *candidate = StringAt(pack, pack->messages[i].id);
    if (candidate && strcmp(candidate, id) == 0)
      return &pack->messages[i];
  }
  return NULL;
}

static bool ValidateAliases(const ArLanguagePack *pack,
                            ArLanguagePackError *error) {
  for (uint32_t i = 0; i < pack->message_count; i++) {
    const ArLanguageMessage *origin = &pack->messages[i];
    if (!origin->is_alias)
      continue;
    const ArLanguageMessage *cursor = origin;
    for (uint32_t depth = 0; depth <= pack->message_count; depth++) {
      const char *target_id = StringAt(pack, cursor->alias);
      const ArLanguageMessage *target = FindMessageInternal(pack, target_id);
      if (!target) {
        SetError(error, "%s: alias target '%s' is not included in this pack",
                 StringAt(pack, origin->id), target_id ? target_id : "");
        return false;
      }
      if (!target->is_alias)
        break;
      cursor = target;
      if (depth == pack->message_count) {
        SetError(error, "%s: alias cycle detected",
                 StringAt(pack, origin->id));
        return false;
      }
    }
  }
  return true;
}

static bool JoinManifestRelativePath(const char *manifest_path,
                                     const char *relative, char *result,
                                     size_t capacity,
                                     ArLanguagePackError *error) {
  const char *slash = strrchr(manifest_path, '/');
  const size_t prefix = slash ? (size_t)(slash - manifest_path + 1) : 0;
  const size_t relative_size = strlen(relative);
  if (prefix + relative_size >= capacity) {
    SetError(error, "%s: resolved pack path is too long", manifest_path);
    return false;
  }
  memcpy(result, manifest_path, prefix);
  memcpy(result + prefix, relative, relative_size + 1);
  return true;
}

static bool LoadManifest(const ArLanguagePackIo *io, const char *manifest_path,
                         ArLanguagePackMetadata *metadata,
                         ManifestSources *sources, uint64_t *revision,
                         ArLanguagePackError *error) {
  ArLanguagePackBlob blob;
  if (!ReadBlob(io, manifest_path, kManifestMaximumBytes, &blob, error))
    return false;
  char *text = CopyTextBlob(&blob, manifest_path, error);
  if (!text) {
    io->release_file(io->context, &blob);
    return false;
  }
  *revision = HashPart(DETERMINISTIC_HASH_FNV1A64_OFFSET, "manifest",
                       "pack.ini", blob.data, blob.size);
  const bool valid =
      ParseManifest(text, manifest_path, metadata, sources, error);
  free(text);
  io->release_file(io->context, &blob);
  return valid;
}

void ArLanguagePack_Init(ArLanguagePack *pack) {
  if (!pack)
    return;
  memset(pack, 0, sizeof(*pack));
  pack->private_magic = kPackMagic;
}

void ArLanguagePack_Destroy(ArLanguagePack *pack) {
  if (!pack)
    return;
  if (pack->private_magic == kPackMagic) {
    free(pack->messages);
    free(pack->operations);
    free(pack->strings);
  }
  memset(pack, 0, sizeof(*pack));
  pack->private_magic = kPackMagic;
}

bool ArLanguagePack_ReadMetadata(const ArLanguagePackIo *io,
                                 const char *manifest_path,
                                 ArLanguagePackMetadata *metadata,
                                 uint64_t *manifest_revision,
                                 ArLanguagePackError *error) {
  ClearError(error);
  if (!manifest_path || !manifest_path[0] || !metadata) {
    SetError(error, "manifest path and metadata destination are required");
    return false;
  }
  ManifestSources ignored;
  uint64_t revision = 0;
  ArLanguagePackMetadata temporary;
  if (!LoadManifest(io, manifest_path, &temporary, &ignored, &revision, error))
    return false;
  *metadata = temporary;
  if (manifest_revision)
    *manifest_revision = revision;
  return true;
}

bool ArLanguagePack_Load(ArLanguagePack *pack, const ArLanguagePackIo *io,
                         const char *manifest_path,
                         ArLanguagePackError *error) {
  ClearError(error);
  if (!pack || !manifest_path || !manifest_path[0]) {
    SetError(error, "pack and manifest path are required");
    return false;
  }
  ArLanguagePack temporary;
  ArLanguagePack_Init(&temporary);
  ManifestSources sources;
  if (!LoadManifest(io, manifest_path, &temporary.metadata, &sources,
                    &temporary.content_revision, error))
    goto failed;

  char resolved[1024];
  for (uint32_t i = 0; i < sources.count; i++) {
    if (!JoinManifestRelativePath(manifest_path, sources.paths[i], resolved,
                                  sizeof(resolved), error))
      goto failed;
    ArLanguagePackBlob blob;
    if (!ReadBlob(io, resolved, kScriptMaximumBytes, &blob, error))
      goto failed;
    char *text = CopyTextBlob(&blob, resolved, error);
    if (!text) {
      io->release_file(io->context, &blob);
      goto failed;
    }
    temporary.content_revision =
        HashPart(temporary.content_revision, "script", sources.paths[i],
                 blob.data, blob.size);
    const bool valid = ParseScript(&temporary, text, resolved, error);
    free(text);
    io->release_file(io->context, &blob);
    if (!valid)
      goto failed;
  }
  const char *fonts[1 + kArLanguageMaximumFallbackFonts];
  uint32_t font_count = 0;
  fonts[font_count++] = temporary.metadata.primary_font;
  for (uint32_t i = 0; i < temporary.metadata.fallback_font_count; i++)
    fonts[font_count++] = temporary.metadata.fallback_fonts[i];
  for (uint32_t i = 0; i < font_count; i++) {
    const char *font = fonts[i];
    if (strncmp(font, "builtin:", 8) == 0) {
      temporary.content_revision =
          HashPart(temporary.content_revision, "builtin-font", font, NULL, 0);
      continue;
    }
    if (!JoinManifestRelativePath(manifest_path, font, resolved,
                                  sizeof(resolved), error))
      goto failed;
    ArLanguagePackBlob blob;
    if (!ReadBlob(io, resolved, kFontMaximumBytes, &blob, error))
      goto failed;
    if (blob.size == 0) {
      io->release_file(io->context, &blob);
      SetError(error, "%s: font is empty", resolved);
      goto failed;
    }
    temporary.content_revision = HashPart(temporary.content_revision, "font",
                                          font, blob.data, blob.size);
    io->release_file(io->context, &blob);
  }
  if (!ValidateAliases(&temporary, error))
    goto failed;

  if (pack->private_magic == kPackMagic)
    ArLanguagePack_Destroy(pack);
  else
    ArLanguagePack_Init(pack);
  *pack = temporary;
  return true;

failed:
  ArLanguagePack_Destroy(&temporary);
  return false;
}

static bool FileRead(void *context, const char *path, size_t maximum_bytes,
                     ArLanguagePackBlob *out_blob, char *error,
                     size_t error_capacity) {
  (void)context;
  FILE *file = fopen(path, "rb");
  if (!file) {
    snprintf(error, error_capacity, "could not open file");
    return false;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    snprintf(error, error_capacity, "could not inspect file size");
    return false;
  }
  const long end = ftell(file);
  if (end < 0 || (uint64_t)end > maximum_bytes) {
    fclose(file);
    snprintf(error, error_capacity, "file exceeds size limit");
    return false;
  }
  rewind(file);
  uint8_t *data = NULL;
  if (end != 0) {
    data = (uint8_t *)malloc((size_t)end);
    if (!data) {
      fclose(file);
      snprintf(error, error_capacity, "out of memory");
      return false;
    }
    if (fread(data, 1, (size_t)end, file) != (size_t)end) {
      free(data);
      fclose(file);
      snprintf(error, error_capacity, "could not read complete file");
      return false;
    }
  }
  fclose(file);
  out_blob->struct_size = sizeof(*out_blob);
  out_blob->data = data;
  out_blob->size = (size_t)end;
  out_blob->token = (uintptr_t)data;
  return true;
}

static void FileRelease(void *context, ArLanguagePackBlob *blob) {
  (void)context;
  if (!blob)
    return;
  free((void *)blob->token);
  memset(blob, 0, sizeof(*blob));
}

void ArLanguagePackFileIo_Init(ArLanguagePackIo *io) {
  if (!io)
    return;
  memset(io, 0, sizeof(*io));
  io->struct_size = sizeof(*io);
  io->abi_version = AR_LANGUAGE_PACK_IO_ABI_VERSION;
  io->read_file = FileRead;
  io->release_file = FileRelease;
}

const ArLanguagePackMetadata *ArLanguagePack_GetMetadata(
    const ArLanguagePack *pack) {
  return pack && pack->private_magic == kPackMagic ? &pack->metadata : NULL;
}

uint32_t ArLanguagePack_MessageCount(const ArLanguagePack *pack) {
  return pack && pack->private_magic == kPackMagic ? pack->message_count : 0;
}

const ArLanguageMessage *ArLanguagePack_GetMessage(const ArLanguagePack *pack,
                                                   uint32_t index) {
  return pack && pack->private_magic == kPackMagic && index < pack->message_count
             ? &pack->messages[index]
             : NULL;
}

const ArLanguageMessage *ArLanguagePack_FindMessage(
    const ArLanguagePack *pack, const char *semantic_id) {
  return pack && pack->private_magic == kPackMagic
             ? FindMessageInternal(pack, semantic_id)
             : NULL;
}

const ArLanguageOperation *ArLanguagePack_GetOperation(
    const ArLanguagePack *pack, const ArLanguageMessage *message,
    uint32_t index) {
  if (!pack || pack->private_magic != kPackMagic || !message ||
      index >= message->operation_count)
    return NULL;
  bool belongs_to_pack = false;
  for (uint32_t i = 0; i < pack->message_count; i++) {
    if (message == &pack->messages[i]) {
      belongs_to_pack = true;
      break;
    }
  }
  if (!belongs_to_pack || message->first_operation >= pack->operation_count ||
      index >= pack->operation_count - message->first_operation)
    return NULL;
  return &pack->operations[message->first_operation + index];
}

const char *ArLanguagePack_GetString(const ArLanguagePack *pack,
                                     ArLanguageString string) {
  return pack && pack->private_magic == kPackMagic ? StringAt(pack, string)
                                                   : NULL;
}
