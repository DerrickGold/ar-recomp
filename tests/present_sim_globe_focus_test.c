#include "present_sim_globe_focus.h"
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  SimFrameData *sim=calloc(1,sizeof(*sim)); assert(sim);
  unsigned samples=0;
  for (unsigned variant=0;variant<24;++variant) {
    const SimGlobeMapping map={.town=1,.origin_x=variant%3*32,.origin_y=variant%4*24,
      .chart_radius=96*(1+variant%4)};
    const ArRenderRectI source={variant%3*20,variant%2*32,360,224};
    sim->effective_features=kSimFeature_CullHaze|kSimFeature_ObjectBillboards|kSimFeature_VirtualHeight;
    sim->camera_x=variant*11; sim->camera_y=variant*7;
    sim->underlay_screen_x0=variant*13; /* Cancels when recovering native anchors. */
    sim->sprite_margin_left=variant%3*80; sim->sprite_margin_right=variant%4*64;
    sim->sprite_margin_top=variant%3*16; sim->sprite_margin_bottom=variant%2*32;
    sim->cull_haze_lead_px=variant%4 ? variant%4*16 : 0;
    sim->cull_corner_px=variant%4*64; sim->cull_lift_inset=variant%2;
    sim->height_scale_x100=100;
    sim->cull_dim_pct=35; sim->cull_haze_pct=10;
    const Sim3DDepthSurfaceFocus focus=PresentSimGlobeFocus_ResolveVisibility(&map,sim,source);
    for (int y=-64;y<=576;y+=8) for (int x=-64;x<=576;x+=8) {
      const float expected=Sim3D_CullProximity(x-sim->camera_x+16,y-sim->camera_y-source.y+17,
          sim->sprite_margin_left,sim->sprite_margin_right,sim->sprite_margin_top,sim->sprite_margin_bottom,
          sim->cull_haze_lead_px ? sim->cull_haze_lead_px : kSimCullHazeLeadDefaultPx,
          sim->cull_corner_px,sim->cull_lift_inset ? Sim3D_MaxDrawLift(100) : 0);
      const float actual=PresentSimGlobeFocus_Weight(&focus,(map.origin_x+x/16.0f)/128,
          (map.origin_y+y/16.0f)/128);
      assert(fabsf(actual-expected)<.00002f); ++samples;
    }
    const ArRenderColorF color={.4f,.6f,.8f,.5f};
    const ArRenderColorF clear=PresentSimGlobeFocus_Color(&focus,0,color);
    assert(clear.r==color.r && clear.g==color.g && clear.b==color.b && clear.a==color.a);
    assert(PresentSimGlobeFocus_Color(&focus,1,color).a==color.a);
    sim->effective_features=0;
    const Sim3DDepthSurfaceFocus disabled=PresentSimGlobeFocus_ResolveVisibility(&map,sim,source);
    assert(PresentSimGlobeFocus_Weight(&disabled,-2,-2)==0);
    sim->effective_features=kSimFeature_CullHaze; /* No lift without billboards/height. */
    const Sim3DDepthSurfaceFocus flat=PresentSimGlobeFocus_ResolveVisibility(&map,sim,source);
    assert(flat.clear_rect.h==(kSimSpriteWindowBiasedHeight+sim->sprite_margin_top+
        sim->sprite_margin_bottom)/(16.0f*128));
  }
  /* Zero default haze/rounding must still retain the rectangular darkening
   * cue. Evaluate the actual defaults, not only the rounded benchmark mask. */
  sim->effective_features=kSimFeature_CullHaze;
  sim->cull_haze_pct=kSimCullHazeDefaultPct;
  sim->cull_dim_pct=kSimCullDimDefaultPct;
  sim->cull_corner_px=kSimCullCornerDefaultPx;
  sim->cull_haze_lead_px=kSimCullHazeLeadDefaultPx;
  const SimGlobeMapping map={.town=1,.origin_x=32,.origin_y=24,.chart_radius=288};
  const Sim3DDepthSurfaceFocus defaults=PresentSimGlobeFocus_ResolveVisibility(
      &map,sim,(ArRenderRectI){0,0,360,224});
  assert(defaults.corner_radius==0 && defaults.haze.a==0 && defaults.dim>0);
  const ArRenderRectF r=defaults.clear_rect;
  assert(PresentSimGlobeFocus_Weight(&defaults,r.x+r.w*.5f,r.y+r.h*.5f)==0);
  assert(PresentSimGlobeFocus_Weight(&defaults,r.x-1,r.y-1)==1);
  const ArRenderColorF dimmed=PresentSimGlobeFocus_Color(&defaults,1,
      (ArRenderColorF){.4f,.6f,.8f,.5f});
  assert(fabsf(dimmed.r-.28f)<.000001f && fabsf(dimmed.g-.42f)<.000001f);
  assert(fabsf(dimmed.b-.56f)<.000001f && dimmed.a==.5f);
  free(sim);
  printf("globe visibility: %u native cull-mask samples; pan, margins, rounded corners, lift and off parity PASS\n",samples);
  return 0;
}
