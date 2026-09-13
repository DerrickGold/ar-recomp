#include "present_sim_globe_mapping.h"
#include "sim/sim_town_terrain.h"
/* Keep checks and fixture setup active in release-configured test builds. */
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
  const float origins[6][2] = {{80,48},{48,48},{16,64},{16,32},{64,96},{32,0}};
  unsigned points = 0;
  for (int town = 1; town <= 6; ++town) for (int height = 0; height <= 150; height += 25) {
    SimGlobeMapping map;
    const float ox = origins[town-1][0], oy = origins[town-1][1];
    assert(SimGlobeMapping_Build(town,ox,oy,96,1.5f,height/100.0f,&map));
    for (int y = -16; y <= 48; y += 2) for (int x = -16; x <= 48; x += 2) {
      float p[3], n[3], e[2];
      assert(SimGlobeMapping_Point(&map,ox+x,oy+y,2,.7f,p));
      assert(SimGlobeMapping_Encode(&map,ox+x,oy+y,2,.7f,n,e));
      const float radius = map.radius+e[0];
      for (int i = 0; i < 3; ++i)
        assert(fabsf(n[i]*radius-(i == 2 ? map.radius : 0)-p[i]) < .0001f);
      assert(fabsf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]-1) < .000001f);
      if (x >= 0 && x < 32 && y >= 0 && y < 32) {
        assert(SimGlobeMapping_Weight(&map,ox+x,oy+y) == 0);
        assert(p[0] == x-16 && p[1] == 16-y);
        const float expected = SimTownTerrain_HeightUnitsAt(town,x*16,y*16)*height/100.0f+.7f;
        assert(fabsf(p[2]-expected) < .00001f);
      }
      if (x <= -8 || x >= 40 || y <= -8 || y >= 40) {
        assert(SimGlobeMapping_Weight(&map,ox+x,oy+y) == 1);
        float normal[3];
        assert(SimWorldNavigationGlobe_SampleAtRadius(96,ox+x,oy+y,normal,NULL));
        SimWorldNavigationGlobe_TransformNormal(&map.frame,normal,normal);
        const float r = map.radius+2*map.landscape/map.metric+.7f;
        assert(fabsf(p[0]-normal[0]*r) < .00001f);
        assert(fabsf(p[1]-normal[1]*r) < .00001f);
      }
      ++points;
    }
    float a[3], b[3];
    assert(SimGlobeMapping_Point(&map,ox+32,oy+16,3,0,a));
    assert(SimGlobeMapping_Point(&map,ox+32.001f,oy+16,3,0,b));
    assert(fabsf(a[0]-b[0]) < .0011f && fabsf(a[2]-b[2]) < .00001f);
    /* Disabling only world relief must not flatten the active town edge. */
    map.landscape = 0;
    map.town_landscape = height/100.0f;
    assert(SimGlobeMapping_Point(&map,ox+32,oy+16,3,0,a));
    const float edge = SimTownTerrain_HeightUnitsAt(town,511.999f,256)*height/100.0f;
    assert(fabsf(a[2]-edge) < .00001f);
    assert(SimGlobeMapping_Point(&map,ox+32.001f,oy+16,3,0,b));
    assert(fabsf(a[2]-b[2]) < .00001f);
    assert(!SimGlobeMapping_Build(0,ox,oy,96,0,1,&map));
    assert(!SimGlobeMapping_Build(town,ox,oy,NAN,0,1,&map));
    assert(!SimGlobeMapping_Point(NULL,ox,oy,0,0,a));
    assert(!SimGlobeMapping_Point(&map,NAN,oy,0,0,a));
  }
  printf("mapping: %u point/round-trip/ownership checks passed\n",points);
}
