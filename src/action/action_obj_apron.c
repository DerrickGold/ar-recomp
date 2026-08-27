#include "action_obj_apron.h"

#include <string.h>

#include "constants.h"

int ActionApron_SurfaceWidth(const ActionApronGeometry *g) {
  if (!g) return 0;
  return kActRaiserAuthenticWidth + 2 * g->ws_extra + 2 * g->apron;
}

int ActionApron_SurfaceColumn(const ActionApronGeometry *g, int screen_x) {
  if (!g) return 0;
  return screen_x + g->ws_extra + g->apron;
}

void ActionApron_LeftSpan(const ActionApronGeometry *g, int *x0, int *x1) {
  if (!g || !x0 || !x1) return;
  /* Column 0 is screen x = -(ws_extra + apron); the display span starts at
   * -ws_extra. */
  *x0 = -(g->ws_extra + g->apron);
  *x1 = -g->ws_extra;
}

void ActionApron_RightSpan(const ActionApronGeometry *g, int *x0, int *x1) {
  if (!g || !x0 || !x1) return;
  *x0 = kActRaiserAuthenticWidth + g->ws_extra;
  *x1 = kActRaiserAuthenticWidth + g->ws_extra + g->apron;
}

bool ActionApron_PartTouchesApron(const ActionApronGeometry *g, int x,
                                  int size) {
  if (!g || size <= 0 || g->apron <= 0) return false;
  int l0, l1, r0, r1;
  ActionApron_LeftSpan(g, &l0, &l1);
  ActionApron_RightSpan(g, &r0, &r1);
  const int x1 = x + size;
  return (x < l1 && x1 > l0) || (x < r1 && x1 > r0);
}

bool ActionApron_PartUsesColorMath(uint16_t tile_attr) {
  /* Palette group 4-7. PpuRasterizeParts derives its palette base from the
   * same three bits, so this cannot drift from the colour it will produce. */
  return ((tile_attr >> 9) & 7) >= 4;
}

/* ── Per-frame channel ──────────────────────────────────────────────────── */

static SrPpuObjPart s_parts[kActionApronMaxParts];
static int s_count;
static int s_overflow;
static int s_peak;

void ActionApron_BeginFrame(void) {
  s_count = 0;
  /* s_overflow and s_peak deliberately accumulate across the run: they are
   * reported once as a sizing verdict, not per frame. */
}

bool ActionApron_AddPart(const ActionApronGeometry *g, int screen_x,
                         int screen_y, uint16_t tile_attr, uint8_t size) {
  if (!g || !size) return false;
  if (!ActionApron_PartTouchesApron(g, screen_x, size))
    return false;
  if (s_count >= kActionApronMaxParts) {
    s_overflow++;
    return false;
  }
  s_parts[s_count].x = (int16_t)screen_x;
  s_parts[s_count].y = (int16_t)screen_y;
  s_parts[s_count].tile_attr = tile_attr;
  s_parts[s_count].size = size;
  s_count++;
  if (s_count > s_peak) s_peak = s_count;
  return true;
}

int ActionApron_Count(void) { return s_count; }
int ActionApron_Overflow(void) { return s_overflow; }
int ActionApron_PeakCount(void) { return s_peak; }
const SrPpuObjPart *ActionApron_Parts(void) { return s_parts; }
