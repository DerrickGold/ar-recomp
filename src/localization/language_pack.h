#ifndef AR_LOCALIZATION_LANGUAGE_PACK_H
#define AR_LOCALIZATION_LANGUAGE_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AR_LANGUAGE_PACK_IO_ABI_VERSION UINT32_C(1)
#define AR_LANGUAGE_PACK_FORMAT_VERSION UINT32_C(1)

enum {
  kArLanguagePackageIdCapacity = 97,
  kArLanguageLocaleCapacity = 33,
  kArLanguageNameCapacity = 193,
  kArLanguageAuthorCapacity = 193,
  kArLanguageLicenseCapacity = 129,
  kArLanguageFontPathCapacity = 512,
  kArLanguageMaximumFallbackFonts = 8,
  kArLanguagePackErrorCapacity = 512,
};

typedef enum ArLanguageDirection {
  kArLanguageDirection_Auto = 0,
  kArLanguageDirection_LeftToRight,
  kArLanguageDirection_RightToLeft,
} ArLanguageDirection;

typedef enum ArLanguagePackTarget {
  kArLanguagePackTarget_UsRuntime = 0,
  kArLanguagePackTarget_ReferenceOnly,
} ArLanguagePackTarget;

typedef enum ArLanguageSourceProfile {
  kArLanguageSourceProfile_Us = 0,
  kArLanguageSourceProfile_EuropeEnglish,
  kArLanguageSourceProfile_German,
  kArLanguageSourceProfile_French,
  kArLanguageSourceProfile_Japanese,
} ArLanguageSourceProfile;

typedef enum ArLanguagePackCoverage {
  kArLanguagePackCoverage_Partial = 0,
  kArLanguagePackCoverage_Complete,
} ArLanguagePackCoverage;

typedef enum ArLanguageOperationKind {
  kArLanguageOperation_Text = 0,
  kArLanguageOperation_Placeholder,
  kArLanguageOperation_LineBreak,
  kArLanguageOperation_PreferredLineBreak,
  kArLanguageOperation_ParagraphBreak,
  kArLanguageOperation_PageBreak,
  kArLanguageOperation_WaitFrames,
  kArLanguageOperation_Anchor,
  kArLanguageOperation_Event,
  kArLanguageOperation_Empty,
  kArLanguageOperation_End,
} ArLanguageOperationKind;

typedef struct ArLanguageString {
  uint32_t offset;
  uint32_t length;
} ArLanguageString;

typedef struct ArLanguageOperation {
  ArLanguageOperationKind kind;
  uint32_t source_line;
  /* Optional decimal formatting on a number placeholder: {value:03}.
   * A minimum width never truncates a larger number. Zero means unpadded. */
  uint8_t minimum_digits;
  union {
    ArLanguageString text;
    ArLanguageString placeholder;
    uint32_t wait_frames;
    ArLanguageString anchor;
    struct {
      ArLanguageString id;
      ArLanguageString arguments;
    } event;
  } value;
} ArLanguageOperation;

typedef struct ArLanguageMessage {
  ArLanguageString id;
  ArLanguageString alias;
  uint32_t first_operation;
  uint32_t operation_count;
  uint32_t source_line;
  /* Its own position in the owning pack, so ownership of a caller-supplied
   * message is a constant-time equality check rather than a scan. */
  uint32_t index;
  bool is_alias;
} ArLanguageMessage;

typedef struct ArLanguagePackMetadata {
  char package_id[kArLanguagePackageIdCapacity];
  char locale[kArLanguageLocaleCapacity];
  char display_name[kArLanguageNameCapacity];
  char autonym[kArLanguageNameCapacity];
  char author[kArLanguageAuthorCapacity];
  char license[kArLanguageLicenseCapacity];
  char fallback[kArLanguagePackageIdCapacity];
  ArLanguageDirection direction;
  ArLanguagePackTarget target;
  ArLanguageSourceProfile source_profile;
  ArLanguagePackCoverage coverage;
  char primary_font[kArLanguageFontPathCapacity];
  char fallback_fonts[kArLanguageMaximumFallbackFonts]
                     [kArLanguageFontPathCapacity];
  uint32_t fallback_font_count;
} ArLanguagePackMetadata;

typedef struct ArLanguagePackError {
  char message[kArLanguagePackErrorCapacity];
} ArLanguagePackError;

/* VFS blobs remain adapter-owned until release_file. The core copies all
 * retained data, so no filesystem/archive pointer escapes a load call. */
typedef struct ArLanguagePackBlob {
  size_t struct_size;
  const uint8_t *data;
  size_t size;
  uintptr_t token;
} ArLanguagePackBlob;

typedef struct ArLanguagePackIo {
  size_t struct_size;
  uint32_t abi_version;
  void *context;
  bool (*read_file)(void *context, const char *path, size_t maximum_bytes,
                    ArLanguagePackBlob *out_blob, char *error,
                    size_t error_capacity);
  void (*release_file)(void *context, ArLanguagePackBlob *blob);
} ArLanguagePackIo;

/* Public for stack allocation. Members below metadata are loader-owned; use
 * accessors instead of retaining their addresses across a successful reload. */
typedef struct ArLanguagePack {
  ArLanguagePackMetadata metadata;
  uint64_t content_revision;
  uint32_t message_count;
  uint32_t operation_count;
  ArLanguageMessage *messages;
  /* Open-addressed semantic-ID index, maintained while parsing. Slots hold
   * "message index + 1"; zero is empty. It keeps duplicate detection, message
   * lookup and alias resolution from rescanning the whole pack. */
  uint32_t *message_lookup;
  size_t message_lookup_capacity;
  ArLanguageOperation *operations;
  char *strings;
  size_t strings_size;
  size_t strings_capacity;
  size_t message_capacity;
  size_t operation_capacity;
  uint32_t private_magic;
} ArLanguagePack;

void ArLanguagePack_Init(ArLanguagePack *pack);
void ArLanguagePack_Destroy(ArLanguagePack *pack);

/* Transactional: failure leaves an already loaded pack unchanged. This layer
 * validates the container, UTF-8 grammar, resource limits, and alias graph.
 * The game-specific semantic-contract validator must approve a loaded pack
 * before it becomes selectable. */
bool ArLanguagePack_Load(ArLanguagePack *pack, const ArLanguagePackIo *io,
                         const char *manifest_path,
                         ArLanguagePackError *error);

/* Catalog/menu fast path. This reads only the manifest. */
bool ArLanguagePack_ReadMetadata(const ArLanguagePackIo *io,
                                 const char *manifest_path,
                                 ArLanguagePackMetadata *metadata,
                                 uint64_t *manifest_revision,
                                 ArLanguagePackError *error);

/* Standard-C adapter. Archive and console ports provide the same small ABI. */
void ArLanguagePackFileIo_Init(ArLanguagePackIo *io);

/* The one place that knows how a pack-internal member path is joined to the
 * host path of the manifest that referenced it, so the loader, the font
 * resolver and any port adapter cannot disagree about it.
 *
 * `manifest_path` is a host path in whatever form the host handed us: rooted,
 * relative, UNC, with spaces or non-ASCII bytes. Its directory prefix is taken
 * verbatim, so the member resolves next to the manifest rather than against the
 * process working directory. Only '/' separates directories on POSIX, where a
 * backslash is an ordinary filename byte; Windows additionally honours '\' and
 * a bare "C:" drive-relative prefix.
 *
 * `member` must be a portable pack-internal path (forward slashes, no drive,
 * no traversal); the manifest parser rejects anything else. Returns false and
 * leaves `result` unspecified when the joined path does not fit `capacity`. */
bool ArLanguagePack_ResolveMemberPath(const char *manifest_path,
                                      const char *member, char *result,
                                      size_t capacity);

const ArLanguagePackMetadata *ArLanguagePack_GetMetadata(
    const ArLanguagePack *pack);
uint32_t ArLanguagePack_MessageCount(const ArLanguagePack *pack);
const ArLanguageMessage *ArLanguagePack_GetMessage(
    const ArLanguagePack *pack, uint32_t index);
const ArLanguageMessage *ArLanguagePack_FindMessage(
    const ArLanguagePack *pack, const char *semantic_id);
const ArLanguageOperation *ArLanguagePack_GetOperation(
    const ArLanguagePack *pack, const ArLanguageMessage *message,
    uint32_t index);
const char *ArLanguagePack_GetString(const ArLanguagePack *pack,
                                     ArLanguageString string);

#endif /* AR_LOCALIZATION_LANGUAGE_PACK_H */
