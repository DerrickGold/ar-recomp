#include "present_sim_globe_project.h"
#include "sim/sim_town_terrain.h"
#undef NDEBUG
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void TestMotion(void) {
  const float origins[6][2] = {{80,48},{48,48},{16,64},{16,32},{64,96},{32,0}};
  const ArRenderRectI source = {0,0,360,224}, viewport = {13,17,800,600};
  unsigned samples = 0;
  for (unsigned town = 1; town <= 6; ++town) for (unsigned radius = 1; radius <= 4; ++radius)
    for (unsigned angle = 0; angle < 3; ++angle) {
      SimGlobeMapping map;
      assert(SimGlobeMapping_Build(town,origins[town-1][0],origins[town-1][1],
          96*radius,2,1,&map));
      const Scene3DCamera camera = {-.575f-.3f*angle,.15f*angle,4.5f,.4f};
      float matrix[16]; Scene3D_BuildViewProjection(&camera,viewport.w,viewport.h,matrix);
      /* Scale local cells into SIM camera units; framing offsets are owned by
       * the caller and tested separately with actual globe output on GPU. */
      for (unsigned col = 0; col < 3; ++col) for (unsigned row = 0; row < 4; ++row)
        matrix[col*4+row] *= col == 0 ? 16.0f*viewport.w/viewport.h/source.w : 16.0f/source.h;
      PresentSimGlobeProjection projection;
      const float reference = Scene3D_AutoFitDistance(camera.fov_y);
      assert(PresentSimGlobeProject_Build(&map,matrix,source,viewport,reference,&projection));
      const float flight_datum = 2+SimTownTerrain_MaximumUnits(town);
      for (unsigned step = 0; step <= 128; ++step) {
        const float t = step/128.0f;
        /* Include incoming/outgoing positions. Native XY is not clamped. */
        const float x = -16+544*t, y = 32+448*t;
        const float ground = 2+SimTownTerrain_HeightUnitsAt(town,fmaxf(0,fminf(x,511.999f)),y);
        for (unsigned flying = 0; flying < 2; ++flying) {
          PresentSimGlobeProjectedPoint point;
          const float support = flying ? flight_datum : ground;
          const float altitude = flying ? 24+16*sinf(t*3.14159265f) : 0;
          assert(PresentSimGlobeProject_Point(&projection,x,y,support,altitude,&point));
          const float radial = hypotf(hypotf(point.world[0],point.world[1]),
              point.world[2]+map.radius+map.reference_height/map.metric);
          assert(fabsf(radial-map.radius-support/map.metric-altitude/16) <
              16*FLT_EPSILON*(map.radius+32));
          Scene3DPoint oracle; float depth;
          assert(Scene3D_ProjectWorldPointWithDepth(matrix,point.world[0],point.world[1],
              point.world[2],viewport.w,viewport.h,&oracle,&depth));
          assert(fabsf(point.screen.x-viewport.x-oracle.x) < .0002f);
          assert(fabsf(point.screen.y-viewport.y-oracle.y) < .0002f);
          assert(point.depth == depth);
          const float scale = Scene3D_ProjectBillboardScale(matrix,
              point.world[0],point.world[1],point.world[2],reference);
          assert(fabsf(point.pixel_scale[0]-(float)viewport.w/source.w*scale) < .00001f);
          if (!flying) {
            const PresentSimGlobeProjectedPoint original=point;
            PresentSimGlobeBillboardAxes axes;
            assert(PresentSimGlobeProject_GroundBillboardAxes(&projection,x,y,&point,&axes));
            /* Independent finite difference of the rendered ground, not the
             * billboard's basis recipe. The feet must retain its eastward
             * screen direction even away from the town centre. */
            PresentSimGlobeProjectedPoint left,right;
            /* A one-cell centred secant avoids subpixel cancellation when
             * the sphere's large radial coordinate is subtracted in float. */
            assert(PresentSimGlobeProject_Point(&projection,x-8,y,support,altitude,&left));
            assert(PresentSimGlobeProject_Point(&projection,x+8,y,support,altitude,&right));
            const float dx=right.screen.x-left.screen.x,dy=right.screen.y-left.screen.y;
            const float cross=axes.right.x*dy-axes.right.y*dx;
            if (fabsf(cross)/(hypotf(dx,dy)*hypotf(axes.right.x,axes.right.y)) >= .001f)
              fprintf(stderr,"ground tangent town=%u radius=%u angle=%u step=%u axis=%.6f,%.6f ground=%.6f,%.6f\n",
                  town,radius,angle,step,axes.right.x,axes.right.y,dx,dy);
            assert(fabsf(cross)/(hypotf(dx,dy)*hypotf(axes.right.x,axes.right.y)) < .001f);
            assert(axes.right.x*dx+axes.right.y*dy > 0);
            assert(axes.right.x*axes.down.y-axes.right.y*axes.down.x > 0);
            assert(!memcmp(&point,&original,sizeof(point))); /* No foot/height adjustment. */
          }
          ++samples;
        }
      }
      /* Copies survive caller-side camera/map changes. */
      PresentSimGlobeProjectedPoint before, after;
      assert(PresentSimGlobeProject_Point(&projection,256,256,2,0,&before));
      memset(matrix,0,sizeof(matrix)); memset(&map,0,sizeof(map));
      assert(PresentSimGlobeProject_Point(&projection,256,256,2,0,&after));
      assert(!memcmp(&before,&after,sizeof(before)));
    }
  printf("curved projection: %u walking/flight samples; six towns, four radii, three angles; depth/scale parity\n",samples);
}

static void TestPitchOnlyFacing(void) {
  const ArRenderRectI source={0,0,360,224}, viewport={11,13,960,600};
  const float pitches[]={-.575f,-.75f,-1.35f};
  const float yaws[]={-.7f,-.35f,0,.35f,.7f};
  SimGlobeMapping map;
  assert(SimGlobeMapping_Build(4,16,32,288,2,.4f,&map));
  for (unsigned p=0;p<3;++p) for (unsigned y=0;y<5;++y) {
    const Scene3DCamera camera={pitches[p],yaws[y],2,.4f};
    float matrix[16]; Scene3D_BuildViewProjection(&camera,viewport.w,viewport.h,matrix);
    for (unsigned col=0;col<3;++col) for (unsigned row=0;row<4;++row)
      matrix[col*4+row]*=col==0 ? 16.0f*viewport.w/viewport.h/source.w : 16.0f/source.h;
    PresentSimGlobeProjection projection;
    assert(PresentSimGlobeProject_Build(&map,matrix,source,viewport,
        Scene3D_AutoFitDistance(camera.fov_y),&projection));
    PresentSimGlobeProjectedPoint anchor;
    assert(PresentSimGlobeProject_Point(&projection,256,256,2,0,&anchor));
    PresentSimGlobeBillboardAxes axes;
    assert(PresentSimGlobeProject_GroundBillboardAxes(&projection,256,256,&anchor,&axes));
    /* At the town centre local east/north/radial are XYZ. An independent
     * analytic pitch-only rotation about X retains camera yaw/roll, unlike
     * the old screen-aligned rectangle. */
    const float sx=sinf(camera.tilt_x),cx=cosf(camera.tilt_x);
    const float sy=sinf(camera.tilt_y),cy=cosf(camera.tilt_y);
    const float length=hypotf(sx,cx*cy);
    const float pixel_aspect=(float)viewport.w*source.h/(viewport.h*source.w);
    assert(fabsf(axes.right.x-anchor.pixel_scale[0]*cy) < .001f);
    assert(fabsf(axes.right.y-anchor.pixel_scale[1]*pixel_aspect*sx*sy) < .001f);
    assert(fabsf(axes.down.x-anchor.pixel_scale[0]/pixel_aspect*sy*(-sx)/length) < .001f);
    assert(fabsf(axes.down.y-anchor.pixel_scale[1]*cy/length) < .001f);
    if (yaws[y]!=0) assert(fabsf(axes.right.y) > .1f);
    else { /* Ordinary neutral-yaw framing retains the original dimensions. */
      assert(fabsf(axes.right.x-anchor.pixel_scale[0]) < .001f);
      assert(fabsf(axes.down.y-anchor.pixel_scale[1]) < .001f);
    }
    /* A two-part person uses the same foot basis for both pieces. Joining
     * corners and a point exactly at the feet are unchanged by the lean. */
    const Scene3DPoint joint={anchor.screen.x-8*axes.down.x,anchor.screen.y-8*axes.down.y};
    const Scene3DPoint top_bottom={anchor.screen.x-16*axes.down.x+8*axes.down.x,
        anchor.screen.y-16*axes.down.y+8*axes.down.y};
    assert(hypotf(joint.x-top_bottom.x,joint.y-top_bottom.y) < .0001f);
  }
  puts("grounded billboards: pitch-only tangent frame, yaw/roll retained, neutral scale and multipart joins PASS");
}

static void TestInvalid(void) {
  SimGlobeMapping map;
  assert(SimGlobeMapping_Build(4,16,32,192,2,1,&map));
  float matrix[16] = {0}; matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
  const ArRenderRectI rect = {0,0,800,600};
  PresentSimGlobeProjection projection;
  assert(PresentSimGlobeProject_Build(&map,matrix,rect,rect,1,&projection));
  PresentSimGlobeProjectedPoint point, sentinel;
  memset(&sentinel,0x5a,sizeof(sentinel)); point = sentinel;
  assert(!PresentSimGlobeProject_Point(NULL,0,0,0,0,&point));
  assert(!PresentSimGlobeProject_Point(&projection,NAN,0,0,0,&point));
  assert(!PresentSimGlobeProject_Point(&projection,0,INFINITY,0,0,&point));
  assert(!PresentSimGlobeProject_Point(&projection,0,0,NAN,0,&point));
  assert(!PresentSimGlobeProject_Point(&projection,0,0,0,NAN,&point));
  projection.matrix[15] = 0; /* On the camera plane. */
  assert(!PresentSimGlobeProject_Point(&projection,0,0,0,0,&point));
  assert(!memcmp(&point,&sentinel,sizeof(point)));
  PresentSimGlobeProjection saved = projection;
  assert(!PresentSimGlobeProject_Build(&map,matrix,(ArRenderRectI){0},rect,1,&projection));
  assert(!PresentSimGlobeProject_Build(&map,matrix,rect,rect,NAN,&projection));
  matrix[0] = NAN;
  assert(!PresentSimGlobeProject_Build(&map,matrix,rect,rect,1,&projection));
  assert(!memcmp(&saved,&projection,sizeof(saved)));

  const Scene3DCamera camera={-.75f,.35f,4.5f,.4f};
  Scene3D_BuildViewProjection(&camera,rect.w,rect.h,matrix);
  assert(PresentSimGlobeProject_Build(&map,matrix,rect,rect,1,&projection));
  assert(PresentSimGlobeProject_Point(&projection,256,256,2,0,&point));
  PresentSimGlobeBillboardAxes axes, untouched;
  assert(PresentSimGlobeProject_GroundBillboardAxes(&projection,256,256,&point,&axes));
  memset(&untouched,0x5a,sizeof(untouched)); axes=untouched;
  assert(!PresentSimGlobeProject_GroundBillboardAxes(NULL,256,256,&point,&axes));
  assert(!PresentSimGlobeProject_GroundBillboardAxes(&projection,NAN,256,&point,&axes));
  assert(!PresentSimGlobeProject_GroundBillboardAxes(&projection,256,INFINITY,&point,&axes));
  assert(!PresentSimGlobeProject_GroundBillboardAxes(&projection,256,256,NULL,&axes));
  point.clip.w=0;
  assert(!PresentSimGlobeProject_GroundBillboardAxes(&projection,256,256,&point,&axes));
  point.clip.w=1;
  projection.matrix[0]=projection.matrix[5]=1e20f;
  assert(!PresentSimGlobeProject_GroundBillboardAxes(&projection,256,256,&point,&axes));
  assert(!memcmp(&axes,&untouched,sizeof(axes)));
}

int main(void) { TestMotion(); TestPitchOnlyFacing(); TestInvalid(); puts("present_sim_globe_project_test: PASS"); }
