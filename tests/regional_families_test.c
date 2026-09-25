/* The family table must describe every ArRegionalRules field exactly once, so
 * adding a rule field without a family row fails here rather than silently
 * dropping out of the codec, fingerprint or profiles. */
#include "regional/regional_families.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { \
    if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); ++failures; } \
  } while (0)

static void CheckTiling(void) {
  /* Families follow the struct's declaration order and leave no gap. The
   * difficulty level is a choice, not a regional source, and is the one field
   * no family covers. */
  size_t expected = 0;
  for (unsigned id = 0; id < kArRegionalFamily_Count; ++id) {
    const ArRegionalFamily *family = ArRegionalFamilies_Get((ArRegionalFamilyId)id);
    CHECK(family && family->offset == expected);
    if (!family) return;
    expected = family->offset + family->slots * sizeof(ArRegionalSource);
    if (id == kArRegionalFamily_Difficulty) {
      CHECK(offsetof(ArRegionalRules, difficulty.level) == expected);
      expected += sizeof(ArRegionalDifficulty);
    }
  }
  CHECK(expected == sizeof(ArRegionalRules));
  CHECK(!ArRegionalFamilies_Get(kArRegionalFamily_Count));
}

static void CheckRules(void) {
  enum { kMaxKeys = 512 };
  const char *keys[kMaxKeys];
  unsigned key_count = 0;
  for (unsigned id = 0; id < kArRegionalFamily_Count; ++id) {
    const ArRegionalFamily *family = ArRegionalFamilies_Get((ArRegionalFamilyId)id);
    CHECK(family->name && family->name[0] && family->info);
    CHECK(family->slots >= 1);
    CHECK(family->rules == family->slots ||
          (id == kArRegionalFamily_LairSeeds && family->slots == 1));
    for (unsigned other = 0; other < id; ++other)
      CHECK(strcmp(family->name, ArRegionalFamilies_Get((ArRegionalFamilyId)other)->name));
    for (unsigned rule = 0; rule < family->rules; ++rule) {
      const ArRegionalRuleInfo info = family->info(rule);
      CHECK(info.key && info.key[0] && info.values);
      if (!info.key) continue;
      for (unsigned seen = 0; seen < key_count; ++seen)
        CHECK(strcmp(info.key, keys[seen]));
      CHECK(key_count < kMaxKeys);
      if (key_count < kMaxKeys) keys[key_count++] = info.key;
    }
  }
}

static void CheckFields(void) {
  for (unsigned id = 0; id < kArRegionalFamily_Count; ++id) {
    const ArRegionalFamily *family = ArRegionalFamilies_Get((ArRegionalFamilyId)id);
    for (unsigned rule = 0; rule < family->rules; ++rule) {
      ArRegionalRules rules;
      memset(&rules, 0, sizeof(rules));
      ArRegionalSource *field = ArRegionalFamilies_Field(&rules, (ArRegionalFamilyId)id, rule);
      CHECK((unsigned char *)field >= (unsigned char *)&rules &&
            (unsigned char *)(field + 1) <= (unsigned char *)(&rules + 1));
      *field = kArRegionalSource_Europe;
      CHECK(ArRegionalFamilies_Source(&rules, (ArRegionalFamilyId)id, rule) ==
            kArRegionalSource_Europe);
    }
  }
  /* Spot checks against the named fields. */
  ArRegionalRules rules;
  memset(&rules, 0, sizeof(rules));
  CHECK(ArRegionalFamilies_Field(&rules, kArRegionalFamily_Fishing, 0) == &rules.fishing);
  CHECK(ArRegionalFamilies_Field(&rules, kArRegionalFamily_Placements, 1) ==
        &rules.placements.pickups);
  CHECK(ArRegionalFamilies_Field(&rules, kArRegionalFamily_LairSeeds, 23) == &rules.lair_seeds);
  CHECK(ArRegionalFamilies_Field(&rules, kArRegionalFamily_Bosses, 30) ==
        &rules.bosses.source[30]);
}

static void CheckSourcesValid(void) {
  ArRegionalRules rules;
  memset(&rules, 0, sizeof(rules));
  CHECK(ArRegionalFamilies_SourcesValid(&rules));
  CHECK(!ArRegionalFamilies_SourcesValid(NULL));
  /* Every slot of every family is range-checked. */
  for (unsigned id = 0; id < kArRegionalFamily_Count; ++id) {
    const ArRegionalFamily *family = ArRegionalFamilies_Get((ArRegionalFamilyId)id);
    for (unsigned rule = 0; rule < family->rules; ++rule) {
      memset(&rules, 0, sizeof(rules));
      *ArRegionalFamilies_Field(&rules, (ArRegionalFamilyId)id, rule) = kArRegionalSource_Count;
      CHECK(!ArRegionalFamilies_SourcesValid(&rules));
    }
  }
  /* The difficulty level is a choice, not a source, and is not checked here. */
  memset(&rules, 0, sizeof(rules));
  rules.difficulty.level = (ArRegionalDifficulty)7;
  CHECK(ArRegionalFamilies_SourcesValid(&rules));
}

int main(void) {
  CheckTiling();
  CheckRules();
  CheckFields();
  CheckSourcesValid();
  return failures ? 1 : 0;
}
