#ifndef SIM_RENDER_METADATA_H
#define SIM_RENDER_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

/* Phase D1's immutable simulation-town render contract.  The producer is
 * written only by the game thread's $01:ADAD/$01:AE6F HLE leaves; consumers
 * receive a value copy in FrameSlot and never retain a producer pointer. */
enum {
  kSimMaxSourceRecords = 92,  /* 48 fixed + 44 world records. */
  kSimMaxRenderObjects = 128, /* At most one priority run per OAM slot. */
};

/* Build-time switch for the authentic top-down picker view.
 *
 * The original contract forced Direct the People / Building Direction and every
 * targeted miracle onto the authentic flat renderer, so targeting was
 * pixel-identical to the original game by construction. In practice the
 * projected ground reads accurately enough to aim on, so this is compiled out
 * by default while the perspective is evaluated. The whole path is retained,
 * not deleted: build with -DAR_SIM3D_PICKER_TOPDOWN=1 (CMake option
 * AR_SIM3D_PICKER_TOPDOWN) to restore it.
 *
 * Gameplay is unaffected either way. `$7F:9215` still selects the picker, the
 * D-pad still targets in original game coordinates, and the selected cell is
 * still the ROM's. Only the presentation of those frames changes. */
#ifndef AR_SIM3D_PICKER_TOPDOWN
#define AR_SIM3D_PICKER_TOPDOWN 0
#endif

typedef uint32_t SimRenderFeatureMask;

typedef enum SimRenderFeature {
  kSimFeature_SeparatedComposite = 1u << 0,
  kSimFeature_GroundProjection = 1u << 1,
  kSimFeature_ObjectBillboards = 1u << 2,
  kSimFeature_VirtualHeight = 1u << 3,
  kSimFeature_Shadows = 1u << 4,
  kSimFeature_SoftShadows = 1u << 5,
  kSimFeature_RimLight = 1u << 6,
  kSimFeature_Backdrop = 1u << 7,
  kSimFeature_PickerExitEase = 1u << 8,
  /* The Mode-7 world map drawn as a half-resolution ground extension outside
   * the town's own 512x512 playfield. Distinct from Backdrop: this lives in
   * the ground plane in front of the sky, not behind the finite ground. */
  kSimFeature_WorldUnderlay = 1u << 9,
  /* Cloud cover over the ground extension. Its job is not decoration: OAM can
   * only place sprites within the authentic window plus the widescreen
   * margins, so the extended ground can never show actors and would otherwise
   * read as a depopulated town. The shroud draws over everything, including
   * objects, so what it covers is unresolvably distant rather than empty. */
  kSimFeature_CloudShroud = 1u << 10,
  /* The town's own ground faded out toward the sprite-drawable edge, letting
   * the already-hazed world map show through. Same job as the shroud stated
   * as light rather than as cover: the bright region is where actors can
   * exist, so the boundary reads as an area of effect instead of as sprites
   * failing. Continuous and hole-free, which is what lets the shroud above it
   * be thin. */
  kSimFeature_CullHaze = 1u << 11,
  kSimFeature_All = (1u << 12) - 1,
} SimRenderFeature;

typedef enum SimViewKind {
  kSimView_None,
  kSimView_Enhanced,
  kSimView_AuthenticPicker,
  kSimView_AuthenticFallback,
} SimViewKind;

/* Per-frame D2 capture result. Kept in the frame-owned metadata contract so
 * diagnostics never need to query the live compositor. */
typedef enum Sim3DCaptureStatus {
  kSim3DCapture_Inactive,
  kSim3DCapture_MasterOff,
  kSim3DCapture_NotRequested,
  kSim3DCapture_Picker,
  kSim3DCapture_NoRenderer,
  kSim3DCapture_OverlayConflict,
  kSim3DCapture_UnsupportedPpu,
  kSim3DCapture_UnsupportedColorMath,
  kSim3DCapture_AllocationFailure,
  kSim3DCapture_Capturing,
  kSim3DCapture_AtlasInvalid,
  kSim3DCapture_PixelMismatch,
  kSim3DCapture_Ready,
} Sim3DCaptureStatus;

typedef enum SimRecordTier {
  kSimRecordTier_Fixed,
  kSimRecordTier_World,
} SimRecordTier;

typedef enum SimRenderObjectTrait {
  /* $D233-$D302 direction/position cursor compositions are painted onto the
   * selected map square, never propped up as screen-facing actors. */
  kSimObjectTrait_MapPlane = 1u << 0,
  /* Effects and projectiles whose composition is authored around the record
   * origin rather than standing on the union foot. */
  kSimObjectTrait_RecordOriginAnchor = 1u << 1,
  /* Classified in D3c, consumed by the D4a shadow pass. */
  kSimObjectTrait_NoShadow = 1u << 2,
  /* Boats keep the map-height anchor but never take a flight shadow. */
  kSimObjectTrait_WaterPlane = 1u << 3,
  /* Art that is overhead in the fiction while anchored to the ground in the
   * data: the miracle clouds. D3b's depth sort orders a band by the record's
   * screen row, which is right for actors standing on the map and wrong for
   * something whose composition hangs in the sky above that row -- a tree one
   * row nearer would draw over the cloud. This puts the object last in its
   * band regardless of row, restoring the overlap the ROM's own OAM order
   * expressed before the sort existed. Deliberately NOT a height: the family
   * must keep its record-origin ground anchor (see D3c). */
  kSimObjectTrait_Overhead = 1u << 4,
} SimRenderObjectTrait;

/* D3c presentation planes.  These are art/presentation classes derived from
 * docs/sim-object-catalog.md, not a gameplay Z coordinate: the ROM keeps
 * every world record on one planar map and `+$1A/+$1C` remain planar
 * velocities. */
typedef enum SimHeightClass {
  kSimHeightClass_None,        /* fixed tier / not in the height system */
  kSimHeightClass_Grounded,    /* people, animals, scene composites */
  kSimHeightClass_WaterPlane,  /* sailboats */
  kSimHeightClass_GroundEffect,/* fire, building lightning, struck ground */
  kSimHeightClass_SemiGrounded,/* Napper ground-pluck frames */
  kSimHeightClass_Flying,      /* angel and enemy classes $12-$15 */
  kSimHeightClass_FlyingProjectile, /* angel arrow */
  kSimHeightClass_MapPlane,    /* direction/position cursors */
  /* A flying actor lowered onto the map plane for a classified ground-strike
   * state, so its body stays continuous with its own ground-anchored effect. */
  kSimHeightClass_GroundStrike,
  kSimHeightClass_Count,
} SimHeightClass;

/* Presentation heights in authentic SNES pixels above the ground plane. */
enum {
  kSimVirtualHeight_Flying = 24,
  kSimVirtualHeight_SemiGrounded = 8,
  /* Per-frame ramp applied between classified planes so a composition change
   * cannot teleport an actor. Contact-critical classes bypass it. */
  kSimHeightSlewStep = 4,
};

/* True for classes whose ground contact is positioned by the ROM itself and
 * must therefore be exact on their very first frame. */
bool Sim3D_HeightClassIsContactExact(SimHeightClass height_class);

typedef struct SimObjectClassification {
  uint8_t height_class;
  int16_t virtual_height;
  uint8_t traits;
} SimObjectClassification;

/* Pure record-semantics-first, composition-override-second classifier.
 * `tier` is a SimRecordTier, `type` is the record's +$0E class, and
 * `semantic_state` is the masked +$12 state (zero for fixed records). */
SimObjectClassification Sim3D_ClassifyObject(
    uint8_t tier, uint16_t type, uint16_t semantic_state,
    uint16_t record_address, uint16_t composition);

const char *Sim3D_HeightClassName(SimHeightClass height_class);

/* Stages with a shipped implementation, as of D4a. `Sim3D_ImplementedFeatures`
 * reports these once a frame captures cleanly; the settings menu uses the
 * constant directly so a stage can still be configured outside a town, where
 * nothing has been captured yet. Extend it as each visual gate passes. */
enum {
  kSim3DShippedFeatures =
      kSimFeature_SeparatedComposite | kSimFeature_GroundProjection |
      kSimFeature_ObjectBillboards | kSimFeature_VirtualHeight |
      kSimFeature_Shadows | kSimFeature_SoftShadows | kSimFeature_RimLight |
      kSimFeature_WorldUnderlay | kSimFeature_CloudShroud |
      kSimFeature_CullHaze | kSimFeature_Backdrop,
};

/* Default D4a shadow darkness, percent of full black. */
enum { kSimShadowOpacityDefaultPct = 60 };
/* Default light: near-overhead, thrown slightly to screen right. */
enum {
  kSimLightAzimuthDefaultDeg = 90,
  kSimLightElevationDefaultDeg = 90,
  kSimShadowSoftnessDefaultPct = 35,
  kSimRimStrengthDefaultPct = 10,
};
/* How far the world underlay reads as "distant": percent of the way from the
 * underlay's own colours to the scene backdrop it is blended over.
 *
 * Raised once the focus falloff landed. This alpha now applies only to the
 * blurred copy -- the sharp one is drawn over it unhazed where the sprite
 * window is live -- so it is a far-field control rather than a whole-map dim,
 * and it can be pushed much harder without flattening the ground the player
 * is actually working on. */
enum { kSimUnderlayHazeDefaultPct = 40 };
/* Cloud shroud: how opaque the cover becomes at full density, and how far
 * beyond the sprite-drawable edge it takes to get there, in authentic pixels.
 * The ramp is what makes the clouds appear to whisk aside as the camera
 * advances -- the boundary is camera-relative, so approaching thins them. */
enum {
  kSimCloudOpacityDefaultPct = 35,
  /* Long, deliberately. The original short ramp came from the era when the
   * shroud alone had to guarantee opacity, so the clear-but-culling band had
   * to be narrow. Per-record cover carries that guarantee now, which frees
   * these to describe recession instead: a wide falloff and a generous inset
   * make the banks read as distance rather than as a wall at a fixed radius.
   * (`Sim3D_CloudCoverage` caps the inset at a quarter of the shorter
   * half-extent regardless, so a large value cannot veil the centre.) */
  kSimCloudFalloffDefaultPx = 96,
  kSimCloudInsetDefaultPx = 80,
  /* Cull lead: how far before the sprite-window edge a record's cover reaches
   * full strength. Roughly a large composition's own width, so a record is
   * fully covered while its last parts are still being emitted. */
  kSimCullLeadDefaultPx = 48,
  /* How far the town ground fades toward the underlay outside the sprite
   * window, and over how many pixels it gets there.
   *
   * 100 is the argument-from-first-principles value: anything less leaves a
   * blend of bright town ground and dim world map, a third brightness
   * matching neither, and at 100 the out-of-range ground simply IS the
   * underlay -- two tiers, no invented middle, and the canvas's own hard
   * 512x512 rectangle is fully transparent by the time it arrives.
   *
   * The shipped default is 50 anyway, chosen by looking at it. The focus
   * falloff landed after that reasoning and now carries much of the same
   * distinction: with the far field defocused, a partial fade no longer reads
   * as a smeared gradient, and keeping some of the town's own detail out
   * there is worth more than the theoretical two-tier purity. Set it to 100
   * to get the argued behaviour back.
   *
   * The ramp is much longer than the cloud lead on purpose: a step reads as a
   * hard line across the ground, which would be a worse tell than the patchy
   * cover it replaces. */
  kSimCullHazeDefaultPct = 50,
  kSimCullHazeLeadDefaultPx = 208,
  /* How far out-of-range ground is taken toward black. Separate from the fade
   * above because they answer different questions: the fade decides which
   * layer is showing, this decides how lit it is. Multiplied into the colour,
   * so it darkens rather than mixing toward the sky the way the underlay's own
   * distance haze does. */
  kSimCullDimDefaultPct = 35,
  /* Corner radius of the lit window. Generous, because the shape reads as
   * deliberate framing at this size and as a rounded rectangle -- something
   * with corners at all -- below roughly a third of the short half-extent. */
  kSimCullCornerDefaultPx = 96,
  /* How much of the defocused world map is allowed to show at full distance.
   * A partial mix reads as depth; a full one reads as a smear, because the
   * 4x downsample is a stand-in for a lens blur and not a very good one. */
  kSimUnderlayDefocusDefaultPct = 40,
  /* Atmospheric backdrop strength: how far the horizon and zenith depart from
   * the scene's own backdrop colour. Zero reproduces the flat fill exactly,
   * which is what the D5a-2 checkpoint compares against. */
  kSimBackdropStrengthDefaultPct = 100,
  /* Where the synthetic horizon sits, percent of viewport height from the top.
   * The real one is never in frame (see DrawSimBackdrop), and the backdrop is
   * only ever seen fully zoomed out past the end of the extended map, so this
   * is placed where sky reads rather than where the ground plane vanishes. */
  kSimBackdropHorizonDefaultPct = 60,
  /* Cloud altitude above the ground plane, in the same authentic pixels D3c
   * states virtual heights in. Comfortably clear of kSimVirtualHeight_Flying
   * (24) so the banks pass over the highest thing the classifier lifts,
   * rather than through it. */
  kSimCloudAltitudeDefaultPx = 72,
  /* Cloud drift rate, percent of the built-in per-layer velocities. Slow by
   * design: banks that visibly race read as a screen effect rather than as
   * weather, and this field also has to hide a cull boundary, which it cannot
   * do if it is somewhere else a second later. */
  kSimCloudDriftDefaultPct = 100,
};

/* The sprite-drawable window in the emitter's own biased coordinates.
 *
 * $01:ADAD/$01:AE6F test a part's biased x against `kSimSpriteWindowBiasedWidth
 * + margins` and its biased y against `kSimSpriteWindowBiasedHeight`, then park
 * the OAM slot at $E000 when either fails. Mirrored here rather than shared
 * from the emitter's private enum because the cull-lead ramp is a pure
 * function of the same window and must be testable without a CPU; the emitter
 * carries a static assertion that the two agree. */
enum {
  kSimSpriteWindowBiasedWidth = 272,   /* 256 authentic + 16 draw bias. */
  kSimSpriteWindowBiasedHeight = 0xF0,
};

/* Why a composition part never reached OAM. A record accumulates the union
 * across its parts, so a record clipped on both axes reports both. */
typedef enum SimClipReason {
  kSimClip_Horizontal = 1u << 0,
  kSimClip_Vertical = 1u << 1,
} SimClipReason;

typedef enum SimMetadataIntegrityFlag {
  kSimMetadataIntegrity_Overflow = 1u << 0,
  kSimMetadataIntegrity_Overlap = 1u << 1,
  kSimMetadataIntegrity_CursorMismatch = 1u << 2,
  kSimMetadataIntegrity_RecordOrder = 1u << 3,
  kSimMetadataIntegrity_InvalidRecord = 1u << 4,
  kSimMetadataIntegrity_WorldSuffix = 1u << 5,
  kSimMetadataIntegrity_AtlasOverflow = 1u << 6,
  kSimMetadataIntegrity_AtlasRasterFailure = 1u << 7,
} SimMetadataIntegrityFlag;

typedef struct SimSourceRecord {
  uint16_t record_address;
  uint16_t composition;
  uint16_t world_x, world_y;
  uint16_t type;
  uint16_t semantic_state;
  uint16_t status;
  uint16_t oam_first;
  uint8_t oam_count;
  uint16_t fragment_first;
  uint16_t fragment_count;
  uint8_t tier;
  uint8_t alternate_attributes;
  /* D5a cull evidence. The emitter's biased composition origin, plus how many
   * of the record's parts the sprite window rejected and why.
   *
   * The anchor is the emitter's own `base_x/base_y`, not a re-derivation from
   * world position and camera: the emitter reaches it through DP $94/$96 with
   * 16-bit wraparound, and a second derivation is a second thing to keep in
   * step. `anchor_valid` is what separates a genuine origin at zero from a
   * record whose producer never supplied one -- every test that predates D5a
   * drives BeginRecord without an anchor. */
  int16_t anchor_x, anchor_y;
  uint8_t anchor_valid;
  uint8_t clip_reason;
  uint16_t clipped_parts;
} SimSourceRecord;

typedef struct SimRenderObject {
  uint16_t record_address;
  uint16_t composition;
  uint16_t world_x, world_y;
  uint16_t type;
  uint16_t semantic_state;
  uint16_t oam_first;
  uint8_t oam_count;
  uint8_t priority;
  uint8_t source_index;
  uint8_t tier;
  /* SNES OBJ palettes 4-7 participate when CGADSUB enables OBJ color math;
   * palettes 0-3 never do. Records are split when this bit changes. */
  uint8_t color_math_eligible;
  int16_t foot_x, foot_y;
  int16_t local_x0, local_y0, local_x1, local_y1;
  uint16_t atlas_x, atlas_y, atlas_w, atlas_h;
  uint8_t traits;
  uint8_t height_class;
  /* Presentation-only lift above the projected ground plane, in authentic
   * SNES pixels. Zero for every grounded and fixed object. `virtual_height`
   * is the published value after per-record easing; `classified_height` is
   * the plane the pure classifier selected for this frame, retained so
   * diagnostics can tell an in-progress ramp from a misclassification. */
  int16_t virtual_height;
  int16_t classified_height;
  uint8_t atlas_valid;
} SimRenderObject;

/* D4a caster selection. Kept beside the classifier rather than in the renderer
 * so "what casts a shadow" stays one data question: a world-tier object with
 * usable atlas art that D3c did not mark MapPlane or NoShadow. Ground-anchored
 * classes still cast — a zero height simply puts the silhouette under the
 * actor's own feet. */
/* Cloud-shroud cover at one point, 0..1.
 *
 * Pure, and deliberately separate from the renderer: the shroud exists to hide
 * ground that OAM cannot populate, so "how covered is this point" is a
 * statement about the sprite-drawable rectangle, not about clouds.
 *
 * The ramp starts `inset` pixels INSIDE the rectangle and reaches full cover
 * `falloff` pixels outside it, so cover is already substantial at the edge
 * itself. Starting the ramp at the edge leaves a band that is visibly clear
 * but already culling sprites, which reads as actors vanishing into nothing.
 * Because the rectangle is camera-relative, advancing the camera thins the
 * cover ahead -- which is what makes the clouds whisk aside rather than
 * slide. */
float Sim3D_CloudCoverage(float x, float y, float clear_x0, float clear_x1,
                          float clear_y0, float clear_y1, float inset,
                          float falloff);

/* D5a cull-lead ramp for one record, 0..1.
 *
 * Zero well inside the sprite-drawable window; 1.0 at the window edge and
 * beyond. The ramp reaches full `lead` pixels BEFORE the edge, not after, so
 * whatever the renderer anchors to this value is already at full strength on
 * the frame the record actually clips. Ramping after the edge is what made
 * the field shroud read as inconsistent: the sprite was gone while the cover
 * over it was still arriving.
 *
 * `corner` rounds the window's corners; the ramp itself is smoothstepped
 * rather than linear, so neither end of it leaves a crease for the eye to
 * find and read as an edge.
 *
 * `lift_inset` raises the window's BOTTOM edge only, and exists because the
 * lit region is painted on the ground and can therefore only ever express the
 * height-zero boundary. A record drawn `lift` pixels up-screen crosses the
 * projected boundary somewhere else entirely, so without this the bright area
 * promises "actors can be here" and is wrong by the lift amount along the
 * bottom. Insetting by the largest lift the classifier hands out makes the
 * promise true for every height class at once.
 *
 * The top edge is deliberately NOT inset. Lift is toward negative y, so a
 * record approaching the top is drawn further outside the window than its
 * anchor sits -- it leaves the lit region before it culls, which is already
 * the safe direction.
 *
 * Pure, and stated in the emitter's biased coordinates, so the cull predicate
 * and the thing that hides it are the same arithmetic rather than two
 * derivations that agree by inspection. */
float Sim3D_CullProximity(int16_t anchor_x, int16_t anchor_y,
                          int margin_left, int margin_right, int lead,
                          int corner, int lift_inset);

/* Whether a record should carry cover this frame, and how much.
 *
 * A record earns cover when the sprite window is what removed it, never when
 * the game did: a composition the ROM declined to draw, a record outside the
 * finite town, or a destroyed projectile is legitimately absent and putting a
 * cloud over it would assert something false. Returns 0 for those. */
float Sim3D_SourceCullCover(const SimSourceRecord *source,
                            int margin_left, int margin_right, int lead,
                            int corner, int lift_inset);

/* How far above its record the renderer draws this source, in authentic
 * pixels, after the presentation height scale.
 *
 * Cover for a culled record is two separate questions and conflating them is
 * what makes a lifted actor look like it vanished early:
 *
 *   WHEN cover arrives is a question about the emitter. It culls on the
 *   record's own y -- the ROM knows nothing about virtual height -- so
 *   Sim3D_SourceCullCover uses the unlifted anchor and must keep doing so.
 *
 *   WHERE cover goes is a question about the renderer. A flying record is
 *   drawn this many pixels up-screen from its record position, so cover
 *   placed at the record lands below the sprite it is meant to hide. At the
 *   bottom edge that reads as the actor blinking out with clear ground under
 *   it, which is the whole artifact D5a exists to remove.
 *
 * Zero for everything grounded, so the two questions only diverge where the
 * renderer actually moved something. */
int16_t Sim3D_SourceDrawLift(const SimSourceRecord *source,
                             unsigned height_scale_x100);

/* The largest lift Sim3D_SourceDrawLift can return, for the window inset.
 * Derived from the classifier's own ceiling rather than measured over the
 * live record list: the inset must not breathe as records come and go, or the
 * ground fade would drift up and down while nothing on screen moved. */
int16_t Sim3D_MaxDrawLift(unsigned height_scale_x100);

bool Sim3D_ObjectCastsShadow(const SimRenderObject *object);

typedef struct SimFrameData {
  SimViewKind view;
  bool master_enabled;
  bool metadata_valid;
  /* What the stage toggles asked for, and what survived the dependency
   * resolver and the frame's own capture state. One profile: comparing two
   * builds of the scene is done by toggling stages across separate runs, not
   * by rendering two profiles from one frame. */
  SimRenderFeatureMask requested_features;
  SimRenderFeatureMask effective_features;
  uint32_t diagnostic_layer_mask;
  uint8_t town;
  uint16_t game_frame;
  uint16_t camera_x, camera_y;
  uint16_t angel_x, angel_y;
  uint16_t picker_flag;
  uint32_t build_serial;
  uint32_t integrity_flags;
  bool atlas_valid;
  uint16_t atlas_width, atlas_height;
  uint16_t atlas_used_width, atlas_used_height;
  bool separated_valid;
  uint8_t separated_status;
  uint32_t separated_mismatch_pixels;
  /* The PPU colour-math state the D2 gate last looked at.
   *
   * The gate fails closed on anything it has not been shown to reproduce, so
   * "unsupported_color_math" is a correct-but-opaque answer: it says the frame
   * was rejected without saying what it was rejected for. Carrying the
   * registers into the frame makes the transition log self-diagnosing, which
   * is the whole point of that log existing. */
  uint8_t separated_cgwsel, separated_cgadsub;
  uint16_t separated_fixed_color;
  uint8_t separated_screen_main, separated_screen_sub;
  uint8_t separated_brightness;
  uint64_t separated_hash;
  uint32_t separated_backdrop_argb;
  bool object_half_add;
  int16_t projection_pitch_mrad, projection_yaw_mrad;
  uint16_t projection_distance_x100;
  /* Resolved presentation tuning: percent of each classified virtual height.
   * Copied here so one frame cannot mix old and new tuning values. */
  uint16_t height_scale_x100;
  /* Resolved D4a shadow darkness, percent. Zero renders no shadow pass at all
   * even when the feature bit is set, so the tuning value alone is enough to
   * A/B the mask without touching the feature mask. */
  uint8_t shadow_opacity_pct;
  /* Extra billboard scale at the catalogue flight plane, percent. Normalized
   * against that plane so the number means what it says regardless of the
   * height scale above; raising the height scale then raises the pop with it. */
  uint8_t height_pop_pct;
  /* Directional light for the D4a shadow pass. Azimuth 0 throws the shadow
   * toward +x (screen right) and advances counter-clockwise; elevation 90 is
   * straight overhead and throws no offset at all. */
  uint16_t light_azimuth_deg;
  uint8_t light_elevation_deg;
  /* D4b blur radius, percent. Zero leaves D4a's hard alpha silhouette. */
  uint8_t shadow_softness_pct;
  /* D4c rim contribution, percent. Zero renders no rim pass at all. */
  uint8_t rim_strength_pct;
  /* World-underlay atmospheric fade, percent. 100 renders no underlay at all,
   * so the dial alone can A/B the layer without touching the feature mask. */
  uint8_t underlay_haze_pct;
  /* Cloud shroud density at full cover, percent; zero draws no clouds. */
  uint8_t cloud_opacity_pct;
  /* Distance over which cover ramps from clear to full, authentic pixels. */
  uint16_t cloud_falloff_px;
  /* How far inside the drawable edge the ramp begins, authentic pixels. */
  uint16_t cloud_inset_px;
  /* Sprite-drawable span in captured-texture columns. The shroud clears
   * exactly the region OAM can populate, so it is derived from the same
   * margins the emitter uses rather than guessed at present time. */
  uint16_t cloud_clear_x0, cloud_clear_x1;
  /* The live widescreen margins the emitter used this frame, published so the
   * renderer can evaluate the cull predicate on the same window the emitter
   * did rather than reconstructing it from cloud_clear_*. */
  int16_t sprite_margin_left, sprite_margin_right;
  /* How far ahead of the sprite-window edge cull cover reaches full strength,
   * in authentic pixels. */
  uint16_t cull_lead_px;
  /* Cull fade: how far the town ground is faded toward the underlay outside
   * the sprite window, and the ramp that gets it there. Zero draws the ground
   * at full opacity everywhere. */
  uint8_t cull_haze_pct;
  /* Out-of-range darkening, percent. Zero leaves brightness alone. */
  uint8_t cull_dim_pct;
  uint16_t cull_haze_lead_px;
  uint16_t cull_corner_px;
  /* Strength of the world map's focus falloff, percent. Zero draws the map
   * sharp everywhere and skips the blurred pass entirely. */
  uint8_t underlay_defocus_pct;
  /* How far the cloud shroud floats above the ground plane, in authentic
   * pixels. Zero lays it flat on the ground, which reads as painted-on fog
   * rather than as cover between the camera and the world. */
  uint16_t cloud_altitude_px;
  /* Cloud drift rate, percent. Zero holds the banks still. */
  uint16_t cloud_drift_pct;
  /* Whether the lit window's bottom edge is inset by the maximum draw lift. */
  uint8_t cull_lift_inset;
  /* Atmospheric backdrop gradient strength, percent. */
  uint8_t backdrop_strength_pct;
  /* Synthetic horizon height, percent of viewport from the top. */
  uint8_t backdrop_horizon_pct;
  /* Resolved world-underlay placement. `underlay_serial` changes whenever the
   * baked image would differ, so the present thread rebuilds its texture from
   * the frame it is drawing rather than from live module state. Zero means the
   * module has nothing usable and the layer must not draw. */
  uint32_t underlay_serial;
  uint8_t underlay_origin_tile_x, underlay_origin_tile_y;
  /* Persistent full-resolution town ground accumulated from verified frames.
   * Changes whenever the canvas image does; zero means nothing to draw. */
  uint32_t town_canvas_serial;
  /* Authentic-pixel column of the captured texture that holds SNES x = 0.
   * Resolved on the game thread so present-time code never re-derives the
   * widescreen margin width. */
  uint16_t underlay_screen_x0;
  uint16_t emitted_oam_count;
  uint16_t claimed_oam_count;
  uint8_t world_oam_first, world_oam_count;
  uint8_t source_count;
  uint8_t zero_oam_source_count;
  uint16_t object_count;
  SimSourceRecord sources[kSimMaxSourceRecords];
  SimRenderObject objects[kSimMaxRenderObjects];
} SimFrameData;

/* Game-thread handoff between the semantic record producer and the atlas
 * builder. The builder works on a value copy, then atomically commits only if
 * the producer serial/count still match. */
typedef struct SimAtlasBuildInput {
  uint32_t build_serial;
  uint16_t object_count;
  SimRenderObject objects[kSimMaxRenderObjects];
} SimAtlasBuildInput;

/* Pure dependency resolver.  implemented_features is a capability mask, not
 * a user request.  D1 intentionally passes zero so every visual path remains
 * authentic while requested/effective diagnostics are exercised. */
SimRenderFeatureMask Sim3D_ResolveFeatureMask(
    SimRenderFeatureMask requested_features,
    SimRenderFeatureMask implemented_features,
    SimViewKind view, bool master_enabled, bool metadata_valid);

/* Game-thread producer API used by the faithful SIM composition leaves. */
void SimRenderMetadata_BeginRecord(
    uint16_t record_address, bool world_record, bool alternate_attributes,
    uint16_t composition, uint16_t world_x, uint16_t world_y,
    uint16_t type, uint16_t semantic_state, uint16_t status,
    uint16_t oam_cursor_before);
/* The emitter's biased composition origin for the record now open. Separate
 * from BeginRecord so the D1 producer contract and its callers are unchanged;
 * a record without this call simply has no cull-lead anchor. */
void SimRenderMetadata_RecordAnchor(int16_t base_x, int16_t base_y);
void SimRenderMetadata_RecordPart(uint16_t oam_cursor, uint16_t attributes);
/* One composition part the sprite window rejected. Counted per part rather
 * than per record because a wide composition can straddle the edge, and a
 * record that lost half its parts is already visibly wrong. */
void SimRenderMetadata_RecordClippedPart(uint8_t reason);
void SimRenderMetadata_EndRecord(uint16_t oam_cursor_after);

bool SimRenderMetadata_CopyAtlasInput(SimAtlasBuildInput *out);
bool SimRenderMetadata_CommitAtlas(
    uint32_t build_serial, const SimRenderObject *objects,
    uint16_t object_count, bool atlas_valid,
    uint16_t atlas_width, uint16_t atlas_height,
    uint16_t atlas_used_width, uint16_t atlas_used_height,
    uint32_t integrity_flags);
bool SimRenderMetadata_AtlasReady(void);

/* Copies the completed producer into an immutable per-frame value. */
void SimRenderMetadata_CaptureFrame(
    SimFrameData *dst, const uint8 *wram, bool master_enabled,
    SimRenderFeatureMask requested_features,
    uint32_t diagnostic_layer_mask,
    SimRenderFeatureMask implemented_features);

/* Deterministic metadata evidence.  The trace is inert unless
 * AR_SIM3D_D1_TRACE names an output JSONL file. */
void SimRenderMetadata_TraceFrame(uint32_t host_frame,
                                  const SimFrameData *frame,
                                  const uint8_t *rgba, int width, int height,
                                  int pitch);
void SimRenderMetadata_TraceClose(void);

/* Test/reset seam.  Production does not need a frame-begin callback: the
 * leaf producer recognizes the ROM pass's fresh cursor plus ordered record
 * restart, including consecutive fully clipped records at cursor zero. */
void SimRenderMetadata_Reset(void);
/* Drops the per-record height easing so the next enhanced frame snaps to its
 * classified planes. Implied by SimRenderMetadata_Reset. */
void SimRenderMetadata_ResetHeightSlew(void);

const char *Sim3D_ViewName(SimViewKind view);
const char *Sim3D_CaptureStatusName(Sim3DCaptureStatus status);

#endif  /* SIM_RENDER_METADATA_H */
