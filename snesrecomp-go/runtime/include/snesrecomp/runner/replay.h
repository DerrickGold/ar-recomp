/**
 * @file replay.h
 * @brief Canonical, transport-neutral runner input replay artifacts.
 */
#pragma once

#include "snesrecomp/runner/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stable eight-byte artifact signature, including the terminating NUL. It is
 * public so adapters supporting a historical format can select the canonical
 * parser without duplicating runner-private wire constants. */
#define SR_INPUT_REPLAY_FORMAT_VERSION 1u
#define SR_INPUT_REPLAY_MAGIC "SRINPUT"
#define SR_INPUT_REPLAY_MAGIC_SIZE 8u
#define SR_INPUT_REPLAY_SHA256_SIZE 32u
#define SR_INPUT_REPLAY_GAME_ID_SIZE 32u

#define SR_INPUT_REPLAY_ROM_DIGEST_VALID UINT32_C(0x00000001)
#define SR_INPUT_REPLAY_INITIAL_STATE_DIGEST_VALID UINT32_C(0x00000002)
#define SR_INPUT_REPLAY_HEADER_FLAGS_SUPPORTED                           \
    (SR_INPUT_REPLAY_ROM_DIGEST_VALID |                                  \
     SR_INPUT_REPLAY_INITIAL_STATE_DIGEST_VALID)

#define SR_INPUT_REPLAY_CHECKPOINT_SEMANTIC_VALID UINT32_C(0x00000001)
#define SR_INPUT_REPLAY_CHECKPOINT_PRESENTATION_VALID UINT32_C(0x00000002)
#define SR_INPUT_REPLAY_CHECKPOINT_FLAGS_SUPPORTED                       \
    (SR_INPUT_REPLAY_CHECKPOINT_SEMANTIC_VALID |                         \
     SR_INPUT_REPLAY_CHECKPOINT_PRESENTATION_VALID)

/** Exact-write callback. `OK` means all `byte_count` bytes were consumed. */
typedef SrResult SrInputReplayWriteFunc(
    void *user_data, const uint8_t *bytes, uint32_t byte_count);
/** Exact-read callback. `OK` means all bytes were produced; `UNAVAILABLE`
 * means physical EOF with no bytes consumed. Other results retain their usual
 * meaning. A non-OK callback must not partially consume the request. */
typedef SrResult SrInputReplayReadFunc(
    void *user_data, uint8_t *bytes, uint32_t byte_count);

/** Metadata written once at the start of an input artifact. `game_id` must
 * contain a NUL-terminated identifier. Hash validity is explicit because an
 * all-zero SHA-256 value is data, not an absence marker. */
typedef struct SrInputReplayHeader {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t start_frame_ordinal;
    uint8_t rom_sha256[SR_INPUT_REPLAY_SHA256_SIZE];
    uint8_t initial_state_sha256[SR_INPUT_REPLAY_SHA256_SIZE];
    char game_id[SR_INPUT_REPLAY_GAME_ID_SIZE];
} SrInputReplayHeader;

#define SR_INPUT_REPLAY_HEADER_V1_SIZE                                  \
    ((uint32_t)(offsetof(SrInputReplayHeader, game_id) +                 \
                sizeof(((SrInputReplayHeader *)0)->game_id)))

typedef struct SrInputReplayFrame {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t frame_ordinal;
    uint16_t packed_buttons[SR_INPUT_CONTROLLER_COUNT];
    uint32_t reserved;
} SrInputReplayFrame;

#define SR_INPUT_REPLAY_FRAME_V1_SIZE                                   \
    ((uint32_t)(offsetof(SrInputReplayFrame, reserved) +                 \
                sizeof(((SrInputReplayFrame *)0)->reserved)))

/** Optional, unique checkpoint paired with the most recently written frame.
 * Schema versions are defined by the digest producers, not this container. */
typedef struct SrInputReplayCheckpoint {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t frame_ordinal;
    uint32_t semantic_schema_version;
    uint32_t presentation_schema_version;
    uint8_t semantic_sha256[SR_INPUT_REPLAY_SHA256_SIZE];
    uint8_t presentation_sha256[SR_INPUT_REPLAY_SHA256_SIZE];
} SrInputReplayCheckpoint;

#define SR_INPUT_REPLAY_CHECKPOINT_V1_SIZE                              \
    ((uint32_t)(offsetof(SrInputReplayCheckpoint, presentation_sha256) + \
                sizeof(((SrInputReplayCheckpoint *)0)                   \
                           ->presentation_sha256)))

typedef uint32_t SrInputReplayRecordType;
enum {
    SR_INPUT_REPLAY_RECORD_FRAME = 1u,
    SR_INPUT_REPLAY_RECORD_CHECKPOINT = 2u
};

typedef struct SrInputReplayRecord {
    uint32_t struct_size;
    SrInputReplayRecordType type;
    SrInputReplayFrame frame;
    SrInputReplayCheckpoint checkpoint;
} SrInputReplayRecord;

#define SR_INPUT_REPLAY_RECORD_V1_SIZE                                  \
    ((uint32_t)(offsetof(SrInputReplayRecord, checkpoint) +              \
                sizeof(((SrInputReplayRecord *)0)->checkpoint)))

/** Opaque caller-owned streaming contexts. Zero-initialize before `begin` and
 * do not inspect or modify storage while an operation is open. The fixed
 * storage extent is ABI, but its contents are not. */
typedef struct SrInputReplayWriter {
    uint64_t storage[8];
} SrInputReplayWriter;

typedef struct SrInputReplayReader {
    uint64_t storage[8];
} SrInputReplayReader;

#define SR_INPUT_REPLAY_WRITER_INIT {{0u}}
#define SR_INPUT_REPLAY_READER_INIT {{0u}}

SrResult sr_input_replay_writer_begin(
    SrInputReplayWriter *writer, SrInputReplayWriteFunc *write,
    void *user_data, const SrInputReplayHeader *header);
SrResult sr_input_replay_writer_append_frame(
    SrInputReplayWriter *writer, const SrInputReplayFrame *frame);
SrResult sr_input_replay_writer_append_checkpoint(
    SrInputReplayWriter *writer,
    const SrInputReplayCheckpoint *checkpoint);
SrResult sr_input_replay_writer_finish(SrInputReplayWriter *writer);

SrResult sr_input_replay_reader_begin(
    SrInputReplayReader *reader, SrInputReplayReadFunc *read,
    void *user_data, SrInputReplayHeader *out_header);
/** Returns `UNAVAILABLE` only after a valid footer and physical EOF. Early EOF,
 * trailing bytes, duplicate checkpoints, and malformed records permanently
 * fail the reader with `INVALID_ARGUMENT`. */
SrResult sr_input_replay_reader_next(
    SrInputReplayReader *reader, SrInputReplayRecord *out_record);

#ifdef __cplusplus
}
#endif
