#ifndef AR_SIM_BACKGROUND_VOXEL_MODELS_H
#define AR_SIM_BACKGROUND_VOXEL_MODELS_H
/* SimBackgroundVoxelModel: builds each town object's voxel model (houses,
 * landmarks, trees, bridges) as faces in authentic town pixels, within the
 * face budget of the chosen detail level, plus conservative height bounds.
 * Phase: pure.
 * Tests: tests/sim_background_voxel_models_test.c */

#include <stdbool.h>
#include <stdint.h>

#include "sim/voxels/sim_background_voxel_quality.h"
#include "sim/voxels/sim_background_voxel_types.h"

enum {
  /* Ultra's absolute ceiling. Each lower quality level has a smaller enforced
   * budget returned by SimBackgroundVoxelModel_ObjectFaceBudget. */
  kSimBackgroundVoxelModelMaxFaces = 384,
  /* Surface-authored crowns no longer need hundreds of occupancy boxes.
   * The complete regional/style sweep peaks below 50; retain ample headroom
   * for new architecture. These are temporary build metadata, not GPU data. */
  kSimBackgroundVoxelModelMaxBoxes = 96,
};

typedef enum SimBackgroundVoxelMaterial {
  kSimVoxelMaterial_Wall,
  kSimVoxelMaterial_WallLight,
  kSimVoxelMaterial_Roof,
  kSimVoxelMaterial_RoofLight,
  kSimVoxelMaterial_Trim,
  kSimVoxelMaterial_Dark,
  kSimVoxelMaterial_Wood,
  kSimVoxelMaterial_Metal,
  kSimVoxelMaterial_Blade,
  kSimVoxelMaterial_Trunk,
  kSimVoxelMaterial_Leaves,
  kSimVoxelMaterial_LeavesLight,
  kSimVoxelMaterial_LeavesDark,
  kSimVoxelMaterial_Paving,
  kSimVoxelMaterial_Foundation,
  kSimVoxelMaterial_Gold,
  kSimVoxelMaterial_Glass,
  kSimVoxelMaterial_Snow,
  kSimVoxelMaterial_Contact,
  kSimVoxelMaterial_BladeStripe,
  kSimVoxelMaterial_Count,
} SimBackgroundVoxelMaterial;

typedef struct SimBackgroundVoxelModelPoint {
  float x, y, z;
} SimBackgroundVoxelModelPoint;

typedef struct SimBackgroundVoxelModelFace {
  SimBackgroundVoxelModelPoint points[4];
  uint8_t material;
  /* 255 is the material colour unchanged. Face direction and deliberate
   * stepped-model variation are expressed without allocating more materials. */
  uint8_t brightness;
  /* Geometry-derived concave-corner visibility. 255 means fully exposed;
   * lighting may combine this with its inexpensive height/contact gradient. */
  uint8_t occlusion[4];
  /* New continuous surfaces use outward winding, including undersides.
   * False preserves the older box/roof winding convention. Fits existing
   * struct padding, so cached faces do not grow. */
  bool outward_winding;
} SimBackgroundVoxelModelFace;

/* Light-projected canopy outlines for every tree family - conifer, broad
 * canopy, palm, burnable shrub and the ancient tree - in authored model XY.
 * Cast slopes include the presentation lean and light shear per unit of
 * authored height. The caller scales/translates the returned ground polygon.
 * Twenty-four samples per crown ring keep this independent of render LOD. */
enum { kSimBackgroundVoxelFoliageShadowMaxPoints = 64 };
bool SimBackgroundVoxelModel_UsesFoliageShadow(const SimBackgroundVoxelObject *object);
/* Equal keys share a crown profile; callers can reuse hulls at the same shear.
 * Keys never collide across families. */
uint16_t SimBackgroundVoxelModel_FoliageShadowVariant(const SimBackgroundVoxelObject *object);
int SimBackgroundVoxelModel_FoliageShadowHull(
    const SimBackgroundVoxelObject *object, float cast_x, float cast_y,
    SimBackgroundVoxelModelPoint out[kSimBackgroundVoxelFoliageShadowMaxPoints]);

/* Grounded rocks and bridges retain surface shading but cast no ground mask. */
bool SimBackgroundVoxelModel_CastsShadow(const SimBackgroundVoxelObject *object);

typedef struct SimBackgroundVoxelModelBox {
  float x0, y0, z0;
  float x1, y1, z1;
} SimBackgroundVoxelModelBox;

typedef struct SimBackgroundVoxelModel {
  uint16_t face_count;
  uint16_t authored_face_count;
  uint16_t face_budget;
  uint16_t box_count;
  bool overflow;
  float min_x, min_y, min_z;
  float max_x, max_y, max_z;
  SimBackgroundVoxelModelFace faces[kSimBackgroundVoxelModelMaxFaces];
  /* Retained for cacheable surface compilation and corner AO. The renderer
   * never submits these records directly. */
  SimBackgroundVoxelModelBox boxes[kSimBackgroundVoxelModelMaxBoxes];
} SimBackgroundVoxelModel;

typedef struct SimBackgroundVoxelModelBounds {
  float min_x, min_y, min_z;
  float max_x, max_y, max_z;
} SimBackgroundVoxelModelBounds;

/* Shared ground support for model rendering and the audit sheet. Separate
 * masses keep courtyard ground exposed, including on sloping terrain.
 * The caller supplies room for kSimBackgroundVoxelModelMaxContacts records. */
enum { kSimBackgroundVoxelModelMaxContacts = 12 };
typedef struct SimBackgroundVoxelModelContact {
  float x0, y0, x1, y1;
} SimBackgroundVoxelModelContact;

int SimBackgroundVoxelModel_Contacts(const SimBackgroundVoxelObject *object,
                                     SimBackgroundVoxelModelContact *out);

uint16_t SimBackgroundVoxelModel_FaceBudget(
    SimBackgroundVoxelDetail detail);

/* The two multi-part town landmarks get an object-specific detail allowance;
 * repeated buildings keep FaceBudget's limits. Ultra's ceiling is unchanged. */
uint16_t SimBackgroundVoxelModel_ObjectFaceBudget(
    const SimBackgroundVoxelObject *object, SimBackgroundVoxelDetail detail);

/* Builds an object-local model in authentic town pixels. X and Y cover the
 * ground footprint; Z is height. This module deliberately knows nothing about
 * SDL, the camera, or GPU resources. */
void SimBackgroundVoxelModel_Build(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelModel *out);

/* Styling is independent from density: detail owns face budgets and stepped
 * resolution, while style selects optional authored architecture. */
void SimBackgroundVoxelModel_BuildStyled(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail detail,
    SimBackgroundVoxelStyle style,
    SimBackgroundVoxelModel *out);

/* Conservative height in authentic town pixels, across Low through the
 * requested maximum detail and all three windmill poses. Other objects keep
 * their captured construction phase. Uses the same authored geometry before
 * surface optimization; no render-cache or backend side effects. Intended
 * for retained scene bounds, not per-frame model compilation. Null/unknown
 * objects return zero; invalid detail/style use BuildStyled's defaults. */
float SimBackgroundVoxelModel_HeightBound(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail maximum_detail,
    SimBackgroundVoxelStyle style);

/* Same retained, all-LOD/all-windmill-pose envelope as HeightBound, including
 * roof/crown/blade overhangs. False with zero bounds for missing geometry. */
bool SimBackgroundVoxelModel_MeasureBounds(
    const SimBackgroundVoxelObject *object,
    SimBackgroundVoxelDetail maximum_detail,
    SimBackgroundVoxelStyle style,
    SimBackgroundVoxelModelBounds *out);

#endif  /* AR_SIM_BACKGROUND_VOXEL_MODELS_H */
