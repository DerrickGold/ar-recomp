#include "action_floor_support.h"
#include <math.h>

unsigned ActionFloorSupport_Cell(const ActionEnvironmentScene *s, int x, int y) {
  uint8_t below, above;
  if (!s || !ActionBgMapView_LookupMetatile(&s->maps[0],x,y,&below)) return 0;
  const unsigned mask = s->collision[below] & 15;
  /* Verified Fillmore temple ledges are one-way top surfaces. Other partial
   * shapes remain visible in the inspector but are not guessed to be flat. */
  const bool capital = s->group == 1 && (s->room == 2 || s->room == 3) &&
      below >= 0x54 && below <= 0x57 && mask == 3;
  const int top = (y / 16) * 16;
  const bool open = top == 0 || (ActionBgMapView_LookupMetatile(&s->maps[0],x,top-1,&above) &&
      s->collision[above] == 0);
  return mask | ((mask == 15 || capital) && open ? 16u : 0u);
}
void ActionFloorSupport_Resolve(const ActionEnvironmentScene *s,
    const ActionEffectLocalRect *area, float height, ActionEffectFloorField *out) {
  if (!out) return;
  *out = (ActionEffectFloorField){0};
  if (!s || !area || !isfinite(area->x0) || !isfinite(area->x1) ||
      !isfinite(area->y0) || !isfinite(area->y1) || !isfinite(height) ||
      area->x0 < -2304 || area->y0 < -2304 || area->x1 > 16640 || area->y1 > 16640 ||
      area->x1 <= area->x0 || area->y1 <= area->y0 ||
      area->x1-area->x0 > 512 || area->y1-area->y0 > 512 || height < 4 || height > 64) return;
  const int x0 = (int)floorf(fmaxf(0,area->x0)/16)*16;
  const int y0 = (int)ceilf(fmaxf(0,area->y0)/16)*16;
  for (int x=x0; x<area->x1 && (unsigned)x<s->maps[0].world_width; x+=16) {
    for (int y=y0; y<=area->y1 && (unsigned)y<s->maps[0].world_height; y+=16) {
      if (!(ActionFloorSupport_Cell(s,x,y)&16)) continue;
      float clearance = fminf(height,y-fmaxf(0,area->y0));
      /* Spikes and non-solid background detail have zero collision; continue
       * through them to the base. Stop mist below any solid ceiling. */
      for (int cy=y-1; cy>=y-clearance; cy-=16) {
        if (ActionFloorSupport_Cell(s,x,cy)&15) {
          clearance=fminf(clearance,y-(cy/16+1)*16);break;
        }
      }
      if (clearance < 1) continue;
      ActionEffectFloorSpan span = {fmaxf(x,area->x0),fminf(x+16,area->x1),(float)y,clearance};
      ActionEffectFloorSpan *last = out->count ? &out->spans[out->count-1] : NULL;
      if (last && last->x1==span.x0 && last->y==span.y && last->height==span.height && span.x1-last->x0<=64) last->x1=span.x1;
      else if (out->count < kActionAuthoredFloorMaxSpans) out->spans[out->count++]=span;
      else { *out=(ActionEffectFloorField){0}; return; }
      break;
    }
  }
}
