#ifndef AR_ACTRAISER_ENHANCEMENTS_INTERNAL_H
#define AR_ACTRAISER_ENHANCEMENTS_INTERNAL_H
/* ActRaiser enhancements internals: what the enhancement passes in this folder
 * share: the callback-scoped PPU frame access and its helpers, the frame-plan
 * accessors, and the entry points the frame draw calls in each pass. Only files
 * in actraiser/enhancements/ include it (tools/check_private_headers.py).
 * Phase: game (frame transaction). */
#include "actraiser/actraiser_rtl_internal.h"

typedef struct ActRaiserPpuFrameAccess {
  const SrPpuFrameTransactionContext *context;
  SrPpuOverlayCaptureState captures[SR_PPU_OVERLAY_SOURCE_COUNT];
} ActRaiserPpuFrameAccess;

/* ---- defined in actraiser_frame_plan.c ---- */
void ActRaiser_CommitFramePlan(int left, int right, int top, int bottom);
const ActionBgPlan *ActRaiser_PendingActionBgPlan(void);
void ActRaiser_ClearWidescreenMarginGaps(
    bool bounded_world_margins,
    const SrPpuFrameTransactionContext *context);
const DioramaRoomOverride *ActRaiser_CurrentVirtualLayerRoom(void);
void ActRaiser_ApplyWidescreenPolicy(void);

/* ---- defined in actraiser_ppu_frame_access.c ---- */
void ActRaiser_BeginPpuFrameAccess(ActRaiserPpuFrameAccess *access);
void ActRaiser_EndPpuFrameAccess(void);
bool ActRaiser_ClaimOverlayCapture(
    uint32_t source, int x, int y, int width, int height, uint32_t flags);
SrPpuOverlayCaptureState ActRaiser_OverlayCaptureState(
    const SrPpuOverlayState *overlay);
bool ActRaiser_ExchangeOverlayCapture(
    uint32_t source, uint64_t generation,
    const SrPpuOverlayCaptureState *expected,
    const SrPpuOverlayCaptureState *replacement);
const SrPpuFrameTransactionContext *ActRaiser_PpuFrame(void);
const SrPpuOverlayCaptureState *ActRaiser_PpuCapture(
    uint32_t source);
bool ActRaiser_SetPpuOverlayCapture(
    uint32_t source, int x, int y, int width, int height, uint32_t flags);
bool ActRaiser_SetPpuOverlayFill(
    uint32_t source, SrPpuTransparentFillMode mode, uint8_t cgram_index);
bool ActRaiser_SetPpuOverlayOamRange(
    uint8_t first, uint8_t count);
bool ActRaiser_BindPpuOutput(
    SrPpuOutputKind kind, uint32_t source, uint32_t band,
    uint8_t *pixels, size_t pitch, uint32_t height);
bool ActRaiser_ResolvePpuObjRange(
    uint8_t first, uint8_t count, uint8_t priority,
    SrPpuObjResolveResult *result);
bool ActRaiser_RasterizePpuObjRange(
    uint8_t first, uint8_t count, uint8_t priority,
    uint32_t *pixels, size_t pitch, size_t byte_size,
    SrPpuObjRasterResult *result);
bool ActRaiser_RasterizePpuObjParts(
    const SrPpuObjPart *parts, size_t part_count,
    int x0, int y0, int x1, int y1,
    uint32_t *pixels, size_t pitch, size_t byte_size);
bool ActRaiser_ConfigurePpuObjCapture(
    const SrPpuObjCaptureRequest *request);

/* ---- defined in actraiser_hud_icon_promotion.c ---- */
void ActRaiser_HudIconBeginFrame(void);
void ActRaiser_HudIconPrepare(void);
void ActRaiser_HudIconComplete(SrResult status, const SrPpuScanoutResult *result);

/* ---- defined in actraiser_death_heim_hub.c ---- */
void ActRaiser_DioramaDeathHeimEyesPrepare(void);
void ActRaiser_DioramaDeathHeimHubStatuesFinish(int width);

/* ---- defined in actraiser_diorama_capture.c ---- */
bool ActRaiser_DioramaBoundsTrackingActive(void);
void ActRaiser_DioramaSampleBg2Bounds(const SrPpuStateSnapshot *ppu, int screen_y);
void ActRaiser_DioramaApronFinish(const ActionApronGeometry *geom);
void ActRaiser_PrepareDioramaCapture(const SrPpuStateSnapshot *ppu);
void ActRaiser_PrepareSceneMasks(uint8_t map_group, uint8_t map_number);
void ActRaiser_PrepareTownCapture(void);
SrPpuBackgroundViewRequest
ActRaiser_PrepareSkyboxView(const SrPpuFrameTransactionContext *context,
                            const SnesRunnerApi *scanout_api,
                            bool scanout_ready, bool profile_diorama);

/* ---- defined in actraiser_frame_draw.c ---- */
void ActRaiser_ReportSim3DCaptureContractFailure(void);

#endif  /* AR_ACTRAISER_ENHANCEMENTS_INTERNAL_H */
