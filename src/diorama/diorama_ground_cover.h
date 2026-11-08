#ifndef AR_DIORAMA_GROUND_COVER_H
#define AR_DIORAMA_GROUND_COVER_H

#include "action/action_ground_cover.h"

/* Receiver-space geometry only: the existing native/authored/trunk adapter
 * supplies actual top corners and projects the result using its own camera
 * mapping. No renderer resources or room/material rules belong here. */
enum { kDioramaGroundCoverCacheSize = 1024 };
/* Opt-in native material ownership. Returns the first row below the continuous
 * opaque support run, bounded by its native16px cell. Called only on static
 * association misses; NULL keeps exact trace matching. */
typedef bool (*DioramaGroundCoverReceiverFn)(const void *,int x,int y,int *bottom);
typedef struct DioramaGroundCoverOwner {
  const void *asset;
  uint64_t revision;       /* Includes geometry, material and editor changes. */
  unsigned kind;          /* Generic, authored or dedicated trunk provider. */
} DioramaGroundCoverOwner;
typedef struct DioramaGroundCoverAssociation {
  DioramaGroundCoverOwner owner;
  uint64_t bank_key;
  ActionGroundCoverPoint from, to;
  float tolerance;
  DioramaGroundCoverReceiverFn receiver;
  unsigned count;
  bool valid, success;
  ActionGroundCoverInterval intervals[kActionGroundCoverMaxIntervals];
} DioramaGroundCoverAssociation;
typedef struct DioramaGroundCoverCache {
  DioramaGroundCoverAssociation entries[kDioramaGroundCoverCacheSize];
  uint64_t hits, misses; /* Diagnostics only; never included in draw state. */
} DioramaGroundCoverCache;

typedef struct DioramaGroundCoverCorner {
  float position[3];
  /* Displacement in the same coordinates per native pixel of snow height.
   * This retains an authored object's rotation/scale without bending its
   * native contact trace a second time. Usually {0,-1,0}. */
  float lift[3];
  /* Original complete material span, before actor/capture/atlas subdivision.
   * Near is zero and far is one; a clipped piece need not contain either. */
  float depth_fraction;
} DioramaGroundCoverCorner;
typedef struct DioramaGroundCoverCap {
  ActionGroundCoverPoint from, to; /* Undeformed native contact trace. */
  /* Near start, near end, far end, far start; existing 012/023 triangles. */
  DioramaGroundCoverCorner corners[4];
  /* Actual complete receiver before actor/texture depth cuts. Producers that
   * subdivide depth supply these; uncut standalone caps use corners directly. */
  bool pressure_quad;
  float pressure_xyz[12];
  float actor_z, contact_radius; /* Same physical Z units as position[2]. */
  DioramaGroundCoverReceiverFn receiver;
  /* Optional native16x16 top artwork, opacity bit0x8000. NULL is fully opaque.
   * Corner UVs are the actual clamped [0,16) values used by the receiving
   * nearest-filtered face, after its texture-repeat/actor subdivision. */
  const uint16_t *top_pixels;
  float top_uv[8];
  /* Optional native-only tiny closing step at the original left/right end.
   * The producer proves the real lower cap has the same physical interval. */
  unsigned char closing_riser[2];
} DioramaGroundCoverCap;
typedef struct DioramaGroundCoverVertex {
  float position[3];
  float native_x;
  ActionGroundCoverSample sample; /* Batched existing incident-light inputs. */
} DioramaGroundCoverVertex;

/* A bounded cache miss evaluates only this trace. Eviction is not an error;
 * camera/pressure changes do not invalidate associations. The supplied asset
 * revision must change when an editor operation changes receiving geometry. */
static bool DioramaGroundCover_Associate(DioramaGroundCoverCache *,
    DioramaGroundCoverOwner, const ActionGroundCoverBank *,
    ActionGroundCoverPoint from, ActionGroundCoverPoint to, float tolerance,
    DioramaGroundCoverReceiverFn receiver,
    const DioramaGroundCoverAssociation **out);

/* Generate only an actual, visible top piece supplied by the caller.
 * Optional authored alpha is clipped against opaque native texel row-runs;
 * no cover is placed over a painted hole or an erased top.
 * Output is independent triangles, preserving the receiver's original
 * diagonal even on folded/nonplanar caps. count=0 on any failure: stage the
 * receiver batch and submit only after its bounded generation succeeds.
 * A successful empty result owns suppression and never requests flat fallback.
 * No allocations, scene scans, lighting scans or projection occur here. */
static bool DioramaGroundCover_BuildCap(DioramaGroundCoverCache *,
    DioramaGroundCoverOwner, const ActionGroundCoverBank *,
    const ActionGroundCoverPressure *, const DioramaGroundCoverCap *, float tolerance,
    DioramaGroundCoverVertex *out, unsigned capacity, unsigned *count);
/* The shallow dense front lip belongs to the same receiver. Only the original
 * complete near edge owns it; actor/texture depth cuts cannot create extra
 * lips. Its crest shares the top's pressure and lift, while its lower edge
 * feathers 1.5 native pixels down the actual transformed front surface. */
static bool DioramaGroundCover_BuildLip(DioramaGroundCoverCache *,
    DioramaGroundCoverOwner, const ActionGroundCoverBank *,
    const ActionGroundCoverPressure *, const DioramaGroundCoverCap *, float tolerance,
    DioramaGroundCoverVertex *out, unsigned capacity, unsigned *count);
static bool DioramaGroundCover_PrepareCapPressure(const ActionGroundCoverBank *,
    const ActionSceneEffectFrame *,const DioramaGroundCoverCap *,ActionGroundCoverPressure *);

#endif
