#ifndef ACTRAISER_DISPLAY_GEOMETRY_H
#define ACTRAISER_DISPLAY_GEOMETRY_H

#include <stdbool.h>

/* ActRaiser's live display geometry is host/game-adapter policy, not runner
 * state. The 120-pixel cap comes from the title's tilemap-streaming budget;
 * it is intentionally narrower than SR_PPU_HORIZONTAL_MARGIN_MAX. */
enum { kActRaiserWidescreenExtraMax = 120 };

typedef struct ActRaiserDisplayGeometry {
  bool widescreen_active;
  int render_extra;
  int display_extra;
  int extra_top;
  int extra_bottom;
} ActRaiserDisplayGeometry;

/* Consumers receive a read-only process-lifetime view. Horizontal mutation is
 * owned by the host display policy; vertical mutation is owned by the
 * ActRaiser frame-policy adapter. */
extern const ActRaiserDisplayGeometry *const g_actraiser_display_geometry;
void DisplayGeometry_SetHorizontal(int render_extra, int display_extra);
void DisplayGeometry_SetVertical(int extra_top, int extra_bottom);

/* These read-only aliases keep existing rendering expressions compact. Their
 * const expansion makes accidental mutation a compile error. */
#define g_ws_active (g_actraiser_display_geometry->widescreen_active)
#define g_ws_extra (g_actraiser_display_geometry->render_extra)
#define g_ws_display_extra (g_actraiser_display_geometry->display_extra)
#define g_ws_extra_top (g_actraiser_display_geometry->extra_top)
#define g_ws_extra_bottom (g_actraiser_display_geometry->extra_bottom)

#endif /* ACTRAISER_DISPLAY_GEOMETRY_H */
