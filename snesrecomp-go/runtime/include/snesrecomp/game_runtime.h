#ifndef SNESRECOMP_LINKED_GAME_RUNTIME_H
#define SNESRECOMP_LINKED_GAME_RUNTIME_H

#include "snesrecomp/runner.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTL_RDNMI_FORCE_NMI UINT32_C(0x00000001)
#define RTL_RDNMI_IN_NMI UINT32_C(0x00000002)
#define RTL_RDNMI_AVAILABLE UINT32_C(0x00000004)

/* Callback-lifetime hardware state for a game-specific $4210 override. The
 * runner retains ownership of its console layout and the callback returns -1
 * to delegate the read to the generic hardware model. */
typedef struct RtlRdnmiReadContext {
    uint32_t struct_size;
    uint32_t flags;
} RtlRdnmiReadContext;

#define RTL_RDNMI_READ_CONTEXT_V2_SIZE                                  \
    ((uint32_t)(offsetof(RtlRdnmiReadContext, flags) +                   \
                sizeof(((RtlRdnmiReadContext *)0)->flags)))

/* The linked game adapter uses these direct singleton calls on its once-per-
 * frame hot path. They preserve the same fixed semantics as the public runner
 * ABI without exposing Snes layout or repeating descriptor validation inside
 * the only in-process consumer. COMPLETE returns transition flags, or -1 when
 * no active runner is available. */
#define RTL_GAME_FRAME_DISPATCH_NMI_IF_ENABLED UINT32_C(0x00000001)
#define RTL_GAME_FRAME_NMI_ENTERED UINT32_C(0x00000001)
int RtlGameFrameBegin(void);
int RtlGameFrameComplete(uint32_t flags);

/* Packed direct view of the four PPU display-control bytes needed by linked
 * game policy at synchronous emulation safe points. External consumers use
 * SrPpuStateSnapshot; this accessor avoids a full snapshot and API-table
 * validation in existing per-write/per-animation hot seams. Zero is also the
 * safe no-runner value for every current consumer. */
#define RTL_GAME_PPU_DISPLAY_CONTROL(value) ((uint8_t)((value) & 0xffu))
#define RTL_GAME_PPU_BG_MODE_CONTROL(value)                         \
    ((uint8_t)(((value) >> 8) & 0xffu))
#define RTL_GAME_PPU_MAIN_SCREEN(value)                             \
    ((uint8_t)(((value) >> 16) & 0xffu))
#define RTL_GAME_PPU_SUB_SCREEN(value)                              \
    ((uint8_t)(((value) >> 24) & 0xffu))
uint32_t RtlGamePpuDisplayState(void);

/* Direct singleton form of the public frame-policy transaction for the linked
 * game. It avoids an API-table lookup and generation round trip while keeping
 * the same validated, all-or-nothing policy contract. */
SrResult RtlGameApplyPpuFramePolicy(const SrPpuFramePolicy *policy);

/* Opaque handle for the active linked runner. It is valid from the lifecycle
 * `runner_changed` notification until the matching NULL notification. Game
 * enhancement code should use this instead of retaining or including the
 * runner's concrete Snes layout. */
SrRunnerHandle *RtlGameRunner(void);

/* Low-overhead event instrumentation for generated/game glue. Query before
 * computing expensive diagnostic context; emission is synchronous and copies
 * the event before returning. */
bool RtlGameEventEnabled(SrEventMask event_mask);
void RtlGameEmitInterrupt(SrInterruptKind kind, uint32_t flags,
                          uint32_t pc24, uint16_t vector,
                          int32_t scanline, const char *label);

#ifdef __cplusplus
}
#endif

#endif
