#ifndef AR_DISPLAY_GEOMETRY_H
#define AR_DISPLAY_GEOMETRY_H

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
  int auto_vertical_budget; /* Requested canvas; not the bounded capture. */
} ActRaiserDisplayGeometry;

typedef struct ActRaiserAutoCanvas {
  int extra_columns;
  int extra_rows;
} ActRaiserAutoCanvas;

/* Expand one axis of 256x224, with 120 columns/64 rows per side.
 * Invalid/minimized drawable sizes return false and leave the result intact. */
bool DisplayGeometry_ResolveAutoCanvas(
    int drawable_width, int drawable_height, bool crt_pixel_aspect,
    ActRaiserAutoCanvas *canvas);
/* Restrict an already bounded Auto budget to what the current scene can draw.
 * bg_mode=-1 means unavailable. projected_sim uses the final presentation
 * decision, including capture fallback; classic scenes never add PPU rows. */
ActRaiserAutoCanvas DisplayGeometry_ConstrainAutoCanvas(
    ActRaiserAutoCanvas requested, int map_group, int map_number, int bg_mode,
    bool projected_sim);
void DisplayGeometry_SetAutoVerticalBudget(int budget);

/* Consumers receive a read-only process-lifetime view. Horizontal mutation is
 * owned by the host display policy; vertical mutation is owned by the
 * ActRaiser frame-policy adapter. */
extern const ActRaiserDisplayGeometry *const g_actraiser_display_geometry;
/* Pure host sizing policy. A zero aspect axis selects the native viewport;
 * vertical capture is resolved separately by the frame-policy adapter. */
ActRaiserDisplayGeometry DisplayGeometry_CalculateHorizontal(
    int height, int aspect_x, int aspect_y,
    bool crt_pixel_aspect, bool diorama_mode);
void DisplayGeometry_SetHorizontal(int render_extra, int display_extra);
void DisplayGeometry_SetVertical(int extra_top, int extra_bottom);

/* These read-only aliases keep existing rendering expressions compact. Their
 * const expansion makes accidental mutation a compile error. */
#define g_ws_active (g_actraiser_display_geometry->widescreen_active)
#define g_ws_extra (g_actraiser_display_geometry->render_extra)
#define g_ws_display_extra (g_actraiser_display_geometry->display_extra)
#define g_ws_extra_top (g_actraiser_display_geometry->extra_top)
#define g_ws_extra_bottom (g_actraiser_display_geometry->extra_bottom)

#endif /* AR_DISPLAY_GEOMETRY_H */
