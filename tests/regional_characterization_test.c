/* Byte-exact characterization of every consumer of the regional rule
 * inventory: the fingerprint, the session codec, the profile queries, the
 * settings readout and session validation. It pins today's behavior while the
 * parallel rule lists move onto one family table
 * (development/specs/ar-recomp-code-organization-cleanup.md, Phase 1).
 *
 * Inputs: the new-game rules, every rule slot set to Japan and to Europe on
 * the requested and on the effective side, every profile expansion, and
 * seeded random mixes. Each area's outputs are folded into one SHA-256 and
 * compared with the digest captured from the pre-refactor code. Run with
 * AR_CHARACTERIZE_PRINT=1 to print the current digests. */
#include "actraiser/regional/actraiser_regional_settings.h"
#include "regional/regional_profiles.h"
#include "regional/session/regional_fingerprint.h"
#include "regional/session/regional_session.h"
#include "regional/session/regional_session_internal.h"
#include "snesrecomp/support/digest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { \
    if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); ++failures; } \
  } while (0)

enum { kSlotCount = sizeof(ArRegionalRules) / sizeof(ArRegionalSource), kRandomPairs = 96 };
_Static_assert(sizeof(ArRegionalSource) == sizeof(int), "rule slots are int-sized enums");
_Static_assert(sizeof(ArRegionalRules) % sizeof(ArRegionalSource) == 0,
               "rules hold only enum slots");

typedef enum Area {
  kArea_Fingerprint, kArea_Codec, kArea_Profiles, kArea_Choices, kArea_Validity,
  kArea_Versions, kArea_Rejection, kArea_Count
} Area;
static const char *const kAreaNames[kArea_Count] = {
  "fingerprint", "codec", "profiles", "choices", "validity", "versions", "rejection",
};
/* Captured from the pre-refactor code (3de88ed; versions and rejection from
 * 1e59e5f, whose codec and fingerprint were still the originals). */
static const char *const kExpected[kArea_Count] = {
  "a5c41552de13575f63f5b9ff8192c4df2b95d089000a947d374629e93fab2496",
  "a25527ec4d96fab3f441346ad073a9fcaa6b68268bafb41a18dcd380ddfcd0d9",
  "a6960baf374a6508865def2190d041e21a03b700fdb9320779d832b654a74ca0",
  "a6f7839f041656472d4389bfaeeee200196a8351e577a538402e5504f7290a76",
  "559a81855e799621873ea9295cbd79719693a9da6c04606971c8073060e80369",
  "254ae4bb03da16cb9d7ed0bc4ed32ba3e348b3fcfe312a2a8ccf964e6c64b2a6",
  "834b357d7d39eea0e0c10d11b2219f6830900756c8044936f5374c9e9490f05a",
};

/* Each area accumulates raw output bytes, hashed once at the end. */
typedef struct Sink {
  uint8_t *bytes;
  size_t used, capacity;
} Sink;
static Sink s_sinks[kArea_Count];

static void Put(Area area, const void *data, size_t size) {
  Sink *sink = &s_sinks[area];
  if (sink->used + size > sink->capacity) {
    sink->capacity = (sink->used + size) * 2;
    sink->bytes = realloc(sink->bytes, sink->capacity);
    if (!sink->bytes) { fprintf(stderr, "out of memory\n"); exit(2); }
  }
  memcpy(sink->bytes + sink->used, data, size);
  sink->used += size;
}
static void PutU32(Area area, uint32_t value) {
  const uint8_t bytes[4] = {value, value >> 8, value >> 16, value >> 24};
  Put(area, bytes, sizeof(bytes));
}

static void SetSlot(ArRegionalRules *rules, unsigned slot, int value) {
  memcpy((uint8_t *)rules + slot * sizeof(int), &value, sizeof(value));
}

static uint32_t s_random = 0x9e3779b9u;
static uint32_t NextRandom(void) {
  s_random ^= s_random << 13;
  s_random ^= s_random >> 17;
  s_random ^= s_random << 5;
  return s_random;
}

/* A deterministic selector for ArRegionalProfiles_SelectSources: the choice
 * depends only on the key, group and current source it is asked about. */
static ArRegionalSource SelectByKey(void *context, const char *key,
                                    ArRegionalProfileGroup group, ArRegionalSource current) {
  uint32_t hash = 2166136261u ^ *(const uint32_t *)context;
  for (const char *p = key; *p; ++p) hash = (hash ^ (uint8_t)*p) * 16777619u;
  hash = (hash ^ (uint32_t)group) * 16777619u;
  hash = (hash ^ (uint32_t)current) * 16777619u;
  return (ArRegionalSource)(hash % kArRegionalSource_Count);
}

static void RecordPair(const ArRegionalSession *base, const ArRegionalRules *requested,
                       const ArRegionalRules *effective) {
  uint8_t digest[32] = {0};
  bool baseline = false;
  const bool fingerprinted = ArRegionalRules_Fingerprint(requested, effective, digest, &baseline);
  const uint8_t fingerprint_flags[2] = {fingerprinted, baseline};
  Put(kArea_Fingerprint, fingerprint_flags, sizeof(fingerprint_flags));
  if (fingerprinted) Put(kArea_Fingerprint, digest, sizeof(digest));

  ArRegionalSession session = *base;
  session.requested = *requested;
  session.effective = *effective;
  const uint8_t valid = ArRegionalSession_Valid(&session);
  Put(kArea_Validity, &valid, 1);

  static uint8_t encoded[kSaveCheckpointPayloadMax], reencoded[kSaveCheckpointPayloadMax];
  size_t size = 0, resize = 0;
  const uint8_t encoded_ok = ArRegionalSession_Encode(&session, encoded, sizeof(encoded), &size);
  Put(kArea_Codec, &encoded_ok, 1);
  if (encoded_ok) {
    PutU32(kArea_Codec, (uint32_t)size);
    Put(kArea_Codec, encoded, size);
    ArRegionalSession decoded;
    const SaveCheckpointStatus status = ArRegionalSession_Decode(encoded, size, &decoded);
    PutU32(kArea_Codec, (uint32_t)status);
    CHECK(status == kSaveCheckpoint_Ready);
    if (status == kSaveCheckpoint_Ready) {
      CHECK(ArRegionalSession_Encode(&decoded, reencoded, sizeof(reencoded), &resize));
      CHECK(resize == size && !memcmp(encoded, reencoded, size));
    }
  }

  ActRaiserRegionalChoiceView choices[kActRaiserRegionalSetting_Count];
  memset(choices, 0xa5, sizeof(choices));
  ActRaiserRegionalSettings_DescribeChoices(requested, effective, choices);
  for (unsigned i = 0; i < kActRaiserRegionalSetting_Count; ++i) {
    const uint8_t view[3] = {choices[i].source, choices[i].active_source, choices[i].pending};
    Put(kArea_Choices, view, sizeof(view));
  }

  ArRegionalProfileSummary summaries[kArRegionalProfile_Count];
  const uint8_t described = ArRegionalProfiles_Describe(requested, summaries);
  Put(kArea_Profiles, &described, 1);
  for (unsigned i = 0; described && i < kArRegionalProfile_Count; ++i) {
    const uint8_t summary[2] = {summaries[i].source, summaries[i].matching_sources};
    Put(kArea_Profiles, summary, sizeof(summary));
    PutU32(kArea_Profiles, summaries[i].members);
  }
  uint16_t groups = 0;
  const uint8_t changes = ArRegionalProfiles_Changes(requested, effective, &groups);
  Put(kArea_Profiles, &changes, 1);
  PutU32(kArea_Profiles, changes ? groups : 0);
  uint32_t salt = s_random;
  ArRegionalRules selected;
  memset(&selected, 0, sizeof(selected));
  const uint8_t selected_ok =
      ArRegionalProfiles_SelectSources(requested, SelectByKey, &salt, &selected);
  Put(kArea_Profiles, &selected_ok, 1);
  if (selected_ok) Put(kArea_Profiles, &selected, sizeof(selected));
}

/* Out-of-range sources, one slot at a time: only the consumers that validate
 * their input (fingerprint, session validation, encoder) are asked. */
static void RecordRejection(const ArRegionalSession *base, const ArRegionalRules *native) {
  for (unsigned slot = 0; slot < kSlotCount; ++slot) {
    for (unsigned side = 0; side < 2; ++side) {
      ArRegionalRules invalid = *native;
      SetSlot(&invalid, slot, kArRegionalSource_Count);
      const ArRegionalRules *requested = side ? native : &invalid;
      const ArRegionalRules *effective = side ? &invalid : native;
      uint8_t digest[32] = {0};
      bool baseline = false;
      const uint8_t fingerprinted =
          ArRegionalRules_Fingerprint(requested, effective, digest, &baseline);
      Put(kArea_Rejection, &fingerprinted, 1);
      if (fingerprinted) Put(kArea_Rejection, digest, sizeof(digest));
      ArRegionalSession session = *base;
      session.requested = *requested;
      session.effective = *effective;
      static uint8_t encoded[kSaveCheckpointPayloadMax];
      size_t size = 0;
      const uint8_t verdicts[2] = {
        ArRegionalSession_Valid(&session),
        ArRegionalSession_Encode(&session, encoded, sizeof(encoded), &size),
      };
      Put(kArea_Rejection, verdicts, sizeof(verdicts));
    }
  }
}

/* The record count each format version carries, as Decode enforces it: for a
 * header-only payload, the one count it does not reject as unsupported. */
static void RecordVersions(void) {
  static const uint8_t kMagics[2][8] = {
    {'A', 'R', 'R', 'E', 'G', 'I', 'O', 'N'}, {'A', 'R', 'P', 'R', 'I', 'C', 'E', 0},
  };
  for (unsigned magic = 0; magic < 2; ++magic) {
    for (unsigned version = 1; version <= 71; ++version) {
      unsigned accepted = 0, matches = 0;
      for (unsigned count = 0; count < 1024; ++count) {
        uint8_t header[36] = {0};
        memcpy(header, kMagics[magic], 8);
        header[8] = (uint8_t)version;
        header[10] = (uint8_t)count;
        header[11] = (uint8_t)(count >> 8);
        ArRegionalSession decoded;
        if (ArRegionalSession_Decode(header, sizeof(header), &decoded) !=
            kSaveCheckpoint_Unsupported) {
          accepted = count;
          ++matches;
        }
      }
      PutU32(kArea_Versions, magic);
      PutU32(kArea_Versions, version);
      PutU32(kArea_Versions, accepted);
      PutU32(kArea_Versions, matches);
    }
  }
}

static bool Matches(Area area, const char *hex) {
  uint8_t digest[32];
  char text[65];
  if (!sr_support_sha256(s_sinks[area].bytes, s_sinks[area].used, digest)) return false;
  for (unsigned i = 0; i < 32; ++i) snprintf(text + 2 * i, 3, "%02x", digest[i]);
  if (getenv("AR_CHARACTERIZE_PRINT"))
    printf("%-12s %s (%zu bytes)\n", kAreaNames[area], text, s_sinks[area].used);
  return !strcmp(text, hex);
}

int main(void) {
  const uint8_t campaign[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  ArRegionalSession base;
  if (!ArRegionalSession_NewGame(&base, 0, campaign, &defaults)) {
    fprintf(stderr, "new game failed\n");
    return 1;
  }
  const ArRegionalRules native = base.requested;
  unsigned pairs = 0;

  RecordPair(&base, &native, &native), ++pairs;
  for (unsigned slot = 0; slot < kSlotCount; ++slot) {
    for (int source = kArRegionalSource_Japan; source < kArRegionalSource_Count; ++source) {
      ArRegionalRules changed = native;
      SetSlot(&changed, slot, source);
      RecordPair(&base, &changed, &native), ++pairs;
      RecordPair(&base, &native, &changed), ++pairs;
    }
  }
  for (unsigned group = 0; group < kArRegionalProfile_Count; ++group) {
    for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
      ArRegionalRules expanded;
      memset(&expanded, 0, sizeof(expanded));
      const uint8_t ok = ArRegionalProfiles_Expand(&native, (ArRegionalProfileGroup)group,
                                                   (ArRegionalSource)source, &expanded);
      Put(kArea_Profiles, &ok, 1);
      if (!ok) continue;
      Put(kArea_Profiles, &expanded, sizeof(expanded));
      RecordPair(&base, &expanded, &native), ++pairs;
      RecordPair(&base, &native, &expanded), ++pairs;
      RecordPair(&base, &expanded, &expanded), ++pairs;
    }
  }
  for (unsigned i = 0; i < kRandomPairs; ++i) {
    ArRegionalRules requested = native, effective = native;
    for (unsigned slot = 0; slot < kSlotCount; ++slot) {
      SetSlot(&requested, slot, NextRandom() % kArRegionalSource_Count);
      SetSlot(&effective, slot, NextRandom() % kArRegionalSource_Count);
    }
    RecordPair(&base, &requested, &effective), ++pairs;
  }

  RecordVersions();
  RecordRejection(&base, &native);
  for (unsigned area = 0; area < kArea_Count; ++area)
    if (!Matches((Area)area, kExpected[area])) {
      fprintf(stderr, "%s outputs changed (set AR_CHARACTERIZE_PRINT=1 to see digests)\n",
              kAreaNames[area]);
      ++failures;
    }
  if (getenv("AR_CHARACTERIZE_PRINT")) printf("%u pairs over %u rule slots\n", pairs, kSlotCount);
  return failures ? 1 : 0;
}
