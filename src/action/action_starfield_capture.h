#ifndef AR_ACTION_STARFIELD_CAPTURE_H
#define AR_ACTION_STARFIELD_CAPTURE_H
#include "action_starfield.h"
#include "action_environment_scene.h"
#include <stdlib.h>

/* Use the native row scroll and edit-resolved palette indices, just as terrain
 * lightning does. A small weighted footprint softens cloud-edge crossings.
 * Missing source facts suppress masked stars rather than revealing them over
 * clouds or mountains. Work is bounded by the authored sample count. */
static inline uint8_t ActionStarfield_Coverage(const ActionEnvironmentScene *scene,
    const ActionRoomSceneFrameState *frame,const ActionEffectInstance *e,ArRenderPointF p) {
  const int width=(int)scene->maps[1].world_width,height=(int)scene->maps[1].world_height;
  if(width<=0||height<=0)return 0;
  const float sx=e->world_x+p.x-scene->camera_x[1],sy=e->world_y+p.y-scene->camera_y[1];
  unsigned clear=0;
  for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx) {
    const unsigned row=(unsigned)fminf(223,fmaxf(0,floorf(sy+dy)));
    int x=(int)floorf(sx+dx+frame->bg_hscroll[1][row])%width;
    const int y=(int)floorf(sy+dy+frame->bg_vscroll[1][row]);
    if(x<0)x+=width;
    unsigned index;
    if(y>=0&&y<height&&ActionEnvironmentScene_PaletteIndex(scene,1,x,y,&index)&&
        (e->field_style.sky.palettes&(1u<<(index/16)))&&
        (e->field_style.sky.colors&(1u<<(index&15))))
      clear+=(3-abs(dx))*(3-abs(dy));
  }
  return (uint8_t)((clear*255+40)/81);
}
static inline void ActionStarfield_Capture(const ActionEnvironmentScene *scene,
    const ActionRoomSceneFrameState *frame,ActionEffectInstance *e) {
  if(!e->field_style.sky.colors)return;
  if(!scene||!scene->vram||!frame||e->particle_count>kActionSkyMaxStars)return;
  for(unsigned i=0;i<e->particle_count;++i)
    e->sky_visibility.alpha[i]=ActionStarfield_Coverage(scene,frame,e,ActionStarfield_Point(e,i));
  if(e->field_style.scatter>0)
    for(unsigned col=0;col<=kActionSkyHazeSegments;++col)
      e->sky_visibility.alpha[kActionSkyMaxStars+col*3+1]=
          ActionStarfield_Coverage(scene,frame,e,ActionStarfield_HazePoint(e,col,1));
  e->sky_visibility.valid=1;
}
#endif
