#ifndef AR_ACTION_VOXEL_H
#define AR_ACTION_VOXEL_H
#include "action/action_room_scene.h"
#include "diorama/diorama_layer_order.h"
#include <math.h>
typedef struct ActionVoxelGeometry ActionVoxelGeometry;
const DioramaVoxelEdits *ActionVoxel_Edits(const ActionVoxelGeometry *geometry);
bool ActionVoxel_HasObjectEdits(const ActionVoxelGeometry *geometry);
const DioramaVoxelObject *ActionVoxel_ObjectEdit(const ActionVoxelGeometry *geometry,
    uint16_t source, uint16_t parent, uint16_t animation);
/* Compiled sparse authored cells own their donor art, so scrolling a donor out
 * of view cannot change a floor or side. RGB555 bit15 marks opaque ink. */
typedef struct VoxelAuthoredCell {
  int16_t x, y; /* Native pixel origin, not cell coordinates. */
  uint8_t priority[4];
  bool surface;
  bool native_front_hidden; /* Visibility-only edit keeps semantic caps/art. */
  bool native_transform; /* Affine-only record keeps compiled semantic faces. */
  bool native_transform_requested; /* Includes identity metadata; still validates the group. */
  bool native_front_art; /* Automatic front keeps native filtering/shading. */
  uint16_t native_rear_rows[16]; /* Owned native rear copies; excludes plants/fills. */
  float front[4], back[4], width, offset_x;
  uint8_t faces, shape;
  uint8_t automatic_art; /* Bit per art index: caps extend opaque contour ink. */
  int16_t shape_y;
  uint16_t shape_h;
  float front_end[4], back_end[4];
  DioramaVoxelBend bend;
  DioramaVoxelTransform transform;
  /* Deduplicated room-owned images: front/top/legacy side/back/left/right/bottom.
   * Pointers are frozen after compilation; IDs identify bounded atlas slots. */
  uint32_t art_id[7];
  const uint16_t *pixels[7];
  uint32_t contour_start;
  uint16_t contour_count;
} VoxelAuthoredCell;
typedef struct VoxelAuthoredContour { uint8_t x,y,length,side; } VoxelAuthoredContour;
typedef struct VoxelCellGeometry {
  float front[4], back[4], width, offset_x;
  bool authored;
  uint8_t faces, shape;
  int16_t shape_y;
  uint16_t shape_h;
  float front_end, back_end;
  DioramaVoxelBend bend;
  DioramaVoxelTransform transform;
} VoxelCellGeometry;
enum { kVoxelAuthoredCellMax = 32768, kVoxelAuthoredArtMax = 8192,
       kVoxelAuthoredContourMax = 262144,
       kVoxelAuthoredVisibleArtMax = 4096 };
uint16_t *ActionVoxel_RecordOwners(const DioramaVoxelEdits *,int x16,int y16,int width16,int height16);
const VoxelAuthoredCell *ActionVoxel_AuthoredCells(const ActionVoxelGeometry *, unsigned *count);
const VoxelAuthoredCell *ActionVoxel_AuthoredCellAt(const ActionVoxelGeometry *, int x16, int y16);
const uint16_t *ActionVoxel_AuthoredArt(const ActionVoxelGeometry *, unsigned id);
unsigned ActionVoxel_AuthoredArtCount(const ActionVoxelGeometry *);
const uint16_t *ActionVoxel_AuthoredCapArt(const ActionVoxelGeometry *,unsigned *count);
const VoxelAuthoredContour *ActionVoxel_AuthoredContours(const ActionVoxelGeometry *,
    const VoxelAuthoredCell *,unsigned *count);
bool ActionVoxel_AuthoredContourExposed(const ActionVoxelGeometry *,const VoxelAuthoredCell *,
    int x,int y,unsigned side);
/* Shared native-Y profile evaluation: adjacent cells share the same domain,
 * including endpoint clipping. No camera state enters the shape. */
void ActionVoxel_AuthoredDepths(const VoxelAuthoredCell *, unsigned quadrant,
    float world_y, float *front, float *back);
/* Add these offsets after ordinary width/offset/depth shaping. Inputs remain
 * original native source coordinates; use the same result on both depths. */
void ActionVoxel_BendOffset(const DioramaVoxelBend *, float source_x,
    float source_y, float *dx, float *dy, float *dz);
void ActionVoxel_AuthoredBend(const VoxelAuthoredCell *, float source_x,
    float source_y, float *dx, float *dy, float *dz);
bool ActionVoxel_TransformValid(const DioramaVoxelTransform *);
/* Transform a completed geometry point around its shared world-space pivot.
 * NULL / absent (zero scale) metadata is identity; source UVs never change. */
void ActionVoxel_TransformPoint(const DioramaVoxelTransform *, float x, float y,
    float z, float *out_x, float *out_y, float *out_z);
const char *ActionVoxel_BuildError(const ActionVoxelGeometry *);
bool ActionVoxel_CellGeometry(const ActionVoxelGeometry *, int x16, int y16, VoxelCellGeometry *);
bool ActionVoxel_AuthoredSurfaceAt(const ActionVoxelGeometry *, int world_x, int world_y);
bool ActionVoxel_HasHiddenNativeFronts(const ActionVoxelGeometry *);
bool ActionVoxel_HasNativeTransforms(const ActionVoxelGeometry *);
bool ActionVoxel_HasTransformedBacks(const ActionVoxelGeometry *);
/* Selection follows compiled native material roles, before authored geometry
 * overrides. Coordinates/bounds use16px cells; oversized queries do not return
 * a partial usable group. */
typedef enum VoxelSemanticKind {
  kVoxelSemantic_None, kVoxelSemantic_Rear, kVoxelSemantic_Foliage,
  kVoxelSemantic_Scenery, kVoxelSemantic_Platform, kVoxelSemantic_Water,
  kVoxelSemantic_Hazard, kVoxelSemantic_Gate, kVoxelSemantic_Column,
  kVoxelSemantic_Branch, kVoxelSemantic_TreeWood, kVoxelSemantic_Stump,
  kVoxelSemantic_Authored
} VoxelSemanticKind;
typedef enum ActionVoxelGroupStatus {
  kActionVoxelGroup_Invalid, kActionVoxelGroup_Empty,
  kActionVoxelGroup_TooLarge, kActionVoxelGroup_Complete,
  kActionVoxelGroup_Ambiguous
} ActionVoxelGroupStatus;
typedef struct VoxelGroupCell { int16_t x,y; } VoxelGroupCell;
typedef struct VoxelSemanticGroup {
  VoxelSemanticKind kind;
  const char *label;
  int x,y,width,height;
  unsigned count;
} VoxelSemanticGroup;
enum { kVoxelSemanticGroupMaxCells=4096,kVoxelSemanticPlatformMaxSpan=64 };
ActionVoxelGroupStatus ActionVoxel_SemanticGroup(const ActionVoxelGeometry *,
    int x16,int y16,VoxelSemanticGroup *,VoxelGroupCell *,unsigned capacity);
/* NULL means a native semantic transform is supported; otherwise return a
 * stable user-readable reason. No geometry or authored state is changed. */
const char *ActionVoxel_TransformUnsupportedReason(const ActionVoxelGeometry *,int x16,int y16);
/* Resolved reset / visibility-only records retain live native artwork. This
 * predicate must follow ownership resolution, so an older donor stays hidden. */
bool ActionVoxel_CellOwnsArtwork(const DioramaVoxelCell *);
bool ActionVoxel_CellUsesNativeTransform(const DioramaVoxelCell *);
/* Exact identity preserves native ownership for default faces/front-only hide. */
bool ActionVoxel_CellKeepsNativeIdentity(const DioramaVoxelCell *);
uint32_t ActionVoxel_AuthoredColor(unsigned rgb555, const ActionRoomSceneFrameState *frame);
/* A profile owns material interpretation and presentation policy. It is
 * separate from room identity, native collision and source sprite priority. */
typedef enum ActionVoxelProfile {
  kActionVoxelProfile_None,
  kActionVoxelProfile_Castle,
  kActionVoxelProfile_FillmoreForest,
  kActionVoxelProfile_FillmoreCave,
  kActionVoxelProfile_FillmoreTemple,
  kActionVoxelProfile_FillmoreTower,
  kActionVoxelProfile_BloodpoolMarsh,
  kActionVoxelProfile_KassandoraDesert,
  kActionVoxelProfile_KassandoraTomb,
  kActionVoxelProfile_AitosMountains,
  kActionVoxelProfile_AitosCaverns,
  kActionVoxelProfile_MarahnaJungle,
  kActionVoxelProfile_MarahnaTemple,
  kActionVoxelProfile_NorthwallSnow,
  kActionVoxelProfile_NorthwallIce,
  kActionVoxelProfile_DeathHeim
} ActionVoxelProfile;
ActionVoxelProfile ActionVoxel_ProfileForRoom(unsigned group, unsigned room);
ActionVoxelProfile ActionVoxel_Profile(const ActionVoxelGeometry *geometry);
/* The hub's BG2 is two distinct scene objects: faces/eyes are promoted to
 * the skybox far band while the scrolling water remains on its own plane.
 * Modeling BG1's skull island must preserve that reviewed source partition. */
static inline bool ActionVoxel_OwnsBackgroundBands(ActionVoxelProfile profile,
                                                   unsigned room, unsigned bg) {
  return profile != kActionVoxelProfile_None &&
      !(profile == kActionVoxelProfile_DeathHeim && room == 1 && bg == 1);
}
/* The additive jungle water must cover the same tilted viewport as the
 * subscreen scenery. Only request guards where the caller has verified a
 * world-map source; this never widens gameplay or invents missing tiles. */
static inline bool ActionVoxel_BackgroundApron(ActionVoxelProfile profile,unsigned bg) {
  return profile==kActionVoxelProfile_MarahnaJungle && bg==1;
}
/* Reviewed pools use a second native map as their geometry source. This
 * governs editor compilation, live publication and authored-edit keys alike. */
static inline bool ActionVoxel_BackgroundWater(ActionVoxelProfile profile) {
  return profile==kActionVoxelProfile_FillmoreCave || profile==kActionVoxelProfile_NorthwallIce;
}
/* The tower's flat-layer facade bias crops its modeled walking deck. This
 * policy is available before geometry publication for native camera queries.
 * Pass None outside Voxel mode; saved settings are never rewritten. */
static inline int ActionVoxel_VerticalFraming(ActionVoxelProfile profile, int authored) {
  return profile == kActionVoxelProfile_FillmoreTower ? 0 : authored;
}
typedef struct ActionVoxelSettings {
  float depth, play_recess;
} ActionVoxelSettings;
/* Host owns one instance per room. Build is transactional: failure leaves the
 * published geometry usable. Rebuild for map, definitions, characters, terrain
 * stamps/pixel edits or palette changes; camera/light changes need no rebuild. */
ActionVoxelGeometry *ActionVoxel_Create(void);
/* Retained packets and display history share immutable geometry. Build may
 * only update an unpublished, exclusively owned object. */
const ActionVoxelGeometry *ActionVoxel_Retain(const ActionVoxelGeometry *geometry);
void ActionVoxel_Release(const ActionVoxelGeometry *geometry);
/* Stable for a published object; changes on successful rebuild. Presentation
 * history uses this value without retaining a pointer to an expired frame. */
uint64_t ActionVoxel_Revision(const ActionVoxelGeometry *geometry);
static inline bool ActionVoxel_Room(unsigned group, unsigned room) {
  return ActionVoxel_ProfileForRoom(group, room) != kActionVoxelProfile_None;
}
static inline ActionVoxelSettings ActionVoxel_DefaultSettings(void) {
  return (ActionVoxelSettings){.depth = .24f, .play_recess = .08f};
}
ActionVoxelSettings ActionVoxel_SettingsForRoom(unsigned group, unsigned room);
/* Bounds include authored scenery beyond the native map. Storage and signed
 * contour coordinates are bounded independently of a specific room shape. */
bool ActionVoxel_ValidBounds(int x0, int y0, int x1, int y1);
enum {
  kVoxelMaxEdges = 16384, /* Whole-room silhouettes, including reviewed matte cutouts. */
  kVoxelMaxVisibleFaces = 8192, /* Viewport draw budget stays independent of map size. */
  kVoxelMaxRearTrims = 128,
  kVoxelMaxGates = 16,
  kVoxelGateDepthSlices = 3,
  kVoxelMaxPatches = 512,
  kVoxelMaxWaterSurfaces = 128,
  kVoxelMaxWaterfalls = 32,
  kVoxelMaxTrunkCurves = 64,
  kVoxelMaxCastleColumns = 128
};
enum {
  kVoxelKindMask = 7,
  kVoxelHighPriority = 8,
  kVoxelRearTrim = 16,
  kVoxelColliding = 32,
  kVoxelTexturedTop = 64,
  kVoxelRearCopy = 128, /* Reviewed mixed art retains a complete rear backing. */
  kVoxelSolid = 1,
  kVoxelThin = 2,
  kVoxelFoliage = 3,
  kVoxelForeground = 4,
  kVoxelGate = 5,
  kVoxelFlatParapet = 6,
  kVoxelRearOnly = 7 /* Border vegetation stays behind the open walking deck. */
};
typedef struct VoxelEdge {
  short x, y, length;
  unsigned char side, near_rank, far_rank, high_priority, rear_trim, colliding, textured_top;
  unsigned char soft_surface;
  unsigned char rear_mounted, neighbor_rear_mounted;
  unsigned char thin_trim, neighbor_thin_trim;
  /* Native-pixel rise of the rear fold: the complete top edge for side0,
   * or the native top/bottom endpoints for a vertical side. Contact remains
   * fixed; only the surface behind it rises to the rear vegetation border. */
  unsigned char rear_fold_top, rear_fold_bottom;
  /* Optional opaque native-art square tiled over an existing top cap.
   * World coordinates select the source only; zero size keeps edge sampling. */
  short top_sample_x, top_sample_y;
  unsigned char top_sample_size;
  uint16_t top_owned_art; /* One-based immutable donor; zero samples capture. */
  uint16_t source_owned_art[2]; /* One-based front/rear native source patches. */
  int16_t source_x[2], source_y[2]; /* World origin of each 16px source patch. */
  int16_t fold_origin_y;
  uint16_t fold_length; /* Original vertical span after native edge splitting. */
  unsigned char trunk_curve; /* One-based semantic lower-trunk owner; zero is unchanged. */
  unsigned char column_curve, neighbor_column_curve;
} VoxelEdge;
/* A lower tree shaft bends from its unchanged branch contact at top_y to the
 * rear vegetation at base_y. Query the owning cell once, then evaluate that
 * same curve for every vertex, including vertices on a neighbouring cell. */
typedef struct VoxelTrunkCurve { short top_y, base_y; } VoxelTrunkCurve;
const VoxelTrunkCurve *ActionVoxel_TrunkCurveForCell(const ActionVoxelGeometry *c,int tile_x,int tile_y);
const VoxelTrunkCurve *ActionVoxel_TrunkCurveForEdge(const ActionVoxelGeometry *c,const VoxelEdge *edge);
float ActionVoxel_TrunkFrontDepth(const ActionVoxelGeometry *c,const VoxelTrunkCurve *curve,float world_y);
float ActionVoxel_TrunkBackDepth(const ActionVoxelGeometry *c,const VoxelTrunkCurve *curve,float world_y);
/* Noncolliding shafts taper immediately below a supported capital, then
 * follow the rear wall so the next landing remains accessible from its side. */
bool ActionVoxel_HasColumns(const ActionVoxelGeometry *c);
const VoxelTrunkCurve *ActionVoxel_ColumnCurveForCell(const ActionVoxelGeometry *c,int tile_x,int tile_y);
const VoxelTrunkCurve *ActionVoxel_ColumnCurveForEdge(const ActionVoxelGeometry *c,const VoxelEdge *edge);
float ActionVoxel_ColumnFrontDepth(const ActionVoxelGeometry *c,const VoxelTrunkCurve *curve,float world_y);
/* A painted ground strip folds onto the back of the deck without moving the
 * native walking contact. Invalid depth inputs leave the original geometry. */
static inline float ActionVoxel_RearFoldOffset(unsigned pixels,float local_depth,
                                               float contact_depth,float rear_depth) {
  if(!pixels || !isfinite(local_depth) || !isfinite(contact_depth) || !isfinite(rear_depth) ||
      !(rear_depth>contact_depth) || local_depth<=contact_depth)return 0;
  if(local_depth>=rear_depth)return (float)pixels;
  return pixels*(local_depth-contact_depth)/(rear_depth-contact_depth);
}
typedef enum VoxelEdgePass {
  kVoxelEdgeNative,
  kVoxelEdgeRearTrim,
  kVoxelEdgeHighDeckBehind
} VoxelEdgePass;
typedef struct VoxelEdgeSpan {
  float near_depth, far_depth;
} VoxelEdgeSpan;
/* Source priority selects the texture; generated decks separately choose
 * their position in the actor draw order. Native front pixels stay intact. */
bool ActionVoxel_HasHighDecks(const ActionVoxelGeometry *c);
bool ActionVoxel_ResolveEdgeSpanAtY(const ActionVoxelGeometry *c,const VoxelEdge *edge,
    VoxelEdgePass pass,float near_depth,float far_depth,float world_y,VoxelEdgeSpan *out);
bool ActionVoxel_ResolveEdgeSpan(const ActionVoxelGeometry *c, const VoxelEdge *edge,
                                     VoxelEdgePass pass, float near_depth, float far_depth,
                                     VoxelEdgeSpan *out);
/* A room can contain both front door trim and rear wall columns. */
float ActionVoxel_EdgeRecess(const ActionVoxelGeometry *c, const VoxelEdge *edge);
typedef struct VoxelRearTrim {
  short x, y, width;
} VoxelRearTrim;
const VoxelRearTrim *ActionVoxel_RearTrims(const ActionVoxelGeometry *c, unsigned *count);
/* One continuous native iron strip. Its transparent art supplies the side
 * panel; the adjoining marble is never part of this surface. */
typedef struct VoxelGate {
  short x, y, height;
  unsigned char high_priority, to_right;
  unsigned char width, centered; /* Width 0 retains the original 8px strip. */
} VoxelGate;
const VoxelGate *ActionVoxel_Gates(const ActionVoxelGeometry *c, unsigned *count);
/* Native-art caps and crossed profiles. Rear grass stays behind the path. */
typedef enum VoxelPatchKind {
  kVoxelPatch_Thorns, kVoxelPatch_BranchRear, kVoxelPatch_SideProfile, kVoxelPatch_Spikes,
  kVoxelPatch_RearGrass
} VoxelPatchKind;
typedef struct VoxelPatch {
  short x, y, width, height;
  unsigned char high_priority, kind;
} VoxelPatch;
const VoxelPatch *ActionVoxel_Patches(const ActionVoxelGeometry *c, unsigned *count);
/* Native pool surface runs, independent of environmental-effects settings. */
typedef struct VoxelWaterSurface {
  short x, y, width;
  unsigned char high_priority, background; /* Zero-based native source. */
  unsigned char sample_y, sample_height; /* Relative to the surface's world y. */
  unsigned char opaque; /* Lava and full-add water keep native source opacity. */
} VoxelWaterSurface;
const VoxelWaterSurface *ActionVoxel_WaterSurfaces(const ActionVoxelGeometry *c, unsigned *count);
typedef struct VoxelWaterfall {
  short x, y, width, height;
  unsigned char high_priority;
} VoxelWaterfall;
const VoxelWaterfall *ActionVoxel_Waterfalls(const ActionVoxelGeometry *c, unsigned *count);
/* World-anchored continuous recession of reviewed rear-wall rock details. */
bool ActionVoxel_HasRearRelief(const ActionVoxelGeometry *c);
float ActionVoxel_RearRelief(const ActionVoxelGeometry *c, float world_x, float world_y);
float ActionVoxel_RearFaceDepth(const ActionVoxelGeometry *c);
float ActionVoxel_Depth(const ActionVoxelGeometry *c);
float ActionVoxel_PlayRecess(const ActionVoxelGeometry *c);
/* Material relief: thin forest scenery follows actor contact; temple columns
 * project from the rear wall. Lookup uses native 8px world cells. */
float ActionVoxel_SceneryRecess(const ActionVoxelGeometry *c);
bool ActionVoxel_RearMountedScenery(const ActionVoxelGeometry *c);
float ActionVoxel_FrontRecess(const ActionVoxelGeometry *c, int tile_x, int tile_y);
/* Reviewed low-priority wood sharing a water character. Bit 7 is the left
 * pixel; selected native pixels use SceneryRecess and all others stay at 0.
 * Borrowed immutable rows, or NULL. High-priority artwork never has a mask.
 * The room-wide bound also limits the front mesh's added pixel partitions. */
enum { kVoxelMaxPixelRecessCells = 64 };
const uint8_t *ActionVoxel_PixelRecessRows(const ActionVoxelGeometry *c, int tile_x, int tile_y);
float ActionVoxel_TrimBackDepth(const ActionVoxelGeometry *c);
float ActionVoxel_TrimDepth(const ActionVoxelGeometry *c);
float ActionVoxel_BranchDepth(const ActionVoxelGeometry *c);
float ActionVoxel_BranchBackDepth(const ActionVoxelGeometry *c);
bool ActionVoxel_Build(ActionVoxelGeometry *c, const ActionRoomScene *scene,
                           const DioramaRoomOverride *terrain, ActionVoxelSettings settings);
/* Optional current native 8x8 words, row-major over the finite BG1 map.
 * Live door scripts may patch the resident ring without editing the map.
 * Authored terrain stamps retain precedence; collision comes from the map. */
bool ActionVoxel_BuildWithTiles(ActionVoxelGeometry *c, const ActionRoomScene *scene,
                                 const DioramaRoomOverride *terrain, ActionVoxelSettings settings,
                                 const uint16_t *tiles, size_t tile_pitch);
/* Exact native animation frames may share geometry when the material policy
 * proves every phase is presentation-only. The reference must retain all ROM
 * phases. Recheck the proof after any other geometry input changes; normalize
 * only the compiler's private character copy, never live VRAM/capture data. */
bool ActionVoxel_AnimationMatches(const ActionRoomScene *reference,
                                   const uint8_t characters[kActionRoomSceneCharacterBytes],
                                   size_t *offset, size_t *bytes);
/* These policies do not bake neighbouring rear artwork into generated faces.
 * Every native phase still has to pass the material/topology proof below. */
static inline bool ActionVoxel_AnimationCacheCandidate(ActionVoxelProfile profile) {
  return profile==kActionVoxelProfile_KassandoraDesert ||
      profile==kActionVoxelProfile_KassandoraTomb || profile==kActionVoxelProfile_AitosCaverns ||
      profile==kActionVoxelProfile_MarahnaTemple || profile==kActionVoxelProfile_NorthwallSnow;
}
bool ActionVoxel_AnimationIsPresentationOnly(const ActionRoomScene *scene,
                                   const ActionRoomScene *reference,
                                   const DioramaRoomOverride *terrain,
                                   const uint16_t *tiles, size_t tile_pitch);
bool ActionVoxel_Active(const ActionVoxelGeometry *c, unsigned group, unsigned room);
/* Reviewed BG1 matte pixels, compiled in world coordinates. Apply only while
 * this geometry owns Voxel presentation; native collision remains unchanged.
 * CutoutRows ORs into eight capture masks (bit 7 leftmost), returning whether
 * any pixels were added. Authored black paint overrides automatic cutouts. */
bool ActionVoxel_HasCutouts(const ActionVoxelGeometry *c);
bool ActionVoxel_CutoutRows(const ActionVoxelGeometry *c, int tile_x, int tile_y,
                            uint8_t rows[8]);
/* Both outputs use output_pitch; native high-priority pixels stay separate.
 * Colour math comes from the captured frame, including native room fades. */
bool ActionVoxel_Split(const ActionVoxelGeometry *c, int ox, int oy, int width, int height,
                        size_t output_pitch, const ActionRoomSceneFrameState *frame,
                        const uint32_t *low, size_t low_pitch,
                        uint32_t *rear, uint32_t *foreground);
const VoxelEdge *ActionVoxel_Edges(const ActionVoxelGeometry *c, unsigned *count);
const uint16_t *ActionVoxel_EdgesForPass(const ActionVoxelGeometry *c, VoxelEdgePass pass,
                                             bool high, unsigned *count);
#endif
