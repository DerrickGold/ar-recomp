#include "present_sim_globe_mapping.h"
#include "sim/sim_town_terrain.h"
/* Keep checks and fixture setup active in release-configured test builds. */
#undef NDEBUG
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static void TestContinuousSurface(void) {
  const float origins[6][2] = {{80,48},{48,48},{16,64},{16,32},{64,96},{32,0}};
  for (unsigned town = 1; town <= 6; ++town) {
    float previous_drop = INFINITY;
    for (unsigned scale = 1; scale <= 4; ++scale) {
      SimGlobeMapping map;
      const float ox = origins[town-1][0], oy = origins[town-1][1];
      assert(SimGlobeMapping_Build(town,ox,oy,96*scale,1.5f,1,&map));
      for (int y=-8;y<=40;y+=3) for (int x=-8;x<=40;x+=3) {
        float point[3],sx,sy,rise;
        assert(SimGlobeMapping_Point(&map,ox+x,oy+y,2.75f,.87f,point));
        assert(SimGlobeMapping_Source(&map,point,&sx,&sy,&rise));
        assert(fabsf(sx-(ox+x))<.0002f && fabsf(sy-(oy+y))<.0002f);
        assert(fabsf(rise-(2.75f*map.landscape/map.metric+.87f))<.0002f);
      }
      /* Subtracting the planet center loses a few ulps of its radius, not
       * of the much smaller local elevation. Bound that float roundoff. */
      const float epsilon = 8*FLT_EPSILON*(map.radius+8);
      /* The local metric stays one SIM cell, rather than scaling the town
       * together with the planet. The reference elevation remains centered. */
      float center[3], near[3], edge[3];
      assert(SimGlobeMapping_Point(&map,ox+16,oy+16,1.5f,0,center));
      assert(fabsf(center[2]) < epsilon);
      assert(SimGlobeMapping_Point(&map,ox+16.25f,oy+16,1.5f,0,near));
      assert(fabsf((near[0]-center[0])/.25f-1) < .02f);
      assert(SimGlobeMapping_Point(&map,ox+32,oy+16,1.5f,0,edge));
      const float drop = center[2]-edge[2];
      assert(drop > 0 && drop < previous_drop);
      previous_drop = drop;
      /* No hidden flat footprint: interior, boundary and outside all sample
       * the same sphere. Encoding remains compatible with the radial GPU ABI. */
      for (int y = -8; y <= 40; y += 4) for (int x = -8; x <= 40; x += 4) {
        float p[3], normal[3], encoded[3], elevation[2];
        assert(SimGlobeMapping_Point(&map,ox+x,oy+y,2,.7f,p));
        assert(SimGlobeMapping_Encode(&map,ox+x,oy+y,2,.7f,encoded,elevation));
        assert(SimWorldNavigationGlobe_SampleAtRadius(map.chart_radius,ox+x,oy+y,normal,NULL));
        SimWorldNavigationGlobe_TransformNormal(&map.frame,normal,normal);
        const float radius = map.radius+2/map.metric+.7f;
        for (int i = 0; i < 3; ++i) {
          const float center_offset = i == 2 ? map.radius+1.5f/map.metric : 0;
          assert(fabsf(p[i]-(normal[i]*radius-center_offset)) < epsilon);
          assert(fabsf(encoded[i]*(map.radius+elevation[0])-
              (i == 2 ? map.radius : 0)-p[i]) < epsilon);
        }
      }
      /* Walking and ground-relative flight map each interpolated chart
       * position. Verify exact altitude for an entire path, not just ends. */
      float endpoints[2][3];
      for (unsigned flight = 0; flight < 2; ++flight) {
        const float altitude = flight ? 2 : 0;
        for (unsigned tick = 0; tick <= 128; ++tick) {
          const float t = tick/128.0f;
          const float x = ox+2+28*t, y = oy+3+26*t;
          const float ground = 1+2*t;
          float p[3];
          assert(SimGlobeMapping_Point(&map,x,y,ground,altitude,p));
          const float z = p[2]+map.radius+map.reference_height/map.metric;
          const float radial = hypotf(hypotf(p[0],p[1]),z);
          assert(fabsf(radial-map.radius-ground/map.metric-altitude) < epsilon);
          if (!flight && (tick == 0 || tick == 128))
            for (int i = 0; i < 3; ++i) endpoints[tick == 128][i] = p[i];
        }
      }
      const float chord_x = (endpoints[0][0]+endpoints[1][0])*.5f;
      const float chord_y = (endpoints[0][1]+endpoints[1][1])*.5f;
      const float chord_z = (endpoints[0][2]+endpoints[1][2])*.5f+
          map.radius+map.reference_height/map.metric;
      assert(hypotf(hypotf(chord_x,chord_y),chord_z) < map.radius+2/map.metric-.1f);
    }
  }
  puts("continuous mapping: six towns, four radii, local scale, radial round trips, walking/flight paths passed");
}

int main(void) {
  TestContinuousSurface();
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
      {
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
    assert(fabsf(a[0]-b[0]) < .0011f && fabsf(a[2]-b[2]) < .0011f);
    /* Zero landscape removes relief, not curvature. There is no implicit
     * town-height contribution or footprint blend in the mapping. */
    map.landscape = 0;
    map.town_landscape = height/100.0f;
    assert(SimGlobeMapping_Point(&map,ox+32,oy+16,3,0,a));
    assert(SimGlobeMapping_Point(&map,ox+32,oy+16,0,0,b));
    assert(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
    assert(a[2] < 0);
    assert(SimGlobeMapping_Point(&map,ox+32.001f,oy+16,3,0,b));
    assert(fabsf(a[2]-b[2]) < .0011f);
    assert(!SimGlobeMapping_Build(0,ox,oy,96,0,1,&map));
    assert(!SimGlobeMapping_Build(town,ox,oy,NAN,0,1,&map));
    assert(!SimGlobeMapping_Point(NULL,ox,oy,0,0,a));
    assert(!SimGlobeMapping_Point(&map,NAN,oy,0,0,a));
  }
  printf("mapping: %u point/round-trip/ownership checks passed\n",points);
}
