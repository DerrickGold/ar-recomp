#include "runner_internal.h"

#include "snesrecomp/game/apu_sync.h"
#include "snes/semantic_state.h"
#include "snes/snes.h"
#include "support/sha256.h"

#include <string.h>

static bool sha_write(void *context, const uint8_t *bytes,
                      size_t byte_count) {
    Sha256Context *sha = (Sha256Context *)context;
    if (sha == NULL || (bytes == NULL && byte_count != 0u)) return false;
    sha256_update(sha, bytes, byte_count);
    return true;
}

static void write_cpu_v2(SnesSemanticWriter *writer,
                         const SrCpuStateSnapshot *cpu) {
    snes_semantic_write_bytes(writer, "cpu2", 4u);
    snes_semantic_write_u32(writer, cpu->flags);
    snes_semantic_write_u64(writer, cpu->frame_counter);
    snes_semantic_write_u32(writer, cpu->execution_pc24);
    snes_semantic_write_u16(writer, cpu->a);
    snes_semantic_write_u16(writer, cpu->x);
    snes_semantic_write_u16(writer, cpu->y);
    snes_semantic_write_u16(writer, cpu->s);
    snes_semantic_write_u16(writer, cpu->d);
    snes_semantic_write_u8(writer, cpu->db);
    snes_semantic_write_u8(writer, cpu->pb);
    snes_semantic_write_u8(writer, cpu->p);
}

SrResult sr_runner_query_semantic_digest(
        SrRunnerHandle *runner, const SrSemanticDigestRequest *request,
        SrSemanticDigestResult *out_result) {
    static const uint8_t domain[] = "snesrecomp-semantic-state-v2";
    Snes *snes = (Snes *)(void *)runner;
    SrCpuStateSnapshot cpu = {
        .struct_size = sizeof(cpu),
    };
    SnesSemanticWriter writer;
    Sha256Context sha;
    SrResult result;
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_SEMANTIC_DIGEST_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_SEMANTIC_DIGEST_RESULT_V2_SIZE ||
        request->flags != 0u || request->reserved != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_SEMANTIC_DIGEST_RESULT_V2_SIZE);
    out_result->struct_size = SR_SEMANTIC_DIGEST_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    out_result->frame_counter = snes->abiFrameCounter;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (sr_runner_audio_query_forbidden()) return SR_RESULT_BUSY;

    /* Game-owned state providers must never run under a runner-owned lock. */
    result = sr_runner_query_cpu_state_snapshot(snes, &cpu);
    if (result != SR_RESULT_OK) return result;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;

    sha256_init(&sha);
    writer.write = sha_write;
    writer.context = &sha;
    writer.failed = false;
    snes_semantic_write_bytes(&writer, domain, sizeof(domain) - 1u);
    snes_semantic_write_u32(
        &writer, SR_DETERMINISM_SEMANTIC_SCHEMA_VERSION);
    snes_semantic_write_u64(&writer, snes->abiFrameCounter);
    write_cpu_v2(&writer, &cpu);
    snes_semantic_write_u16(&writer, snes->input1_currentState);
    snes_semantic_write_u16(&writer, snes->input2_currentState);

    (void)snes_write_semantic_main_state_v2(snes, &writer);
    RtlApuLock();
    (void)snes_write_semantic_apu_state_v2(snes, &writer);
    RtlApuUnlock();
    if (writer.failed) return SR_RESULT_UNAVAILABLE;
    sha256_final(&sha, out_result->sha256);
    out_result->schema_version = SR_DETERMINISM_SEMANTIC_SCHEMA_VERSION;
    return SR_RESULT_OK;
}
