#include "actraiser/actraiser_localization_values.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

enum {
  kWramLairEnemyIndex = 0x0002,
  kWramLairCount = 0x0006,
  kWramSoundMusicId = 0x0010,
  kWramSoundEffectId = 0x0012,
  kWramTotalPopulation = 0x0218,
  kWramTownPopulation = 0x021C,
  kWramTownGrowth = 0x0228,
  kWramTownLevels = 0x022E,
  kWramTownItems = 0x023A,
  kWramMasterSp = 0x0282,
  kWramMasterHp = 0x0293,
  kWramNextLevelPopulation = 0x0297,
  kWramMasterLives = 0x02AB,
  kWramActionScores = 0x02B3,
  kWramCurrentTownIndex = 0x0341,
  kTownCount = 6,
  kActCount = 2,
};

static const char *const kTownKeys[kTownCount] = {
  "fillmore", "bloodpool", "kasandora", "aitos", "marahna", "northwall",
};

static void SetError(char *error, size_t capacity, const char *format, ...) {
  if (!error || !capacity) return;
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(error, capacity, format, arguments);
  va_end(arguments);
}

static bool Valid(const ActRaiserLocalizationValues *values) {
  return values && values->struct_size >= sizeof(*values) &&
      values->abi_version == ACTRAISER_LOCALIZATION_VALUES_ABI_VERSION &&
      values->wram && values->wram_bytes >= kActRaiserWramSize &&
      ArLanguagePack_GetMetadata(values->pack) &&
      (!values->fallback_pack || ArLanguagePack_GetMetadata(values->fallback_pack));
}

static uint8_t Read8(const ActRaiserLocalizationValues *values,
                     size_t address) {
  return address < values->wram_bytes ? values->wram[address] : 0;
}

static uint16_t Read16(const ActRaiserLocalizationValues *values,
                       size_t address) {
  return (uint16_t)(Read8(values, address) |
                    ((uint16_t)Read8(values, address + 1u) << 8));
}

static const ArLanguageMessage *ResolveAlias(
    const ArLanguagePack *pack, const ArLanguageMessage *message) {
  const ArLanguageMessage *cursor = message;
  for (uint32_t depth = 0; cursor && cursor->is_alias &&
                           depth <= ArLanguagePack_MessageCount(pack);
       ++depth) {
    cursor = ArLanguagePack_FindMessage(
        pack, ArLanguagePack_GetString(pack, cursor->alias));
  }
  return cursor && !cursor->is_alias ? cursor : NULL;
}

static bool CopyPlainTerm(const ArLanguagePack *pack, const char *semantic_id,
                          char *output, size_t capacity) {
  if (!pack || !semantic_id || !output || !capacity) return false;
  const ArLanguageMessage *message = ResolveAlias(
      pack, ArLanguagePack_FindMessage(pack, semantic_id));
  if (!message) return false;
  size_t used = 0;
  for (uint32_t index = 0; index < message->operation_count; ++index) {
    const ArLanguageOperation *operation =
        ArLanguagePack_GetOperation(pack, message, index);
    if (!operation) return false;
    const char *text = NULL;
    size_t bytes = 0;
    switch (operation->kind) {
      case kArLanguageOperation_Text:
        text = ArLanguagePack_GetString(pack, operation->value.text);
        bytes = operation->value.text.length;
        break;
      case kArLanguageOperation_LineBreak:
      case kArLanguageOperation_PreferredLineBreak:
      case kArLanguageOperation_ParagraphBreak:
      case kArLanguageOperation_PageBreak:
        text = " ";
        bytes = 1;
        break;
      case kArLanguageOperation_Empty:
        continue;
      case kArLanguageOperation_End:
        index = message->operation_count;
        continue;
      default:
        return false;
    }
    if (!text || bytes >= capacity - used) return false;
    if (bytes == 1 && text[0] == ' ' &&
        (!used || output[used - 1u] == ' '))
      continue;
    memcpy(output + used, text, bytes);
    used += bytes;
  }
  while (used && output[used - 1u] == ' ') --used;
  if (!used) return false;
  output[used] = 0;
  return true;
}

static bool SetText(ArDialogueValue *value, const char *text) {
  const size_t bytes = text ? strlen(text) : 0;
  if (!value || !bytes || bytes >= sizeof(value->text)) return false;
  memcpy(value->text, text, bytes + 1u);
  return true;
}

static bool SetNumber(ArDialogueValue *value, int64_t number) {
  if (!value) return false;
  value->number = number;
  return true;
}

static unsigned CurrentTown(const ActRaiserLocalizationValues *values) {
  unsigned town = Read8(values, kWramCurrentTownIndex);
  if (town >= 1u && town <= kTownCount) return town - 1u;
  town = Read8(values, kActRaiserWram_CurrentMap);
  return town >= 1u && town <= kTownCount ? town - 1u : 0u;
}

static bool CopyTermById(const ActRaiserLocalizationValues *values,
                         const char *semantic_id,
                         ArDialogueValue *value) {
  // Partial publications omit unchanged terms. Only absence falls back:
  // invalid/empty authored terms keep the existing fail-closed behavior.
  const ArLanguagePack *pack = values->pack;
  if (!ArLanguagePack_FindMessage(pack, semantic_id)) pack = values->fallback_pack;
  return CopyPlainTerm(pack, semantic_id,
                       value->text, sizeof(value->text));
}

static bool ResolveTownText(const ActRaiserLocalizationValues *values,
                            unsigned town, ArDialogueValue *value) {
  if (town >= kTownCount) return false;
  char semantic_id[64];
  const int written = snprintf(semantic_id, sizeof(semantic_id),
                               "city.%s.name", kTownKeys[town]);
  return written > 0 && (size_t)written < sizeof(semantic_id) &&
      CopyTermById(values, semantic_id, value);
}

static bool ResolveEnemyText(const ActRaiserLocalizationValues *values,
                             ArDialogueValue *value) {
  unsigned enemy = Read8(values, kWramLairEnemyIndex);
  if (enemy > 3u) enemy = 0;
  char semantic_id[64];
  const int written = snprintf(semantic_id, sizeof(semantic_id),
                               "enemy.name.slot_%02u", enemy);
  return written > 0 && (size_t)written < sizeof(semantic_id) &&
      CopyTermById(values, semantic_id, value);
}

static int FindTownField(const char *name, const char *suffix) {
  char key[96];
  for (unsigned town = 0; town < kTownCount; ++town) {
    const int written = snprintf(key, sizeof(key), "city_%s_%s",
                                 kTownKeys[town], suffix);
    if (written > 0 && (size_t)written < sizeof(key) && !strcmp(name, key))
      return (int)town;
  }
  return -1;
}

static int FindScore(const char *name) {
  char key[96];
  for (unsigned town = 0; town < kTownCount; ++town) {
    for (unsigned act = 0; act < kActCount; ++act) {
      const int written = snprintf(key, sizeof(key), "score_%s_act_%u",
                                   kTownKeys[town], act + 1u);
      if (written > 0 && (size_t)written < sizeof(key) && !strcmp(name, key))
        return (int)(town * kActCount + act);
    }
  }
  return -1;
}

static bool DecodeBcdScore(uint16_t bcd, uint32_t *numeric) {
  uint32_t score = 0;
  for (int shift = 12; shift >= 0; shift -= 4) {
    const unsigned digit = (bcd >> shift) & 0x0Fu;
    if (digit > 9u) return false;
    score = score * 10u + digit;
  }
  score *= 10u;
  *numeric = score;
  return true;
}

bool ActRaiserLocalizationValues_Capture(
    ActRaiserLocalizationValues *values,
    const uint8_t *wram, size_t wram_bytes,
    const ArLanguagePack *pack, const ArLanguagePack *fallback_pack,
    const char *master_name) {
  if (!values || !wram || wram_bytes < kActRaiserWramSize ||
      !ArLanguagePack_GetMetadata(pack) ||
      (fallback_pack && !ArLanguagePack_GetMetadata(fallback_pack)) ||
      !master_name || !master_name[0] ||
      strlen(master_name) >= sizeof(values->master_name))
    return false;
  ActRaiserLocalizationValues captured = {
      .struct_size = sizeof(captured),
      .abi_version = ACTRAISER_LOCALIZATION_VALUES_ABI_VERSION,
      .wram = wram,
      .wram_bytes = wram_bytes,
      .pack = pack,
      .fallback_pack = fallback_pack,
  };
  snprintf(captured.master_name, sizeof(captured.master_name), "%s",
           master_name);
  *values = captured;
  return true;
}

bool ActRaiserLocalizationValues_Resolve(
    void *context, const char *name,
    ArLanguagePlaceholderKind expected_kind,
    ArDialogueValue *value, char *error, size_t error_capacity) {
  if (error && error_capacity) error[0] = 0;
  const ActRaiserLocalizationValues *values =
      (const ActRaiserLocalizationValues *)context;
  if (!Valid(values) || !name || !value ||
      expected_kind == kArLanguagePlaceholder_Unknown) {
    SetError(error, error_capacity, "invalid live localization value request");
    return false;
  }
  memset(value, 0, sizeof(*value));
  value->kind = expected_kind;

  bool resolved = false;
  if (expected_kind == kArLanguagePlaceholder_Icon) {
    resolved = !strncmp(name, "icon.", 5) && SetText(value, name);
  } else if (!strcmp(name, "master_name")) {
    resolved = expected_kind == kArLanguagePlaceholder_LocalizedText &&
        SetText(value, values->master_name);
  } else if (!strcmp(name, "town_name") ||
             !strcmp(name, "current_city_name")) {
    resolved = expected_kind == kArLanguagePlaceholder_LocalizedText &&
        ResolveTownText(values, CurrentTown(values), value);
  } else if (!strcmp(name, "enemy_name")) {
    resolved = expected_kind == kArLanguagePlaceholder_LocalizedText &&
        ResolveEnemyText(values, value);
  } else if (!strcmp(name, "lair_count")) {
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kWramLairCount));
  } else if (!strcmp(name, "master_level")) {
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kActRaiserWram_TownLevel));
  } else if (!strcmp(name, "master_hp")) {
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kWramMasterHp));
  } else if (!strcmp(name, "master_sp")) {
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kWramMasterSp));
  } else if (!strcmp(name, "master_magic_points")) {
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values,
                                kActRaiserWram_PersistentMagicPoints));
  } else if (!strcmp(name, "master_lives_display")) {
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, (uint16_t)Read8(values, kWramMasterLives) + 1u);
  } else if (!strcmp(name, "next_level_population")) {
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kWramNextLevelPopulation));
  } else if (!strcmp(name, "total_population")) {
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kWramTotalPopulation));
  } else if (!strcmp(name, "sound_music_id")) {
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kWramSoundMusicId));
  } else if (!strcmp(name, "sound_effect_id")) {
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kWramSoundEffectId));
  } else if (!strcmp(name, "required_master_level")) {
    static const uint8_t kRequiredLevels[] = {1, 2, 4, 6, 8, 10};
    unsigned index = Read8(values, kWramLairEnemyIndex);
    if (index >= sizeof(kRequiredLevels)) index = sizeof(kRequiredLevels) - 1u;
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, kRequiredLevels[index]);
  }

  int town = FindTownField(name, "population");
  if (!resolved && town >= 0)
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kWramTownPopulation + town * 2u));
  town = FindTownField(name, "level");
  if (!resolved && town >= 0)
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kWramTownLevels + town * 2u));
  town = FindTownField(name, "items");
  if (!resolved && town >= 0)
    resolved = expected_kind == kArLanguagePlaceholder_Number &&
        SetNumber(value, Read16(values, kWramTownItems + town * 2u));
  town = FindTownField(name, "growth_state");
  if (!resolved && town >= 0 &&
      expected_kind == kArLanguagePlaceholder_LocalizedTerm) {
    unsigned growth = Read8(values, kWramTownGrowth + town);
    if (growth > 5u) growth = 5u;
    char term[64];
    const int written = snprintf(term, sizeof(term),
                                 "growth_state.%02u", growth);
    resolved = written > 0 && (size_t)written < sizeof(term) &&
        SetText(value, term);
  }

  const int score_index = FindScore(name);
  if (!resolved && score_index >= 0 &&
      expected_kind == kArLanguagePlaceholder_Number) {
    uint32_t score = 0;
    resolved = DecodeBcdScore(
        Read16(values, kWramActionScores + (size_t)score_index * 2u),
        &score) && SetNumber(value, score);
  }
  if (!resolved && !strcmp(name, "total_score") &&
      expected_kind == kArLanguagePlaceholder_Number) {
    uint32_t total = 0;
    resolved = true;
    for (unsigned index = 0; index < kTownCount * kActCount; ++index) {
      uint32_t score = 0;
      if (!DecodeBcdScore(
              Read16(values, kWramActionScores + index * 2u),
              &score)) {
        resolved = false;
        break;
      }
      total += score;
    }
    if (resolved) resolved = SetNumber(value, total);
  }

  if (!resolved) {
    SetError(error, error_capacity, "unsupported live value: %s", name);
    return false;
  }
  snprintf(value->name, sizeof(value->name), "%s", name);
  return true;
}

static uint64_t HashBytes(uint64_t hash, const void *data, size_t bytes) {
  const uint8_t *cursor = (const uint8_t *)data;
  for (size_t index = 0; index < bytes; ++index) {
    hash ^= cursor[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

uint64_t ActRaiserLocalizationValues_ReportRevision(
    const ActRaiserLocalizationValues *values) {
  if (!Valid(values)) return 0;
  uint64_t hash = UINT64_C(14695981039346656037);
  hash = HashBytes(hash, values->master_name, strlen(values->master_name));
  hash = HashBytes(hash, &values->pack->content_revision,
                   sizeof(values->pack->content_revision));
  if (values->fallback_pack)
    hash = HashBytes(hash, &values->fallback_pack->content_revision,
                    sizeof(values->fallback_pack->content_revision));
  static const struct { uint16_t address; uint16_t bytes; } kRanges[] = {
    {kWramTotalPopulation, 2}, {kWramTownPopulation, kTownCount * 2},
    {kWramTownGrowth, kTownCount}, {kWramTownLevels, kTownCount * 2},
    {kWramTownItems, kTownCount * 2}, {kWramMasterSp, 2},
    {kActRaiserWram_TownLevel, 2}, {kWramMasterHp, 2},
    {kActRaiserWram_PersistentMagicPoints, 2},
    {kWramNextLevelPopulation, 2}, {kWramMasterLives, 1},
    {kWramActionScores, kTownCount * kActCount * 2},
  };
  for (size_t index = 0; index < sizeof(kRanges) / sizeof(kRanges[0]); ++index)
    hash = HashBytes(hash, values->wram + kRanges[index].address,
                     kRanges[index].bytes);
  return hash ? hash : 1u;
}
