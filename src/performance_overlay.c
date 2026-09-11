#include "performance_overlay.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "settings_overlay_render.h"

enum { kColumnChars = 32, kColumnGap = 2, kGlyphPixels = 8 };
static struct {
  PerformanceOverlayModel model;
  ArRenderTexture texture;
  uint64_t revision;
  int level, width, height, texture_width, texture_height;
  bool ready, initialized, texture_ready, texture_unavailable;
} s_cache;

void PerformanceOverlay_Reset(ArRenderDevice *device) {
  if (!s_cache.initialized) return;
  ArRenderDevice_DestroyTexture(device, s_cache.texture);
  memset(&s_cache, 0, sizeof(s_cache));
}

static void Line(PerformanceOverlayModel *model, int column, int row,
                 const char *format, ...) {
  if (model->line_count == kPerformanceOverlay_MaxLines) return;
  const int cell = kGlyphPixels * model->scale;
  const int y = model->panel.y + cell + row * (cell + model->scale * 2);
  if (y + cell > model->panel.y + model->panel.h - cell) return;
  PerformanceOverlayLine *line = &model->lines[model->line_count++];
  line->x = model->panel.x + cell + column * (kColumnChars + kColumnGap) * cell;
  line->y = y;
  va_list args;
  va_start(args, format);
  vsnprintf(line->text, sizeof(line->text), format, args);
  va_end(args);
  int available = (model->panel.x + model->panel.w - cell - line->x) / cell;
  if (column == 0 && available > kColumnChars && row >= 5)
    available = kColumnChars;
  if (available < 0) available = 0;
  if (available < (int)sizeof(line->text)) line->text[available] = '\0';
}

static void Stage(PerformanceOverlayModel *model, const PerformanceSnapshot *snapshot,
                  int column, int row, PerformanceStage stage) {
  const PerformanceValue *value = &snapshot->stages[stage];
  Line(model, column, row, "%-18.18s %6.2f %6.2f",
      PerformanceMetrics_StageName(stage), value->mean_ms, value->maximum_ms);
}

void PerformanceOverlay_Build(const PerformanceSnapshot *snapshot, int level,
    ArRenderExtentI output, PerformanceOverlayModel *model) {
  if (!model) return;
  *model = (PerformanceOverlayModel){0};
  if (!snapshot || level <= 0 || output.width < 240 || output.height < 120) return;
  model->scale = output.width >= 1120 && output.height >= 720 ? 2 : 1;
  const int cell = kGlyphPixels * model->scale;
  const int margin = cell / 2;
  const bool menu = snapshot->context.host_mode == 2;
  const bool detailed = level >= 2 && !menu && output.width >= 560 && output.height >= 390;
  const int rows = detailed ? 35 : menu ? 5 : 10;
  int width = (detailed ? 68 : 56) * cell;
  int height = rows * (cell + model->scale * 2) + cell * 2;
  if (width > output.width - margin * 2) width = output.width - margin * 2;
  if (height > output.height - margin * 2) height = output.height - margin * 2;
  model->panel = (ArRenderRectI){margin,
      menu ? output.height - height - margin : margin, width, height};
  static const char *const modes[] = {"game", "paused", "settings", "A/B"};
  const PerformanceContext *context = &snapshot->context;
  const char *host = context->host_mode >= 0 && context->host_mode < 4
      ? modes[context->host_mode] : "unknown";
  Line(model, 0, 0, "PERFORMANCE | %s / %s | %02X:%02X",
      PerformanceMetrics_SceneName(context->scene), host, context->map_group, context->map_number);
  if (!snapshot->ready) {
    Line(model, 0, 1, "Collecting a fresh 1-second sample...");
    Line(model, 0, 2, "GPU execution time unavailable; full details in run log.");
    return;
  }
  Line(model, 0, 1, "%.1f FPS  %.2f ms  p95 %.2f  max %.2f",
      snapshot->fps, snapshot->frame_mean_ms, snapshot->frame_p95_ms, snapshot->frame_max_ms);
  char cap[16] = "off";
  if (context->limit_fps > 0) snprintf(cap, sizeof(cap), "%d", context->limit_fps);
  Line(model, 0, 2, "%dx%d  vsync %s  cap %s | ticks %.2f  repeats %.2f",
      context->width, context->height, context->vsync ? "on" : "off", cap,
      snapshot->counts[kPerformanceCount_Ticks], snapshot->counts[kPerformanceCount_Represents]);
  if (menu) {
    Line(model, 0, 3, "CPU menu %.2f  present/wait %.2f  sleep %.2f ms",
        snapshot->stages[kPerformance_SettingsUi].mean_ms,
        snapshot->stages[kPerformance_Swap].mean_ms, snapshot->stages[kPerformance_Pacing].mean_ms);
    Line(model, 0, 4, "Compact while settings open. Full CPU details in run log.");
    return;
  }
  if (!detailed) {
    PerformanceStage largest = kPerformance_Events;
    for (int i = kPerformance_Events; i <= kPerformance_Housekeeping; i++) {
      if (i == kPerformance_PostProcess || i == kPerformance_HostUi ||
          i == kPerformance_SettingsUi || i == kPerformance_Overlay ||
          i == kPerformance_Swap || i == kPerformance_Pacing) continue;
      if (snapshot->stages[i].mean_ms > snapshot->stages[largest].mean_ms) largest = (PerformanceStage)i;
    }
    Line(model, 0, 3, "Largest CPU stage: %s %.2f ms", PerformanceMetrics_StageName(largest),
        snapshot->stages[largest].mean_ms);
    Line(model, 0, 4, "Present/wait %.2f ms | sleep %.2f ms",
        snapshot->stages[kPerformance_Swap].mean_ms, snapshot->stages[kPerformance_Pacing].mean_ms);
    Line(model, 0, 5, "Owner %.2f / helpers* %.2f ms",
        snapshot->stages[kPerformance_WorkOwner].mean_ms, snapshot->stages[kPerformance_WorkHelpers].mean_ms);
    Line(model, 0, 6, "Join %.2f ms / jobs %.1f",
        snapshot->stages[kPerformance_WorkJoin].mean_ms, snapshot->counts[kPerformanceCount_HelperJobs]);
    Line(model, 0, 7, "GPU execution: unavailable");
    Line(model, 0, 8, "*Parallel sum, not frame time");
    Line(model, 0, 9, "Full stage details in run log");
    return;
  }
  Line(model, 0, 3, "CPU wall time: avg ms/present | peak ms/call. Nested rows overlap.");
  Line(model, 0, 4, "GPU execution: unavailable. Present/wait is NOT a GPU timer.");
  static const PerformanceStage main_stages[] = {
    kPerformance_Events, kPerformance_Emulation, kPerformance_Ppu,
    kPerformance_PpuSetup, kPerformance_PpuScanout, kPerformance_PpuFinish,
    kPerformance_SimColor, kPerformance_SimHud, kPerformance_SimDiagnostics,
    kPerformance_WorldMap, kPerformance_Metadata, kPerformance_TownCanvas,
    kPerformance_CanvasRaster, kPerformance_CanvasEnhance,
    kPerformance_Capture, kPerformance_Upload, kPerformance_Presentation,
    kPerformance_PostProcess, kPerformance_HostUi, kPerformance_SettingsUi,
    kPerformance_Overlay, kPerformance_Swap, kPerformance_Pacing, kPerformance_Housekeeping,
    kPerformance_WorkOwner, kPerformance_WorkHelpers, kPerformance_WorkJoin,
  };
  Line(model, 0, 5, "MAIN PIPELINE         AVG   PEAK");
  for (size_t i = 0; i < sizeof(main_stages) / sizeof(*main_stages); i++)
    Stage(model, snapshot, 0, 6 + (int)i, main_stages[i]);
  const bool action = context->scene == kPerformanceScene_Action;
  const int first = action ? kPerformance_ActionFirst : kPerformance_SimFirst;
  const int count = action ? kPerformance_ActionCount : kPerformance_SimCount;
  Line(model, 1, 5, "%s             AVG   PEAK", action ? "ACTION 3D" : "SIM/WORLD");
  for (int i = 0; i < count; i++) Stage(model, snapshot, 1, 6 + i, (PerformanceStage)(first + i));
  const double *work = snapshot->counts;
  Line(model, 1, 26, "Scene batches %.0f / verts %.0f", work[kPerformanceCount_Draws], work[kPerformanceCount_Vertices]);
  Line(model, 1, 27, "Scene upload %.2f + %.2f MiB",
      work[kPerformanceCount_UploadBytes] / 1048576, work[kPerformanceCount_DepthUploadBytes] / 1048576);
  Line(model, 1, 28, "Jobs %.1f / helpers %.1f", work[kPerformanceCount_WorkJobs], work[kPerformanceCount_HelperJobs]);
  Line(model, 1, 29, "Fallback %.2f / failed %.2f", work[kPerformanceCount_Fallbacks], work[kPerformanceCount_FailedPresents]);
  Line(model, 1, 30, "Counts above are per present.");
  Line(model, 1, 31, "*Helpers sum parallel work.");
  Line(model, 1, 32, "Scene counts exclude host UI.");
  Line(model, 0, 34, "Full details: run log");
  Line(model, 1, 34, "p95: up to 512 intervals");
}

static bool DrawPanel(ArRenderDevice *device, const PerformanceOverlayModel *model,
                       int offset_x, int offset_y) {
  const ArRenderRectF panel = {(float)(model->panel.x - offset_x), (float)(model->panel.y - offset_y),
      (float)model->panel.w, (float)model->panel.h};
  if (!ArRenderDevice_DrawSolidRect(device, &panel,
          (ArRenderColorF){.015f, .025f, .05f, .91f}, kArRenderBlendMode_Alpha))
    return false;
  for (int i = 0; i < model->line_count; i++) {
    const PerformanceOverlayLine *line = &model->lines[i];
    SettingsOverlay_DrawGameText(line->x - offset_x, line->y - offset_y, model->scale, 255, line->text);
  }
  return true;
}

bool PerformanceOverlay_Render(ArRenderDevice *device,
    const PerformanceSnapshot *snapshot, int level, ArRenderExtentI output) {
  if (level <= 0) { PerformanceOverlay_Reset(device); return true; }
  if (!snapshot || !ArRenderDevice_IsReady(device)) return true;
  /* Both formatting and glyph submission are sample-rate work. Warm frames
   * draw one retained panel; optional target failures keep the direct path. */
  if (!s_cache.initialized || s_cache.revision != snapshot->revision || s_cache.ready != snapshot->ready ||
      s_cache.level != level || s_cache.width != output.width || s_cache.height != output.height) {
    PerformanceOverlay_Build(snapshot, level, output, &s_cache.model);
    s_cache.revision = snapshot->revision;
    s_cache.ready = snapshot->ready;
    s_cache.level = level;
    s_cache.width = output.width;
    s_cache.height = output.height;
    s_cache.initialized = true;
    s_cache.texture_ready = false;
    if (s_cache.texture_width != s_cache.model.panel.w || s_cache.texture_height != s_cache.model.panel.h) {
      ArRenderDevice_DestroyTexture(device, s_cache.texture);
      s_cache.texture = ArRenderTexture_Invalid();
      s_cache.texture_width = s_cache.model.panel.w;
      s_cache.texture_height = s_cache.model.panel.h;
      s_cache.texture_unavailable = false;
    }
  }
  const PerformanceOverlayModel *model = &s_cache.model;
  if (!model->line_count) return true;
  if (!s_cache.texture_ready && !s_cache.texture_unavailable) {
    const ArRenderTextureDesc desc = {.width = model->panel.w, .height = model->panel.h,
        .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Target,
        .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_AlphaPremultiplied};
    if (!ArRenderCapabilities_Has(ArRenderDevice_Capabilities(device),
            kArRenderCapability_ScopedRenderTargets | kArRenderCapability_RenderTargets) ||
        (!ArRenderTexture_IsValid(s_cache.texture) && !ArRenderDevice_CreateTexture(device, &desc, &s_cache.texture))) {
      s_cache.texture_unavailable = true;
    } else {
      ArRenderTargetState saved;
      const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(device, s_cache.texture, &saved);
      if (begin == kArRenderTargetBegin_StateLost) return false;
      if (begin == kArRenderTargetBegin_Ready) {
        if (ArRenderDevice_Clear(device, (ArRenderColorF){0})) {
          s_cache.texture_ready = DrawPanel(device, model, model->panel.x, model->panel.y);
        }
        if (!ArRenderDevice_EndTarget(device, &saved)) return false;
      }
      if (!s_cache.texture_ready) s_cache.texture_unavailable = true;
    }
  }
  if (!s_cache.texture_ready) { DrawPanel(device, model, 0, 0); return true; }
  const ArRenderRectF panel = {(float)model->panel.x, (float)model->panel.y,
      (float)model->panel.w, (float)model->panel.h};
  /* A rejected draw may have submitted partially. Do not double-composite it
   * with an immediate fallback; use the direct path on the next frame. */
  if (!ArRenderDevice_DrawTexture(device, s_cache.texture, NULL, &panel)) {
    s_cache.texture_ready = false;
    s_cache.texture_unavailable = true;
  }
  return true;
}
