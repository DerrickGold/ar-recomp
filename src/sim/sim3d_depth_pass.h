#ifndef SIM3D_DEPTH_PASS_H
#define SIM3D_DEPTH_PASS_H

#include <stdbool.h>
#include <stddef.h>

#include "render/render_device.h"

typedef enum Sim3DDepthPassLayer {
  /* Invisible terrain geometry is submitted first and writes only depth.  The
   * textured town ground remains in the ordinary color pass, while this layer lets
   * the same hills and cliff skirts reject solid models hidden behind them. */
  kSim3DDepthPass_DepthOccluder,
  kSim3DDepthPass_Solid,
  kSim3DDepthPass_Mountain,
  /* Samples the accumulated screen-space shadow mask on the exact terrain
   * top mesh. It tests against opaque depth without writing it, which clips
   * shadows at ridges, cliff lips, buildings and bridge geometry. */
  kSim3DDepthPass_ShadowReceiver,
  /* Transparent world effects are submitted after all opaque geometry. They
   * still test against the shared depth target, but use a no-depth-write
   * pipeline so smoke/glow cannot punch transparent holes through mountains. */
  kSim3DDepthPass_Effect,
  /* Colored world surfaces share opaque depth with authored town models.
   * Blur/haze overlays test that surface depth without replacing it. These
   * values are appended to preserve existing project-private layer IDs. */
  kSim3DDepthPass_Ground,
  kSim3DDepthPass_GroundBlur,
  kSim3DDepthPass_GroundHaze,
  /* World weather shares one repeating atlas. Shadows sample the exact
   * ground mesh; cloud bodies test opaque depth without writing it. */
  kSim3DDepthPass_CloudShadow,
  kSim3DDepthPass_Cloud,
  /* Independent globe cutouts can coexist with the active town's atlas. */
  kSim3DDepthPass_WorldMountain,
  /* Sorted translucent density slices; independent atlas, no depth writes. */
  kSim3DDepthPass_VolumeCloud,
  kSim3DDepthPassLayerCount,
} Sim3DDepthPassLayer;

typedef struct Sim3DDepthVertex {
  float x, y;
  float depth;
  ArRenderColorF color;
  ArRenderPointF uv;
} Sim3DDepthVertex;

typedef struct Sim3DDepthMesh Sim3DDepthMesh;
typedef struct Sim3DDepthPosition { float x, y, depth; } Sim3DDepthPosition;

/* Optional retained screen-space quad geometry. The caller owns the opaque
 * mesh, all calls belong to the presentation thread, and Reset invalidates
 * its GPU contents without freeing the caller's handle. Ready also checks
 * the current viewport. Republish after camera/geometry changes or !Ready.
 * Updates copy positions; appends copy UVs/colors, never borrowing arrays.
 *
 * Create/Update/Append require an active pass. Update must precede the first
 * append of this mesh in that pass. Only transparent layers accept samples;
 * ordinary and retained appends cannot mix within a material in one pass.
 * Sample order is preserved. Destroying an already queued mesh aborts that
 * pass rather than leaving dangling commands. Create/Update failure allows
 * an ordinary-geometry fallback before queuing samples. Submit retains the
 * ordinary path's failure contract for final GPU allocation/transfer errors. */
Sim3DDepthMesh *Sim3DDepthPass_CreateMesh(void);
bool Sim3DDepthPass_MeshReady(const Sim3DDepthMesh *mesh);
bool Sim3DDepthPass_UpdateMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthPosition *positions, size_t quad_count);
bool Sim3DDepthPass_AppendMeshSample(Sim3DDepthPassLayer layer,
    Sim3DDepthMesh *mesh, const ArRenderPointF *uv, size_t quad_count,
    ArRenderColorF color);
void Sim3DDepthPass_DestroyMesh(Sim3DDepthMesh *mesh);

/* Retained, already projected opaque-material geometry, with colors and UVs.
 * Update copies the same values as AppendQuads; Ready includes the viewport.
 * Range appends can interleave with ordinary appends in exact per-material call
 * order (including equal-depth/alpha behavior). Range/availability rejection
 * queues nothing, so that range can use ordinary geometry instead. Updates
 * after any range is queued are rejected. All other ownership/reset rules
 * above apply. Supports Solid, Ground, Mountain, WorldMountain, DepthOccluder
 * and ShadowReceiver, each with its original material/depth policy. Other
 * layers keep their existing sample contracts. No new shader/projection policy. */
/* Opaque geometry handles/ranges have separate bounded budgets: exhausting
 * this optional cache cannot consume transparent-effect handle/sample slots. */
Sim3DDepthMesh *Sim3DDepthPass_CreateGeometryMesh(void);
bool Sim3DDepthPass_UpdateGeometryMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthVertex *vertices, size_t quad_count);
bool Sim3DDepthPass_AppendGeometryMeshRange(Sim3DDepthPassLayer layer, Sim3DDepthMesh *mesh,
    size_t first_quad, size_t quad_count);
bool Sim3DDepthPass_AppendGeometryMesh(Sim3DDepthPassLayer layer, Sim3DDepthMesh *mesh);
/* Copy the complete ordinary material batch collected so far, without
 * changing queued draws. This is a CPU-owned staging copy, NOT GPU readback.
 * Rejects empty, sampled, failed or over-budget batches and queued handles.
 * The caller owns the cache key and must capture only a complete material. */
bool Sim3DDepthPass_CaptureGeometryMesh(Sim3DDepthPassLayer layer, Sim3DDepthMesh *mesh);
typedef struct Sim3DDepthGeometryRange {
  Sim3DDepthPassLayer layer;
  size_t first_quad, quad_count;
} Sim3DDepthGeometryRange;
/* Copy several complete ordinary layers into ONE owned mesh. Empty layers
 * produce empty ranges; an entirely empty set rejects. Layers must be unique,
 * supported, and unsampled. Neither draws nor output ranges change on failure.
 * The caller certifies source revisions and owns the returned range values. */
bool Sim3DDepthPass_CaptureGeometryLayers(Sim3DDepthMesh *mesh,
    const Sim3DDepthPassLayer *layers, size_t layer_count,
    Sim3DDepthGeometryRange *ranges);
/* Atomic optional append: validate all ranges and reserve the complete draw
 * budget before queuing anything. Rejection permits an ordinary full-batch
 * fallback without drawing any layer twice. Empty ranges are skipped. */
bool Sim3DDepthPass_AppendGeometryRanges(Sim3DDepthMesh *mesh,
    const Sim3DDepthGeometryRange *ranges, size_t range_count);

/* Experimental, untextured model-space solids. No shipping view opts in yet.
 * Unlike screen-space meshes, these survive viewport/camera changes. Geometry
 * and colors are copied on Update; Append copies a column-major transform
 * (-W..W clip Z, as Scene3D math uses). Solid depth/blend rules are unchanged.
 * Only this mesh kind accepts these calls. Reset/destroy/queued-update rules
 * apply. Appends interleave with ordinary/retained Solid ranges in call order.
 *
 * The adapter conservatively tests the retained AABB against all six planes.
 * A clipped/behind-eye/nonfinite transform is rejected without queuing work:
 * use ordinary geometry for those cases. This deliberately preserves affine
 * attributes until clipped-edge equivalence is established. No pixel-clean
 * snapping or per-model facing policy is inferred by the platform layer. */
typedef struct Sim3DDepthModelVertex {
  float position[3];
  ArRenderColorF color;
} Sim3DDepthModelVertex;
Sim3DDepthMesh *Sim3DDepthPass_CreateModelMesh(void);
/* Separate experimental policy: retain homogeneous W and let the GPU clip
 * partially visible or behind-eye primitives before perspective division.
 * Uses screen-linear color interpolation; clipped-edge shading/roundoff need
 * not reproduce the legacy CPU clipper bit-for-bit. Same Update/Append and
 * bounded opaque ownership/order rules. No shipping view opts in yet. Fully
 * invisible objects should still be coarsely culled by the scene owner. */
Sim3DDepthMesh *Sim3DDepthPass_CreateHardwareClippedModelMesh(void);
bool Sim3DDepthPass_UpdateModelMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthModelVertex *vertices, size_t quad_count);
bool Sim3DDepthPass_AppendModelMesh(Sim3DDepthMesh *mesh,
    const float matrix[16]);

/* Optional camera-independent radial solids. The scene supplies chart-space
 * unit normals, an anchor elevation in source units, and an additional rise
 * in world units. The adapter rotates each normal, computes
 *   radius = sphere_radius + reference_height * height_scale
 *   rise = (anchor - reference_height) * height_scale + extra_rise
 *   world = radius * (normal - {0,0,1}) + normal * rise
 * and projects with matrix, retaining homogeneous W for hardware clipping.
 * No map, town, LOD, lighting, animation clock or chart policy enters the
 * adapter. Colors are screen-linear, as with hardware-clipped model meshes.
 *
 * variant=0 is always visible; other integer tags (1..65535) draw only when
 * matching the transform's variant. All four corners must have the same tag.
 * This permits bounded precompiled poses in ONE ordered batch, without a draw
 * per object. Unit normals tolerate 0.001 rounding error. Update/Append copy
 * all inputs, obey the opaque budgets/ordering/reset rules, and reject invalid
 * or overflowing data without queuing any work. Ready is viewport-independent.
 * The caller still owns whole-object culling and source revision validation. */
typedef struct Sim3DDepthRadialVertex {
  float normal[3];
  float elevation[2]; /* anchor in source units, extra rise in world units */
  ArRenderColorF color;
  float variant;
} Sim3DDepthRadialVertex;
typedef struct Sim3DDepthRadialTransform {
  float matrix[16];
  float basis[3][3]; /* rows: right, up, outward */
  float sphere_radius, reference_height, height_scale;
  unsigned variant;
} Sim3DDepthRadialTransform;
Sim3DDepthMesh *Sim3DDepthPass_CreateRadialMesh(void);
bool Sim3DDepthPass_UpdateRadialMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthRadialVertex *vertices, size_t quad_count);
typedef struct Sim3DDepthMeshRange { size_t first_quad, quad_count; } Sim3DDepthMeshRange;
/* Optional ordered index selection within the published source mesh. Copies
 * ranges before returning; never reuploads source vertices. Total selected
 * quads must fit the same bounded vertex budget. UpdateRadialMesh restores
 * the default whole-mesh selection. Rejected selection updates leave the old
 * selection intact; queued updates are rejected. Reset invalidates both. */
bool Sim3DDepthPass_SelectRadialMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthMeshRange *ranges, size_t range_count);
bool Sim3DDepthPass_AppendRadialMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthRadialTransform *transform);

/* Optional spherical atlas mapping on the GPU. Positions are already clipped
 * screen-space quads, exactly as for UpdateMesh. Normals describe the FOUR
 * original corners in texture-sphere space. triangle=0 selects those corners
 * directly; 1/2 reconstructs UVs from original triangle {0,1,2}/{0,2,3}, using
 * the two nonzero-corner clipping weights for each output vertex. Wrapping
 * and latitude clamping happen BEFORE this difference-form interpolation.
 * No camera, game state, clock, atlas policy or native GPU handle is borrowed.
 *
 * Spherical meshes use only UpdateSphericalMesh/AppendSphericalSample; ordinary
 * meshes use UpdateMesh/AppendMeshSample. Lifetime/reset/failure rules above
 * apply to both. Atlas rectangles describe one longitude/latitude chart in
 * texels; the texture must also contain its repeated longitude copy. */
typedef struct Sim3DDepthSphericalQuad {
  Sim3DDepthPosition positions[4];
  float normals[4][3];
  float weights[4][2];
  unsigned triangle;
} Sim3DDepthSphericalQuad;

typedef struct Sim3DDepthSphericalSample {
  float rotation[4]; /* cos/sin longitude, cos/sin latitude */
  ArRenderPointF offset; /* normalized chart displacement, after rotation */
  ArRenderRectI atlas;
  ArRenderPointF texture_size;
  ArRenderColorF color;
} Sim3DDepthSphericalSample;

Sim3DDepthMesh *Sim3DDepthPass_CreateSphericalMesh(void);
bool Sim3DDepthPass_UpdateSphericalMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthSphericalQuad *quads, size_t quad_count);
bool Sim3DDepthPass_AppendSphericalSample(Sim3DDepthPassLayer layer,
    Sim3DDepthMesh *mesh, const Sim3DDepthSphericalSample *sample);

/* Continuous spherical body mapping. Unlike exact-depth shadow receivers,
 * these transparent shells sample normalized, perspective-interpolated sphere
 * directions per fragment: longitude seams/poles need no moving CPU splits.
 * Source quads retain unit directions and screen-linear opacity. Placement is
 * radius * basis * normal + centre; texture_basis maps SOURCE normals into the
 * texture sphere before each sample's rotation. Both bases are orthonormal.
 * The matrix uses -W..W clip Z; hardware owns clipping, not the caller.
 *
 * One existing effect handle, camera/viewport-independent Ready, copied values,
 * bounded source/sample budgets and the same reset/queued-update rules. Only
 * Cloud material is supported (its existing atlas, linear sampler, no depth
 * writes). Append validates ALL samples atomically; rejection queues nothing
 * and permits complete ordinary fallback. No ordinary Cloud appends may mix.
 * This is continuous mapping, not bit-identical legacy affine atlas mapping. */
typedef struct Sim3DDepthSphericalBodyVertex {
  float normal[3], opacity;
} Sim3DDepthSphericalBodyVertex;
typedef struct Sim3DDepthSphericalBodyTransform {
  float matrix[16], basis[3][3], centre[3], radius, texture_basis[3][3];
} Sim3DDepthSphericalBodyTransform;
Sim3DDepthMesh *Sim3DDepthPass_CreateSphericalBodyMesh(void);
bool Sim3DDepthPass_UpdateSphericalBodyMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthSphericalBodyVertex *vertices, size_t quad_count);
bool Sim3DDepthPass_AppendSphericalBodies(Sim3DDepthMesh *mesh,
    const Sim3DDepthSphericalBodyTransform *transform,
    const Sim3DDepthSphericalSample *samples, size_t sample_count);

/* Shared radial surface source used by globe views.
 * One camera-independent quad stream drives BOTH textured Ground and optional
 * spherical CloudShadow samples. Every draw uses identical homogeneous
 * positions/triangles and hardware clipping. Colors AND UVs are screen-linear;
 * this is not a bit-exact replacement for legacy CPU-clipped attributes.
 *
 * normal is the source-space unit direction used for radial placement and,
 * by default, spherical atlas coordinates (see shadow_basis below).
 * elevation is {anchor source units, extra rise
 * source units}; extra_scale converts the latter to world units. shade_normal
 * is a unit (or zero) source-space lighting normal. Ground RGB is multiplied
 * by ambient + diffuse * max(0, dot(shade_normal, light)); alpha is unchanged.
 * Light must be unit or zero, with ambient/diffuse each in [0,1]. The caller
 * supplies light in this same space, without a backend lighting
 * model, chart policy, map identity, game clock or borrowed data.
 *
 * Update copies all source data; Ready survives camera/viewport changes.
 * Append copies the complete transform and sample array, atomically queues
 * one Ground draw plus all shadows, or queues NOTHING. Ground follows opaque
 * call ordering; shadows use the existing separate effect budget/no-depth-write
 * policy and may coexist with retained spherical samples, not ordinary shadow
 * geometry. Shadows ignore source color/lighting, using each sample's color.
 * The handle consumes ONE existing opaque slot. Existing vertex budgets,
 * reset/queued-update/destroy rules apply; no source selection is inferred.
 * Uses the existing Ground/Cloud atlas publication and sampler policy.
 * radial.variant must be zero. The scene still owns coarse culling, source
 * revisions and fallback BEFORE queuing any part of this surface. */
typedef struct Sim3DDepthSurfaceVertex {
  float normal[3], elevation[2], shade_normal[3];
  ArRenderColorF color;
  ArRenderPointF uv;
} Sim3DDepthSurfaceVertex;
typedef struct Sim3DDepthSurfaceTransform {
  Sim3DDepthRadialTransform radial;
  float extra_scale, light[3], ambient, diffuse;
  /* Optional orthonormal source-to-shadow coordinate frame. All zeros mean
   * identity, preserving the ordinary chart-space source. Placement/lighting
   * are unaffected. Lets a rigidly oriented source sample another sphere frame. */
  float shadow_basis[3][3];
} Sim3DDepthSurfaceTransform;
Sim3DDepthMesh *Sim3DDepthPass_CreateSurfaceMesh(void);
bool Sim3DDepthPass_UpdateSurfaceMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceVertex *vertices, size_t quad_count);
/* Optional independent overlay-mask coordinates, one per source vertex.
 * NULL preserves ordinary texture-UV masking. Copies both arrays; invalid
 * mask coordinates reject the whole update before changing retained data.
 * Mask coordinates must be finite and within [-16,16]. Texture sampling and
 * spherical shadows are unaffected; no scene/map interpretation is inferred. */
bool Sim3DDepthPass_UpdateSurfaceMeshWithMask(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceVertex *vertices, const ArRenderPointF *mask_uv,
    size_t quad_count);
/* Optional ordered selection, copied before returning. At most 64 ranges,
 * total quads within the original source budget. One zero-length range draws
 * nothing; NULL/zero restores the full source. Source updates restore full selection. Rejection
 * leaves the old selection intact; queued updates are forbidden. The backend
 * compacts selected ranges on the GPU only when selection/source changes, so
 * each material remains one draw and no source vertices return to the CPU. */
bool Sim3DDepthPass_SelectSurfaceMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthMeshRange *ranges, size_t range_count);
bool Sim3DDepthPass_AppendSurfaceMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceTransform *transform,
    const Sim3DDepthSphericalSample *shadows, size_t shadow_count);
/* Optional screen-linear overlays use the SAME source/position/depth path.
 * clear_rect and feather are in source UV units: opacity ramps smoothly with
 * Euclidean distance outside the rectangle. Zero feather means full coverage.
 * GroundBlur multiplies lit source RGBA by color; GroundHaze replaces RGB by
 * color, retaining source alpha. At most one of each layer, no depth writes.
 * The entire ground/shadow/overlay group is atomic and shares existing budgets. */
typedef struct Sim3DDepthSurfaceOverlay {
  Sim3DDepthPassLayer layer;
  ArRenderRectF clear_rect;
  float feather;
  ArRenderColorF color;
} Sim3DDepthSurfaceOverlay;
bool Sim3DDepthPass_AppendSurfaceLayers(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceTransform *transform,
    const Sim3DDepthSphericalSample *shadows, size_t shadow_count,
    const Sim3DDepthSurfaceOverlay *overlays, size_t overlay_count);

/* Independently transformed/materialled ranges in ONE retained source. Ranges
 * address the current selected stream (or full source when not selected).
 * Valid empty ranges draw nothing. Copies all inputs; validates every range,
 * transform, layer and combined budget before queuing ANY batch. At most 64
 * batches, within the existing geometry/effect budgets and opaque handle count.
 * Range order is draw order within each layer; ordinary insertion order is kept.
 * layer must explicitly be Ground, Mountain or WorldMountain. Cutouts use their
 * own atlas and nearest sampler, with the same alpha/depth policy as ordinary
 * geometry. Only Ground accepts shadows/overlays: those do not sample cutout
 * alpha and would otherwise fill transparent holes. */
typedef struct Sim3DDepthSurfaceBatch {
  Sim3DDepthPassLayer layer;
  Sim3DDepthMeshRange range;
  Sim3DDepthSurfaceTransform transform;
  const Sim3DDepthSphericalSample *shadows;
  size_t shadow_count;
  const Sim3DDepthSurfaceOverlay *overlays;
  size_t overlay_count;
} Sim3DDepthSurfaceBatch;
bool Sim3DDepthPass_AppendSurfaceBatches(Sim3DDepthMesh *mesh,
    const Sim3DDepthSurfaceBatch *batches, size_t batch_count);

/* Creates the shaders/pipeline and verifies D32 support. Call during video
 * startup so an unsupported backend is a launch error, never a missing-scene
 * fallback discovered after entering SIM mode. */
bool Sim3DDepthPass_Require(ArRenderDevice *device);

/* A viewport-sized, transparent color target paired with a real D32 depth
 * attachment. Geometry is collected by material so texture changes cost a
 * handful of draws. Opaque visibility is resolved by GPU depth, not painter
 * ordering; transparent overlays test depth without replacing it. */
bool Sim3DDepthPass_Begin(ArRenderDevice *device, int width, int height,
                          ArRenderFilter output_filter);
/* Ordinary backend textures are not necessarily valid sampling resources for
 * a backend's custom depth pipeline. Upload changed regions of the mountain
 * cutout atlas into pass-owned storage instead. Regions use full-atlas pixel
 * coordinates and are submitted as one backend transfer transaction. The
 * first publication must cover the complete texture. */
bool Sim3DDepthPass_UploadMountainAtlasRegions(
    ArRenderDevice *device, const uint32_t *argb_pixels,
    int width, int height, int pitch,
    const ArRenderRectI *regions, int region_count);
/* Independent pass-owned atlas storage for Mountain, WorldMountain, Ground,
 * GroundBlur, Cloud or VolumeCloud.
 * The layer is semantic material identity, never a native texture handle.
 * Uses the same ARGB/pitch/dirty-region contract as the mountain wrapper. */
bool Sim3DDepthPass_UploadAtlasRegions(
    ArRenderDevice *device, Sim3DDepthPassLayer layer,
    const uint32_t *argb_pixels, int width, int height, int pitch,
    const ArRenderRectI *regions, int region_count);

/* Optional owner-thread snapshots of the Ground atlas. One cache may exist,
 * with at most 16 immutable versions, each at most 2048x2048 RGBA8 (256 MiB
 * total). Callers own content/revision identities; the adapter only copies
 * a successfully published mutable atlas, entirely on the GPU. Capture is
 * atomic and does not select a different version. Each Begin restores the
 * mutable atlas. Select requires an active pass; Select(NULL, 0) restores the
 * mutable atlas and failed selection leaves the binding unchanged.
 * Binding/capture cannot change after Ground has been queued in a pass.
 * Reset releases GPU payloads but preserves the handle for recapture.
 * Destroying a queued selection invalidates that pass, not borrowed memory. */
typedef struct Sim3DDepthAtlasCache Sim3DDepthAtlasCache;
enum { kSim3DDepthAtlasVersionLimit = 16 };
Sim3DDepthAtlasCache *Sim3DDepthPass_CreateAtlasCache(void);
bool Sim3DDepthPass_HasAtlasVersion(const Sim3DDepthAtlasCache *cache, unsigned version);
bool Sim3DDepthPass_CaptureAtlasVersion(Sim3DDepthAtlasCache *cache, unsigned version);
bool Sim3DDepthPass_SelectAtlasVersion(Sim3DDepthAtlasCache *cache, unsigned version);
void Sim3DDepthPass_DestroyAtlasCache(Sim3DDepthAtlasCache *cache);
bool Sim3DDepthPass_AppendQuad(Sim3DDepthPassLayer layer,
                               const Sim3DDepthVertex vertices[4]);
/* Appends contiguous groups of four vertices while preserving the same
 * project-private layer contract as AppendQuad. Backends reserve once for the
 * complete batch; callers still know nothing about backend vertex storage. */
bool Sim3DDepthPass_AppendQuads(Sim3DDepthPassLayer layer,
                                const Sim3DDepthVertex *vertices,
                                size_t quad_count);
/* Submits all collected layers. shadow_texture is required only when a
 * ShadowReceiver quad was appended; pass an invalid handle for the ordinary
 * solid pass. */
ArRenderTexture Sim3DDepthPass_Submit(
    ArRenderDevice *device, ArRenderTexture shadow_texture);
bool Sim3DDepthPass_IsCollecting(void);
const char *Sim3DDepthPass_LastError(void);
void Sim3DDepthPass_Reset(ArRenderDevice *device);

#endif  /* SIM3D_DEPTH_PASS_H */
