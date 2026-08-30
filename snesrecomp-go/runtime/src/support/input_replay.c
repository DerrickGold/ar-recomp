#include "snesrecomp/support/input_replay.h"

#include <string.h>

enum {
    kHeaderBytes = 128,
    kFrameBytes = 24,
    kCheckpointBytes = 96,
    kFooterBytes = 32,
    kWriterOpen = 1,
    kWriterFinished = 2,
    kReaderOpen = 1,
    kReaderFinished = 2,
};

static const uint8_t kMagic[8] = {'S', 'R', 'I', 'N', 'P', 'U', 'T', 0};
static const uint32_t kFrameTag = UINT32_C(0x4d415246); /* FRAM */
static const uint32_t kCheckpointTag = UINT32_C(0x4b484343); /* CCHK */
static const uint32_t kFooterTag = UINT32_C(0x21444e45); /* END! */

static void store_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void store_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void store_u64(uint8_t *bytes, uint64_t value) {
    unsigned index;
    for (index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
}

static uint16_t load_u16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t load_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t load_u64(const uint8_t *bytes) {
    uint64_t value = 0u;
    unsigned index;
    for (index = 0u; index < 8u; ++index)
        value |= (uint64_t)bytes[index] << (index * 8u);
    return value;
}

static int zero_bytes(const uint8_t *bytes, size_t count) {
    size_t index;
    for (index = 0u; index < count; ++index)
        if (bytes[index] != 0u) return 0;
    return 1;
}

static SrResult write_exact(SrInputReplayWriter *writer,
                            const uint8_t *bytes, uint32_t count) {
    SrResult result = writer->write(writer->user_data, bytes, count);
    return result == SR_RESULT_OK ? SR_RESULT_OK : result;
}

static SrResult read_exact(SrInputReplayReader *reader, uint8_t *bytes,
                           uint32_t count) {
    SrResult result = reader->read(reader->user_data, bytes, count);
    return result == SR_RESULT_UNAVAILABLE
        ? SR_RESULT_INVALID_ARGUMENT : result;
}

SrResult sr_input_replay_writer_begin(
        SrInputReplayWriter *writer, SrInputReplayWriteFunc *write,
        void *user_data, const SrInputReplayHeader *header) {
    uint8_t bytes[kHeaderBytes] = {0};
    const char *game_id_end;
    SrResult result;
    if (writer == NULL || write == NULL || header == NULL ||
        writer->struct_size < SR_INPUT_REPLAY_WRITER_V1_SIZE ||
        writer->state != 0u ||
        header->struct_size < SR_INPUT_REPLAY_HEADER_V1_SIZE ||
        (header->flags & ~SR_INPUT_REPLAY_HEADER_FLAGS_SUPPORTED) != 0u ||
        memchr(header->game_id, '\0', sizeof(header->game_id)) == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    game_id_end = (const char *)memchr(
        header->game_id, '\0', sizeof(header->game_id));
    memcpy(bytes, kMagic, sizeof(kMagic));
    store_u32(bytes + 8u, SR_INPUT_REPLAY_FORMAT_VERSION);
    store_u32(bytes + 12u, kHeaderBytes);
    store_u32(bytes + 16u, header->flags);
    store_u32(bytes + 20u, SR_INPUT_CONTROLLER_COUNT);
    store_u64(bytes + 24u, header->start_frame_ordinal);
    if ((header->flags & SR_INPUT_REPLAY_ROM_DIGEST_VALID) != 0u)
        memcpy(bytes + 32u, header->rom_sha256,
               SR_INPUT_REPLAY_SHA256_SIZE);
    if ((header->flags & SR_INPUT_REPLAY_INITIAL_STATE_DIGEST_VALID) != 0u)
        memcpy(bytes + 64u, header->initial_state_sha256,
               SR_INPUT_REPLAY_SHA256_SIZE);
    memcpy(bytes + 96u, header->game_id,
           (size_t)(game_id_end - header->game_id) + 1u);
    result = write(user_data, bytes, sizeof(bytes));
    if (result != SR_RESULT_OK) return result;
    writer->state = kWriterOpen;
    writer->write = write;
    writer->user_data = user_data;
    writer->start_frame_ordinal = header->start_frame_ordinal;
    writer->next_frame_ordinal = header->start_frame_ordinal;
    writer->frame_count = 0u;
    writer->last_frame_ordinal = 0u;
    return SR_RESULT_OK;
}

SrResult sr_input_replay_writer_append_frame(
        SrInputReplayWriter *writer, const SrInputReplayFrame *frame) {
    uint8_t bytes[kFrameBytes] = {0};
    if (writer == NULL || frame == NULL ||
        writer->struct_size < SR_INPUT_REPLAY_WRITER_V1_SIZE ||
        writer->state != kWriterOpen || writer->write == NULL ||
        writer->frame_count == UINT64_MAX ||
        (writer->frame_count != 0u &&
         writer->last_frame_ordinal == UINT64_MAX) ||
        frame->struct_size < SR_INPUT_REPLAY_FRAME_V1_SIZE ||
        frame->flags != 0u || frame->reserved != 0u ||
        (frame->packed_buttons[0] & UINT16_C(0xf000)) != 0u ||
        (frame->packed_buttons[1] & UINT16_C(0xf000)) != 0u ||
        frame->frame_ordinal != writer->next_frame_ordinal)
        return SR_RESULT_INVALID_ARGUMENT;
    store_u32(bytes, kFrameTag);
    store_u32(bytes + 4u, kFrameBytes);
    store_u64(bytes + 8u, frame->frame_ordinal);
    store_u16(bytes + 16u, frame->packed_buttons[0]);
    store_u16(bytes + 18u, frame->packed_buttons[1]);
    {
        SrResult result = write_exact(writer, bytes, sizeof(bytes));
        if (result != SR_RESULT_OK) return result;
    }
    writer->last_frame_ordinal = frame->frame_ordinal;
    ++writer->frame_count;
    writer->next_frame_ordinal = frame->frame_ordinal == UINT64_MAX
        ? 0u : frame->frame_ordinal + 1u;
    return SR_RESULT_OK;
}

SrResult sr_input_replay_writer_append_checkpoint(
        SrInputReplayWriter *writer,
        const SrInputReplayCheckpoint *checkpoint) {
    uint8_t bytes[kCheckpointBytes] = {0};
    if (writer == NULL || checkpoint == NULL ||
        writer->struct_size < SR_INPUT_REPLAY_WRITER_V1_SIZE ||
        writer->state != kWriterOpen || writer->write == NULL ||
        checkpoint->struct_size < SR_INPUT_REPLAY_CHECKPOINT_V1_SIZE ||
        (checkpoint->flags &
         ~SR_INPUT_REPLAY_CHECKPOINT_FLAGS_SUPPORTED) != 0u ||
        checkpoint->flags == 0u || writer->frame_count == 0u ||
        checkpoint->frame_ordinal != writer->last_frame_ordinal ||
        (((checkpoint->flags & SR_INPUT_REPLAY_CHECKPOINT_SEMANTIC_VALID) == 0u) !=
             (checkpoint->semantic_schema_version == 0u)) ||
        (((checkpoint->flags & SR_INPUT_REPLAY_CHECKPOINT_PRESENTATION_VALID) == 0u) !=
             (checkpoint->presentation_schema_version == 0u)))
        return SR_RESULT_INVALID_ARGUMENT;
    store_u32(bytes, kCheckpointTag);
    store_u32(bytes + 4u, kCheckpointBytes);
    store_u64(bytes + 8u, checkpoint->frame_ordinal);
    store_u32(bytes + 16u, checkpoint->flags);
    store_u32(bytes + 20u, checkpoint->semantic_schema_version);
    store_u32(bytes + 24u, checkpoint->presentation_schema_version);
    if ((checkpoint->flags &
         SR_INPUT_REPLAY_CHECKPOINT_SEMANTIC_VALID) != 0u)
        memcpy(bytes + 32u, checkpoint->semantic_sha256,
               SR_INPUT_REPLAY_SHA256_SIZE);
    if ((checkpoint->flags &
         SR_INPUT_REPLAY_CHECKPOINT_PRESENTATION_VALID) != 0u)
        memcpy(bytes + 64u, checkpoint->presentation_sha256,
               SR_INPUT_REPLAY_SHA256_SIZE);
    return write_exact(writer, bytes, sizeof(bytes));
}

SrResult sr_input_replay_writer_finish(SrInputReplayWriter *writer) {
    uint8_t bytes[kFooterBytes] = {0};
    SrResult result;
    if (writer == NULL ||
        writer->struct_size < SR_INPUT_REPLAY_WRITER_V1_SIZE ||
        writer->state != kWriterOpen || writer->write == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    store_u32(bytes, kFooterTag);
    store_u32(bytes + 4u, kFooterBytes);
    store_u64(bytes + 8u, writer->frame_count);
    store_u64(bytes + 16u, writer->last_frame_ordinal);
    result = write_exact(writer, bytes, sizeof(bytes));
    if (result == SR_RESULT_OK) writer->state = kWriterFinished;
    return result;
}

SrResult sr_input_replay_reader_begin(
        SrInputReplayReader *reader, SrInputReplayReadFunc *read,
        void *user_data, SrInputReplayHeader *out_header) {
    uint8_t bytes[kHeaderBytes];
    uint32_t flags;
    SrResult result;
    if (reader == NULL || read == NULL || out_header == NULL ||
        reader->struct_size < SR_INPUT_REPLAY_READER_V1_SIZE ||
        reader->state != 0u ||
        out_header->struct_size < SR_INPUT_REPLAY_HEADER_V1_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    reader->read = read;
    reader->user_data = user_data;
    result = read_exact(reader, bytes, sizeof(bytes));
    if (result != SR_RESULT_OK) return result;
    flags = load_u32(bytes + 16u);
    {
        const uint8_t *game_id_end =
            memchr(bytes + 96u, '\0', SR_INPUT_REPLAY_GAME_ID_SIZE);
        if (memcmp(bytes, kMagic, sizeof(kMagic)) != 0 ||
        load_u32(bytes + 8u) != SR_INPUT_REPLAY_FORMAT_VERSION ||
        load_u32(bytes + 12u) != kHeaderBytes ||
        (flags & ~SR_INPUT_REPLAY_HEADER_FLAGS_SUPPORTED) != 0u ||
        load_u32(bytes + 20u) != SR_INPUT_CONTROLLER_COUNT ||
        game_id_end == NULL ||
        !zero_bytes(game_id_end + 1u,
                    (size_t)(bytes + kHeaderBytes - game_id_end - 1u)) ||
        (((flags & SR_INPUT_REPLAY_ROM_DIGEST_VALID) == 0u) &&
         !zero_bytes(bytes + 32u, SR_INPUT_REPLAY_SHA256_SIZE)) ||
        (((flags & SR_INPUT_REPLAY_INITIAL_STATE_DIGEST_VALID) == 0u) &&
         !zero_bytes(bytes + 64u, SR_INPUT_REPLAY_SHA256_SIZE)))
            return SR_RESULT_INVALID_ARGUMENT;
    }
    memset(out_header, 0, SR_INPUT_REPLAY_HEADER_V1_SIZE);
    out_header->struct_size = SR_INPUT_REPLAY_HEADER_V1_SIZE;
    out_header->flags = flags;
    out_header->start_frame_ordinal = load_u64(bytes + 24u);
    memcpy(out_header->rom_sha256, bytes + 32u,
           SR_INPUT_REPLAY_SHA256_SIZE);
    memcpy(out_header->initial_state_sha256, bytes + 64u,
           SR_INPUT_REPLAY_SHA256_SIZE);
    memcpy(out_header->game_id, bytes + 96u, SR_INPUT_REPLAY_GAME_ID_SIZE);
    reader->state = kReaderOpen;
    reader->start_frame_ordinal = out_header->start_frame_ordinal;
    reader->next_frame_ordinal = out_header->start_frame_ordinal;
    reader->frame_count = 0u;
    reader->last_frame_ordinal = 0u;
    return SR_RESULT_OK;
}

SrResult sr_input_replay_reader_next(
        SrInputReplayReader *reader, SrInputReplayRecord *out_record) {
    uint8_t bytes[kCheckpointBytes] = {0};
    uint32_t tag;
    uint32_t size;
    SrResult result;
    if (reader == NULL || out_record == NULL ||
        reader->struct_size < SR_INPUT_REPLAY_READER_V1_SIZE ||
        out_record->struct_size < SR_INPUT_REPLAY_RECORD_V1_SIZE ||
        reader->read == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    if (reader->state == kReaderFinished) return SR_RESULT_UNAVAILABLE;
    if (reader->state != kReaderOpen) return SR_RESULT_INVALID_ARGUMENT;
    result = read_exact(reader, bytes, 8u);
    if (result != SR_RESULT_OK) return result;
    tag = load_u32(bytes);
    size = load_u32(bytes + 4u);
    if (size < 8u || size > sizeof(bytes)) return SR_RESULT_INVALID_ARGUMENT;
    result = read_exact(reader, bytes + 8u, size - 8u);
    if (result != SR_RESULT_OK) return result;
    memset(out_record, 0, SR_INPUT_REPLAY_RECORD_V1_SIZE);
    out_record->struct_size = SR_INPUT_REPLAY_RECORD_V1_SIZE;
    if (tag == kFrameTag && size == kFrameBytes) {
        uint64_t ordinal = load_u64(bytes + 8u);
        if (reader->frame_count == UINT64_MAX ||
            (reader->frame_count != 0u &&
             reader->last_frame_ordinal == UINT64_MAX) ||
            ordinal != reader->next_frame_ordinal ||
            load_u32(bytes + 20u) != 0u ||
            (load_u16(bytes + 16u) & UINT16_C(0xf000)) != 0u ||
            (load_u16(bytes + 18u) & UINT16_C(0xf000)) != 0u)
            return SR_RESULT_INVALID_ARGUMENT;
        out_record->type = SR_INPUT_REPLAY_RECORD_FRAME;
        out_record->frame.struct_size = SR_INPUT_REPLAY_FRAME_V1_SIZE;
        out_record->frame.frame_ordinal = ordinal;
        out_record->frame.packed_buttons[0] = load_u16(bytes + 16u);
        out_record->frame.packed_buttons[1] = load_u16(bytes + 18u);
        reader->last_frame_ordinal = ordinal;
        reader->next_frame_ordinal = ordinal == UINT64_MAX
            ? 0u : ordinal + 1u;
        ++reader->frame_count;
        return SR_RESULT_OK;
    }
    if (tag == kCheckpointTag && size == kCheckpointBytes) {
        uint32_t flags = load_u32(bytes + 16u);
        uint32_t semantic_schema = load_u32(bytes + 20u);
        uint32_t presentation_schema = load_u32(bytes + 24u);
        if (reader->frame_count == 0u ||
            load_u64(bytes + 8u) != reader->last_frame_ordinal ||
            (flags & ~SR_INPUT_REPLAY_CHECKPOINT_FLAGS_SUPPORTED) != 0u ||
            flags == 0u || load_u32(bytes + 28u) != 0u ||
            (((flags & SR_INPUT_REPLAY_CHECKPOINT_SEMANTIC_VALID) == 0u) !=
                 (semantic_schema == 0u)) ||
            (((flags & SR_INPUT_REPLAY_CHECKPOINT_PRESENTATION_VALID) == 0u) !=
                 (presentation_schema == 0u)) ||
            (((flags & SR_INPUT_REPLAY_CHECKPOINT_SEMANTIC_VALID) == 0u) &&
             !zero_bytes(bytes + 32u, SR_INPUT_REPLAY_SHA256_SIZE)) ||
            (((flags & SR_INPUT_REPLAY_CHECKPOINT_PRESENTATION_VALID) == 0u) &&
             !zero_bytes(bytes + 64u, SR_INPUT_REPLAY_SHA256_SIZE)))
            return SR_RESULT_INVALID_ARGUMENT;
        out_record->type = SR_INPUT_REPLAY_RECORD_CHECKPOINT;
        out_record->checkpoint.struct_size =
            SR_INPUT_REPLAY_CHECKPOINT_V1_SIZE;
        out_record->checkpoint.flags = flags;
        out_record->checkpoint.frame_ordinal = reader->last_frame_ordinal;
        out_record->checkpoint.semantic_schema_version = semantic_schema;
        out_record->checkpoint.presentation_schema_version =
            presentation_schema;
        memcpy(out_record->checkpoint.semantic_sha256, bytes + 32u,
               SR_INPUT_REPLAY_SHA256_SIZE);
        memcpy(out_record->checkpoint.presentation_sha256, bytes + 64u,
               SR_INPUT_REPLAY_SHA256_SIZE);
        return SR_RESULT_OK;
    }
    if (tag == kFooterTag && size == kFooterBytes) {
        uint64_t frame_count = load_u64(bytes + 8u);
        uint64_t last_frame = load_u64(bytes + 16u);
        if (frame_count != reader->frame_count ||
            (frame_count != 0u && last_frame != reader->last_frame_ordinal) ||
            (frame_count == 0u && last_frame != 0u) ||
            !zero_bytes(bytes + 24u, 8u))
            return SR_RESULT_INVALID_ARGUMENT;
        reader->state = kReaderFinished;
        return SR_RESULT_UNAVAILABLE;
    }
    return SR_RESULT_INVALID_ARGUMENT;
}
