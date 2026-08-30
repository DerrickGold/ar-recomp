#include "snesrecomp/runner/replay.h"
#include "../src/support/sha256.h"

#include <stdio.h>
#include <string.h>

typedef struct MemoryStream {
    uint8_t bytes[1024];
    size_t size;
    size_t offset;
} MemoryStream;

static int failures;

static void check(int condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "input replay failed: %s\n", message);
    ++failures;
}

static SrResult write_memory(void *user_data, const uint8_t *bytes,
                             uint32_t byte_count) {
    MemoryStream *stream = (MemoryStream *)user_data;
    if (stream == NULL || bytes == NULL ||
        stream->size + byte_count > sizeof(stream->bytes))
        return SR_RESULT_UNAVAILABLE;
    memcpy(stream->bytes + stream->size, bytes, byte_count);
    stream->size += byte_count;
    return SR_RESULT_OK;
}

static SrResult read_memory(void *user_data, uint8_t *bytes,
                            uint32_t byte_count) {
    MemoryStream *stream = (MemoryStream *)user_data;
    if (stream == NULL || bytes == NULL ||
        stream->offset + byte_count > stream->size)
        return SR_RESULT_UNAVAILABLE;
    memcpy(bytes, stream->bytes + stream->offset, byte_count);
    stream->offset += byte_count;
    return SR_RESULT_OK;
}

static SrResult read_busy(void *user_data, uint8_t *bytes,
                          uint32_t byte_count) {
    (void)user_data;
    (void)bytes;
    (void)byte_count;
    return SR_RESULT_BUSY;
}

static void fill_digest(uint8_t digest[32], uint8_t seed) {
    unsigned index;
    for (index = 0u; index < 32u; ++index)
        digest[index] = (uint8_t)(seed + index);
}

int main(void) {
    MemoryStream stream = {0};
    MemoryStream duplicate_checkpoint_stream = {0};
    SrInputReplayWriter writer = SR_INPUT_REPLAY_WRITER_INIT;
    SrInputReplayReader reader = SR_INPUT_REPLAY_READER_INIT;
    SrInputReplayHeader header = {
        .struct_size = sizeof(header),
        .flags = SR_INPUT_REPLAY_ROM_DIGEST_VALID |
                 SR_INPUT_REPLAY_INITIAL_STATE_DIGEST_VALID,
        .start_frame_ordinal = 40u,
        .game_id = "synthetic-game",
    };
    SrInputReplayFrame frame = {
        .struct_size = sizeof(frame),
        .frame_ordinal = 40u,
        .packed_buttons = {0x0123u, 0x0456u},
    };
    SrInputReplayCheckpoint checkpoint = {
        .struct_size = sizeof(checkpoint),
        .flags = SR_INPUT_REPLAY_CHECKPOINT_SEMANTIC_VALID |
                 SR_INPUT_REPLAY_CHECKPOINT_PRESENTATION_VALID,
        .frame_ordinal = 40u,
        .semantic_schema_version = 1u,
        .presentation_schema_version = 1u,
    };
    SrInputReplayHeader decoded = {
        .struct_size = sizeof(decoded),
    };
    SrInputReplayRecord record = {
        .struct_size = sizeof(record),
    };
    size_t full_size;
    SrResult truncated_result;

    fill_digest(header.rom_sha256, 1u);
    fill_digest(header.initial_state_sha256, 33u);
    fill_digest(checkpoint.semantic_sha256, 65u);
    fill_digest(checkpoint.presentation_sha256, 97u);
    check(sr_input_replay_writer_begin(
              &writer, write_memory, &stream, &header) == SR_RESULT_OK,
          "writer begin");
    check(sr_input_replay_writer_append_frame(&writer, &frame) ==
              SR_RESULT_OK,
          "first frame");
    frame.frame_ordinal = 42u;
    check(sr_input_replay_writer_append_frame(&writer, &frame) ==
              SR_RESULT_INVALID_ARGUMENT,
          "non-contiguous frame accepted");
    check(sr_input_replay_writer_append_checkpoint(&writer, &checkpoint) ==
              SR_RESULT_OK,
          "checkpoint");
    check(sr_input_replay_writer_append_checkpoint(&writer, &checkpoint) ==
              SR_RESULT_INVALID_ARGUMENT,
          "duplicate checkpoint accepted");
    frame.frame_ordinal = 41u;
    frame.packed_buttons[0] = 0x0789u;
    frame.packed_buttons[1] = 0x0abcu;
    check(sr_input_replay_writer_append_frame(&writer, &frame) ==
              SR_RESULT_OK,
          "second frame");
    check(sr_input_replay_writer_finish(&writer) == SR_RESULT_OK,
          "writer finish");
    full_size = stream.size;
    check(full_size == 304u &&
              memcmp(stream.bytes, "SRINPUT", 7u) == 0 &&
              stream.bytes[8] == SR_INPUT_REPLAY_FORMAT_VERSION &&
              stream.bytes[12] == 128u &&
              stream.bytes[128] == 'F' && stream.bytes[129] == 'R' &&
              stream.bytes[130] == 'A' && stream.bytes[131] == 'M',
          "canonical wire layout");
    {
        uint8_t wire_digest[32];
        static const uint8_t expected_wire_digest[32] = {
            0x43, 0xb9, 0x0e, 0x3b, 0x6d, 0x8d, 0xba, 0x67,
            0x3f, 0x7f, 0x1d, 0xe8, 0x31, 0xa7, 0x95, 0xb4,
            0x8b, 0xa3, 0x49, 0x55, 0xac, 0x38, 0xa1, 0x70,
            0x6c, 0x93, 0xc5, 0x23, 0x02, 0x7a, 0xee, 0x56,
        };
        sha256_compute(stream.bytes, stream.size, wire_digest);
        check(memcmp(wire_digest, expected_wire_digest,
                     sizeof(wire_digest)) == 0,
              "wire schema changed without a format-version bump");
    }

    check(sr_input_replay_reader_begin(
              &reader, read_memory, &stream, &decoded) == SR_RESULT_OK &&
              decoded.start_frame_ordinal == 40u &&
              strcmp(decoded.game_id, "synthetic-game") == 0 &&
              memcmp(decoded.rom_sha256, header.rom_sha256, 32u) == 0,
          "header round trip");
    check(sr_input_replay_reader_next(&reader, &record) == SR_RESULT_OK &&
              record.type == SR_INPUT_REPLAY_RECORD_FRAME &&
              record.frame.frame_ordinal == 40u &&
              record.frame.packed_buttons[0] == 0x0123u &&
              record.frame.packed_buttons[1] == 0x0456u,
          "first frame round trip");
    record.struct_size = sizeof(record);
    check(sr_input_replay_reader_next(&reader, &record) == SR_RESULT_OK &&
              record.type == SR_INPUT_REPLAY_RECORD_CHECKPOINT &&
              record.checkpoint.frame_ordinal == 40u &&
              record.checkpoint.semantic_schema_version == 1u &&
              memcmp(record.checkpoint.presentation_sha256,
                     checkpoint.presentation_sha256, 32u) == 0,
          "checkpoint round trip");
    record.struct_size = sizeof(record);
    check(sr_input_replay_reader_next(&reader, &record) == SR_RESULT_OK &&
              record.type == SR_INPUT_REPLAY_RECORD_FRAME &&
              record.frame.frame_ordinal == 41u &&
              record.frame.packed_buttons[0] == 0x0789u,
          "second frame round trip");
    record.struct_size = sizeof(record);
    check(sr_input_replay_reader_next(&reader, &record) ==
              SR_RESULT_UNAVAILABLE &&
              sr_input_replay_reader_next(&reader, &record) ==
                  SR_RESULT_UNAVAILABLE,
          "valid footer/end-of-stream");

    memcpy(duplicate_checkpoint_stream.bytes, stream.bytes, 248u);
    memcpy(duplicate_checkpoint_stream.bytes + 248u,
           stream.bytes + 152u, 96u);
    memcpy(duplicate_checkpoint_stream.bytes + 344u,
           stream.bytes + 248u, full_size - 248u);
    duplicate_checkpoint_stream.size = full_size + 96u;
    memset(&reader, 0, sizeof(reader));
    decoded.struct_size = sizeof(decoded);
    check(sr_input_replay_reader_begin(
              &reader, read_memory, &duplicate_checkpoint_stream,
              &decoded) == SR_RESULT_OK,
          "duplicate-checkpoint reader begin");
    record.struct_size = sizeof(record);
    check(sr_input_replay_reader_next(&reader, &record) == SR_RESULT_OK &&
              record.type == SR_INPUT_REPLAY_RECORD_FRAME,
          "duplicate-checkpoint first frame");
    record.struct_size = sizeof(record);
    check(sr_input_replay_reader_next(&reader, &record) == SR_RESULT_OK &&
              record.type == SR_INPUT_REPLAY_RECORD_CHECKPOINT,
          "duplicate-checkpoint first checkpoint");
    record.struct_size = sizeof(record);
    check(sr_input_replay_reader_next(&reader, &record) ==
                  SR_RESULT_INVALID_ARGUMENT &&
              sr_input_replay_reader_next(&reader, &record) ==
                  SR_RESULT_INVALID_ARGUMENT,
          "duplicate checkpoint record accepted");

    stream.offset = 0u;
    stream.size = full_size + 1u;
    stream.bytes[full_size] = 0xa5u;
    memset(&reader, 0, sizeof(reader));
    decoded.struct_size = sizeof(decoded);
    check(sr_input_replay_reader_begin(
              &reader, read_memory, &stream, &decoded) == SR_RESULT_OK,
          "trailing-byte reader begin");
    do {
        record.struct_size = sizeof(record);
        truncated_result = sr_input_replay_reader_next(&reader, &record);
    } while (truncated_result == SR_RESULT_OK);
    check(truncated_result == SR_RESULT_INVALID_ARGUMENT &&
              sr_input_replay_reader_next(&reader, &record) ==
                  SR_RESULT_INVALID_ARGUMENT,
          "trailing bytes accepted after footer");

    stream.offset = 0u;
    stream.size = full_size - 1u;
    memset(&reader, 0, sizeof(reader));
    decoded.struct_size = sizeof(decoded);
    check(sr_input_replay_reader_begin(
              &reader, read_memory, &stream, &decoded) == SR_RESULT_OK,
          "truncated reader begin");
    do {
        record.struct_size = sizeof(record);
        truncated_result = sr_input_replay_reader_next(&reader, &record);
    } while (truncated_result == SR_RESULT_OK);
    check(truncated_result == SR_RESULT_INVALID_ARGUMENT &&
              sr_input_replay_reader_next(&reader, &record) ==
                  SR_RESULT_INVALID_ARGUMENT,
          "truncated artifact mistaken for a completed replay");

    memset(&reader, 0, sizeof(reader));
    decoded.struct_size = sizeof(decoded);
    check(sr_input_replay_reader_begin(
              &reader, read_busy, NULL, &decoded) == SR_RESULT_BUSY,
          "reader discarded a non-EOF transport result");

    memset(&stream, 0, sizeof(stream));
    memset(&writer, 0, sizeof(writer));
    header.start_frame_ordinal = UINT64_MAX;
    check(sr_input_replay_writer_begin(
              &writer, write_memory, &stream, &header) == SR_RESULT_OK,
          "maximum-ordinal writer begin");
    frame.frame_ordinal = UINT64_MAX;
    frame.packed_buttons[0] = 0u;
    frame.packed_buttons[1] = 0u;
    check(sr_input_replay_writer_append_frame(&writer, &frame) ==
                  SR_RESULT_OK &&
              sr_input_replay_writer_append_frame(&writer, &frame) ==
                  SR_RESULT_INVALID_ARGUMENT &&
              sr_input_replay_writer_finish(&writer) == SR_RESULT_OK,
          "frame ordinal wrapped instead of terminating its sequence");

    if (failures != 0) return 1;
    puts("input replay: PASS");
    return 0;
}
