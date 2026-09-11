#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/runtime.h"
#include "snes/snes.h"
#include "snes/saveload.h"
#include "runner_internal.h"
#include "runner_state_internal.h"
#include "snesrecomp/game/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const DispatchEntry g_dispatch_table[] = {{0}};
const unsigned g_dispatch_table_count = 0;
static unsigned failures, depth, unlocks;
static uint64_t locked_cycle, max_chunk;
static bool measure_chunks, consume_on_unlock;
void RtlApuLock(void) {
    if (depth++ == 0 && g_snes) locked_cycle = apu_cycle_count(g_snes->apu);
}
void RtlApuUnlock(void) {
    if (depth == 0) abort();
    if (--depth != 0) return;
    ++unlocks;
    if (measure_chunks) {
        uint64_t cycles = apu_cycle_count(g_snes->apu) - locked_cycle;
        if (cycles > max_chunk) max_chunk = cycles;
    }
    if (consume_on_unlock) {
        consume_on_unlock = false;
        /* Model a callback taking the lock between producer chunks. */
        for (unsigned i = 0; i < 9000; ++i) apu_cycle(g_snes->apu);
    }
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "audio timing %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void cadence(unsigned refresh, unsigned output_rate) {
    apu_reset(g_snes->apu);
    RtlSetAudioOutputRate((int)output_rate);
    uint64_t ticks = 0, rendered = 0;
    uint32_t highwater = 0;
    int16_t pcm[2048];
    /* Three minutes crosses the old ring saturation point. Presentation and
     * callback demand vary independently of the emulated NTSC tick clock. */
    for (uint64_t present = 1; present <= 180u * refresh; ++present) {
        const uint64_t due = present * 236250000u /
            ((uint64_t)refresh * 11u * 357366u);
        while (ticks < due) { RtlAdvanceApuTimeline(); ++ticks; }
        const uint32_t fill = g_snes->apu->dsp->sampleWrite -
                              g_snes->apu->dsp->sampleRead;
        if (fill > highwater) highwater = fill;
        const uint64_t wanted = present * output_rate / refresh;
        CHECK(wanted - rendered <= 1024u);
        RtlRenderAudio(pcm, (int)(wanted - rendered), 2);
        rendered = wanted;
    }
    CHECK(g_snes->apu->timelineTargetCycles ==
          ticks * UINT64_C(357366) * 5632u / 118125u);
    CHECK(highwater < 1100u);
    CHECK(g_snes->apu->dsp->sampleWrite - g_snes->apu->dsp->sampleRead < 550u);
    CHECK(g_snes->apu->sampleClock >= 180u * 32000u);
    CHECK(g_snes->apu->sampleClock <= 180u * 32000u + 550u);
}

typedef struct Saved { SaveLoadInfo base; size_t offset; uint8_t bytes[1024*1024]; } Saved;
static void transfer(SaveLoadInfo *info, void *data, size_t size) {
    Saved *saved = (Saved *)info;
    if (size > sizeof(saved->bytes) - saved->offset) { info->failed = true; return; }
    if (info->saving) memcpy(saved->bytes + saved->offset, data, size);
    else memcpy(data, saved->bytes + saved->offset, size);
    saved->offset += size;
}
static void continuation(void) {
    apu_reset(g_snes->apu);
    measure_chunks = consume_on_unlock = true;
    max_chunk = unlocks = 0;
    RtlAdvanceApuTimeline();
    measure_chunks = false;
    CHECK(max_chunk <= 256u && unlocks > 1u);
    CHECK(apu_cycle_count(g_snes->apu) == 17038u);
    Saved *saved = calloc(1, sizeof(*saved));
    CHECK(saved != NULL);
    if (!saved) return;
    saved->base = (SaveLoadInfo){.func=transfer, .saving=true, .portable=true, .format_version=14};
    apu_saveload(g_snes->apu, &saved->base);
    CHECK(!saved->base.failed);
    const size_t size = saved->offset;
    RtlAdvanceApuTimeline();
    const uint64_t target = g_snes->apu->timelineTargetCycles;
    const uint32_t remainder = g_snes->apu->timelineCycleRemainder;
    saved->offset = 0; saved->base.saving = false;
    apu_saveload(g_snes->apu, &saved->base);
    CHECK(!saved->base.failed && saved->offset == size);
    RtlAdvanceApuTimeline();
    CHECK(g_snes->apu->timelineTargetCycles == target && target == 34077u);
    CHECK(g_snes->apu->timelineCycleRemainder == remainder);
    free(saved);
}
int main(void) {
    uint8_t *wram = calloc(1, 0x20000);
    g_snes = snes_init(wram);
    if (!g_snes) return 1;
    continuation();
    cadence(60, 44100);
    cadence(90, 48000);
    cadence(120, 48000);
    snes_free(g_snes); g_snes = NULL; free(wram);
    return failures ? 1 : 0;
}
