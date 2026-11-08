#ifndef AR_ACTION_GROUND_COVER_H
#define AR_ACTION_GROUND_COVER_H

#include "action_effects.h"
#include "render/render_types.h"

/* Pure dense-cover samples. The renderer owns actual supporting top polygons,
 * their native-contact provenance, deformation, depth and native priority.
 * This module never invents terrain or projects a rectangle over a pit. */
enum { kActionGroundCoverMaxIntervals = kActionContourMaxPoints - 1,
  kActionGroundCoverMaxContactCuts = 2 + 3 * kActionFieldWakeCount };
#define ACTION_GROUND_COVER_MAX_CONTACT_TOLERANCE 1.01f

typedef struct ActionGroundCoverPoint { float x, y; } ActionGroundCoverPoint;
typedef struct ActionGroundCoverBank {
  unsigned point_count;
  ActionGroundCoverPoint points[kActionContourMaxPoints]; /* Native world XY. */
  float height_noise[kActionContourMaxPoints];
  float depth, painted_width, intensity, wake;
  ArRenderColorF body, crest;
  uint64_t content_key; /* Value key; excludes clock, camera and current feet. */
} ActionGroundCoverBank;
typedef struct ActionGroundCoverFoot { float x, radius, strength; } ActionGroundCoverFoot;
typedef struct ActionGroundCoverContactTrace {
  ActionGroundCoverPoint from,to; /* Actual actor-plane world XY. */
  float source_from_x,source_to_x; /* Immutable native profile coordinate. */
} ActionGroundCoverContactTrace;
typedef struct ActionGroundCoverPressure {
  ActionGroundCoverFoot feet[kActionFieldWakeCount]; /* Player, strongest enemy. */
} ActionGroundCoverPressure;
typedef struct ActionGroundCoverInterval { float t0, t1; } ActionGroundCoverInterval;
typedef struct ActionGroundCoverSample {
  float native_floor_y, settled_height, height, compression, edge;
  /* Tint inputs remain separate so the adapter can use the existing incident
   * light calculation before interpolation, without duplicating lighting. */
  ArRenderColorF body, crest;
  float crest_mix, alpha;
} ActionGroundCoverSample;

typedef enum ActionGroundCoverClaim {
  kActionGroundCoverUnsupported,
  kActionGroundCoverClaimedEmpty,
  kActionGroundCoverClaimedGeometry,
  kActionGroundCoverFailure,
} ActionGroundCoverClaim;

/* Only the positive-width top of a visible foreground BG1 dense cloud bank.
 * Failure clears out. Its receiver owns dense top/lip geometry; loose grains
 * retain their independent WorldDust route. */
bool ActionGroundCover_Prepare(const ActionEffectInstance *, ActionGroundCoverBank *out);
/* Association caches also need the receiving geometry/editor revision.
 * Clock/pressure never invalidates the bank's static support association. */
bool ActionGroundCover_SameSupport(const ActionGroundCoverBank *, const ActionGroundCoverBank *);
bool ActionGroundCover_PreparePressure(const ActionGroundCoverBank *,
    const ActionSceneEffectFrame *, ActionGroundCoverPressure *out);
/* Same captured actor/wake policy, evaluated against an actual native receiver
 * trace rather than the decorative authored path. At most the captured80
 * actors are considered once per receiving cap, never per output vertex. */
bool ActionGroundCover_PrepareSupportPressure(const ActionGroundCoverBank *,
    const ActionSceneEffectFrame *,ActionGroundCoverPoint from,ActionGroundCoverPoint to,
    ActionGroundCoverPressure *out);
/* At most two actual012/023 triangle intersections. Degenerate/parallel
 * pieces carry no pressure; they never invent a guessed support plane. */
bool ActionGroundCover_PrepareMappedPressure(const ActionGroundCoverBank *,
    const ActionSceneEffectFrame *,const ActionGroundCoverContactTrace *,unsigned count,
    ActionGroundCoverPressure *out);

/* Associate one EXISTING top-edge native-contact trace. Output is ordered in
 * the caller's t direction, including reversed traces, and split at path knots.
 * Tolerance is explicit and limited to the native one-pixel contour allowance.
 * true/count=0 is a valid empty footprint. Capacity/invalid-input failure writes
 * no intervals and sets count=0; never display a partial bank after failure. */
bool ActionGroundCover_ClipSupport(const ActionGroundCoverBank *,
    ActionGroundCoverPoint from, ActionGroundCoverPoint to, float tolerance,
    ActionGroundCoverInterval *out, unsigned capacity, unsigned *count);

/* Dynamic profile cuts within one cached support interval. Preserve both
 * endpoints and insert each active foot centre/radius without resampling the
 * static noise profile. This lets a wide authored cap compress between its
 * original vertices. Output uses the same from->to parameter as ClipSupport;
 * it never fills a gap between two different supporting polygons. */
bool ActionGroundCover_ContactCuts(const ActionGroundCoverPressure *,
    float from_x, float to_x, ActionGroundCoverInterval support,
    float *out, unsigned capacity, unsigned *count);

/* depth_fraction spans this receiving polygon's native near(0)/far(1) edges,
 * solely for its rear feather. contact_weight is supplied by the renderer from
 * actual actor Z relative to that cap: core, side-hall and generic caps have
 * different actor positions. A zero weight keeps settled snow unchanged.
 * Call only inside a previously associated support interval; actual deformed
 * XYZ receives the resulting pixel height. No clock enters this sample. */
bool ActionGroundCover_Sample(const ActionGroundCoverBank *, const ActionGroundCoverPressure *,
    float native_world_x, float depth_fraction, float contact_weight,
    ActionGroundCoverSample *out);

ActionGroundCoverClaim ActionGroundCover_ResolveClaim(bool provider_ready,
    bool bank_prepared, bool association_ok, unsigned supported_polygons);
bool ActionGroundCover_SuppressesFlatTop(ActionGroundCoverClaim);

#endif
