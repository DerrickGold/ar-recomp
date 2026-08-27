#ifndef SIM_RENDER_METADATA_H
#define SIM_RENDER_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"
#include "sim_world_navigation_scene.h"
#include "runner_next.h"
#include "types.h"

/* Immutable simulation-town render contract. The $01:ADAD/$01:AE6F HLE leaves
 * produce it; presentation receives a value copy through FrameSlot and never
 * retains a producer pointer. */
enum {
  kSimMaxSourceRecords = 92,  /* 48 fixed + 44 world records. */
  kSimMaxRenderObjects = 128, /* At most one priority run per OAM slot. */
  /* OAM-backed and host-only parts in emitter order. Synthetic parts do not
   * consume one of the 128 hardware slots. ROM checkpoints measured at most
   * 50 OAM parts and 15 synthetic parts (not necessarily in the same frame)
   * across ranges 0..256; 192 leaves almost 3x headroom over their summed
   * upper bound. Overflow fails closed and is published in diagnostics. */
  kSimMaxResolvedParts = 192,
  /* One semantic emitter per relevant record, not one per particle. Thirty-two
   * covers the largest mapped 4x4 impact cohort plus its parent and leaves
   * room for overlapping families. Overflow is still reported and fails the
   * effect stages closed; this is a capacity, never a truncation policy. */
  kSimMaxEffectInstances = 32,
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
  /* Host-authored transient illumination and deterministic particles. They
   * are separate gates so either half can be A/B tested independently. */
  kSimFeature_EffectLighting = 1u << 12,
  kSimFeature_Particles = 1u << 13,
  kSimFeature_All = (1u << 14) - 1,
} SimRenderFeature;

typedef enum SimViewKind {
  kSimView_None,
  kSimView_Enhanced,
  /* Inter-town map $09: full developed world with a forced perpendicular
   * ground camera. Scripted in-plane rotation remains allowed. */
  kSimView_WorldNavigation,
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
  /* Art that is overhead in the fiction: flying actors and miracle clouds. D3b's
   * depth sort orders a band by the record's screen row, which is right for
   * actors standing on the map and wrong for something flying above it. This
   * puts the object last in its band and, during voxel interleave, after every
   * terrain depth slice. Deliberately NOT a height: height and draw order are
   * separate policies, and the cloud family must keep its record-origin ground
   * anchor (see D3c). */
  kSimObjectTrait_Overhead = 1u << 4,
  /* Map selectors are interaction feedback, not terrain. Their authentic OBJ
   * priority is retained inside this class, but projected presentation defers
   * the complete class until the world is finished so raised models can never
   * hide the yellow selection outline. This is deliberately separate from
   * Overhead: a cursor has no physical height and remains projected flat onto
   * its selected cells. */
  kSimObjectTrait_SelectionOverlay = 1u << 5,
  /* Status and thought bubbles the ROM hangs on a structure record. In the
   * flat view they sit at the record's own cell and simply paint over the
   * building; projected, that cell is the building's FOOT, so the bubble ends
   * up inside the model and behind whatever stands in front of it. These ride
   * on top of the structure they belong to instead: the renderer adds the
   * height of the model on that cell. */
  kSimObjectTrait_StructureOverlay = 1u << 6,
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

/* Does this class's height measure from the terrain under it, or from the town
 * as a whole? Grounded art stands on whatever it is over, so a mountain raises
 * it. The angel, enemies and the angel's arrows fly at an absolute altitude
 * above the town -- adding terrain to those makes them jump the moment they
 * cross a peak's cells, which is the opposite of flying over it. */
bool Sim3D_HeightClassStandsOnTerrain(SimHeightClass height_class);

/* Can the town's own geometry HIDE this class? Deliberately narrower than
 * StandsOnTerrain above, which answers the different question of whether
 * terrain RAISES the object. Only art genuinely resting on the ground can be
 * occluded. Everything carrying altitude stays visible -- including the
 * classes that merely dip toward the ground, the Napper's pluck and the
 * dragon's building strike: they pass above the roofs they reach over, and
 * letting a building swallow them reads as the effect vanishing mid-animation.
 * Erring toward visible is the right bias here; a projectile that should have
 * been hidden is a far smaller error than one that disappears. */
bool Sim3D_HeightClassIsOccludable(SimHeightClass height_class);

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

/* Stages with a shipped implementation. `Sim3D_ImplementedFeatures`
 * reports these once a frame captures cleanly; the settings menu uses the
 * constant directly so a stage can still be configured outside a town, where
 * nothing has been captured yet. Extend it as each visual gate passes. */
enum {
  kSim3DShippedFeatures =
      kSimFeature_SeparatedComposite | kSimFeature_GroundProjection |
      kSimFeature_ObjectBillboards | kSimFeature_VirtualHeight |
      kSimFeature_Shadows | kSimFeature_SoftShadows | kSimFeature_RimLight |
      kSimFeature_WorldUnderlay | kSimFeature_CloudShroud |
      kSimFeature_CullHaze | kSimFeature_Backdrop |
      kSimFeature_EffectLighting | kSimFeature_Particles,
};

/* Default D4a shadow darkness, percent of full black. The landscape pass has
 * broad overlapping casters, so a softer value preserves terrain colour. */
enum { kSimShadowOpacityDefaultPct = 45 };
/* Default light: near-overhead, thrown slightly to screen right. */
enum {
  kSimLightAzimuthDefaultDeg = 90,
  kSimLightElevationDefaultDeg = 90,
  kSimShadowSoftnessDefaultPct = 50,
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
   * The shipped default is 10 anyway, chosen by looking at it. The focus
   * falloff landed after that reasoning and now carries much of the same
   * distinction: with the far field defocused, a partial fade no longer reads
   * as a smeared gradient, and keeping the town's own detail out there is
   * worth more than the theoretical two-tier purity. 10 is a light touch --
   * just enough to mark the boundary, with the defocus and the out-of-range
   * darkening doing the rest of the near/far separation. Set it to 100 to get
   * the argued behaviour back.
   *
   * The ramp used to default to 208 on the argument that a step reads as a
   * hard line across the ground, worse than the patchy cover it replaces.
   * That held while the fade was the only thing separating near from far, but
   * the focus falloff and out-of-range darkening now carry that separation,
   * and a 208px ramp spends most of its length dimming ground the player is
   * actively working in. 16 (the minimum, one ramp step) keeps the town's own
   * vision range bright and lets the fade do its job right at the edge. */
  kSimCullHazeDefaultPct = 10,
  kSimCullHazeLeadDefaultPx = 16,
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
  kSimBackdropStrengthDefaultPct = kPercentScale,
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
  kSimCloudDriftDefaultPct = kPercentScale,
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
  kSimMetadataIntegrity_PartContract = 1u << 8,
} SimMetadataIntegrityFlag;

typedef uint8_t (*SimEruptionScriptFetch)(void *context, uint16_t address);

typedef struct SimEruptionFlightPlan {
  /* Map pixels of descent left between the record's cursor and the script's
   * landing command, counted forward one $03 at a time. Authored, and it only
   * ever decreases. Zero when the walk did not resolve a landing. */
  int fall_pixels;
  /* The crater mouth, from the last position command before the cursor that
   * placed the actor ON the map. Authored per town, so it is exact from the
   * first frame of the event rather than learned by watching. Best-effort:
   * the walk that finds it starts at the script base, and a script that has
   * already looped past a jump cannot be replayed, which is why the arc keeps
   * a learned fallback for the crater and none for the descent. */
  int16_t crater_x, crater_y;
  /* Where this fireball is going, read AHEAD of the record getting there: the
   * column and the row above the map come from the staging teleport still in
   * front of the cursor, the landing row from the descent that follows it.
   * Valid only while that teleport has not executed yet -- which is exactly
   * the climb, and exactly when the arc needs to already know where it is
   * throwing. Once the record is descending the walk cannot see the teleport
   * any more and the arc flies the snapshot it took earlier. */
  int16_t landing_x, landing_y;
  /* The row the staging teleport parks the record on, measured at -16 across
   * every captured run. The descent runs from here to `landing_y`, and that
   * span is what the second half of the throw is parameterised by. */
  int16_t entry_y;
  /* Frames from here to the landing command: the wait currently running, read
   * live out of +$22, plus one frame for every command still ahead and the
   * full operand of every wait not yet reached.
   *
   * This is the ONLY clock the throw runs on, and it is the one quantity that
   * spans all three of the ROM's phases. The record's own position cannot:
   * it freezes for the 76-frame countdown between the climb and the descent,
   * which is where a position-driven arc has to hide the fireball and then
   * bring it back. +$22 is what makes that countdown legible -- during a wait
   * the cursor has already advanced PAST the $09, so walking the script alone
   * counts the wait as either its whole operand or nothing at all. */
  int frames_to_land;
  uint8_t valid;
  uint8_t crater_valid;
  uint8_t landing_valid;
} SimEruptionFlightPlan;

typedef struct SimSourceRecord {
  uint16_t record_address;
  uint16_t composition;
  uint16_t world_x, world_y;
  uint16_t type;
  uint16_t semantic_state;
  /* Raw +$06 is polymorphic. Ordinary world actors use it as flags; the
   * town-creation world processes retain animation script base $A8BB here.
   * Capture the source fact and let exact semantic classifiers interpret it. */
  uint16_t record_word06;
  uint16_t status;
  uint16_t oam_first;
  uint8_t oam_count;
  uint16_t fragment_first;
  uint16_t fragment_count;
  uint8_t tier;
  uint8_t alternate_attributes;
  /* Palettes selected by the parts this record actually emitted. The same
   * composition address can name different runtime-built art: scripted red
   * fire and post-Lightning blue fire both use $E6CA/$E6D0/$E6D6, but their
   * OAM parts select palettes 1 and 2 respectively. Keep that distinction at
   * the producer boundary instead of asking presentation to infer colour from
   * a pointer or from the shared CGRAM. */
  uint8_t obj_palette_mask;
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
  /* The flight this record's own script authors, handed over by the producer
   * that can reach the cart. Zeroed for everything that is not an eruption
   * fireball. */
  SimEruptionFlightPlan flight;
  uint8_t clip_reason;
  uint16_t clipped_parts;
  uint16_t synthetic_parts;
} SimSourceRecord;

typedef struct SimRenderObject {
  uint16_t record_address;
  uint16_t composition;
  uint16_t world_x, world_y;
  uint16_t type;
  uint16_t semantic_state;
  uint16_t oam_first;
  uint8_t oam_count;
  /* Complete exact parts in emitter order. The atlas uses these only when the
   * count proves every OAM-backed and synthetic part in this fragment arrived. */
  uint16_t part_first;
  uint8_t part_count;
  uint8_t synthetic_part_count;
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
  /* Signed presentation offset from the record's authentic map position, in
   * authentic pixels. Zero for everything the ROM places itself; non-zero
   * only where a presentation stage is deliberately moving art the ROM had
   * nowhere better to put, and always converging back to zero before the
   * record's own logic acts on its position. */
  int16_t offset_x, offset_y;
  /* Set by a presentation stage for art the ROM emits but the enhanced view
   * must not draw. The eruption's spawn queue is the motivating case: the ROM
   * parks unlaunched fireballs one row above the map, which is off-screen in
   * the authentic 2D window but hangs in open sky once the town is projected. */
  uint8_t hidden;
} SimRenderObject;

typedef enum SimEffectKind {
  kSimEffect_None = 0,
  kSimEffect_LightningMiracle,
  kSimEffect_BlueDragonLightning,
  kSimEffect_TownCreationLightning,
  kSimEffect_RedDemonFire,
  kSimEffect_GroundFire,
  kSimEffect_HouseFire,
  /* Volcanic eruption story event, measured in run 20260818-070141. The
   * airborne fireball and the fire it leaves on the ground are separate kinds
   * because only the fireball moves; the ground fire deliberately shares the
   * house fire's phase family and lighting ramp. */
  kSimEffect_VolcanoFireball,
  kSimEffect_VolcanoGroundFire,
} SimEffectKind;

typedef enum SimEffectColorFamily {
  kSimEffectColor_None = 0,
  kSimEffectColor_LightningBlue,
  kSimEffectColor_FireRed,
  kSimEffectColor_FireBlue,
} SimEffectColorFamily;

typedef enum SimEffectPhase {
  kSimEffectPhase_None = 0,
  kSimEffectPhase_LightningCloud,
  kSimEffectPhase_LightningLead,
  kSimEffectPhase_LightningBranch,
  kSimEffectPhase_LightningImpactA,
  kSimEffectPhase_LightningImpactB,
  kSimEffectPhase_BlueDragonAttack,
  kSimEffectPhase_BlueDragonBoltA,
  kSimEffectPhase_BlueDragonBoltB,
  kSimEffectPhase_BlueDragonBoltC,
  kSimEffectPhase_TownCreationGap,
  kSimEffectPhase_TownCreationBoltA,
  kSimEffectPhase_TownCreationBoltB,
  kSimEffectPhase_TownCreationBoltC,
  kSimEffectPhase_TownCreationBoltD,
  kSimEffectPhase_RedDemonAttack,
  kSimEffectPhase_RedFireSmall,
  kSimEffectPhase_RedFireMedium,
  kSimEffectPhase_RedFireLarge,
  kSimEffectPhase_GroundFireA,
  kSimEffectPhase_GroundFireB,
  kSimEffectPhase_GroundFireC,
  kSimEffectPhase_HouseFireA,
  kSimEffectPhase_HouseFireB,
  kSimEffectPhase_HouseFireC,
  /* The two held eruption fireball frames. Named A/B after their authored
   * order ($01:A853 then $01:A857) rather than rise/fall: the art's vertical
   * flip and the record's motion disagree, and nothing measured pins which
   * way the ROM intends the blob to point. */
  kSimEffectPhase_VolcanoFireballA,
  kSimEffectPhase_VolcanoFireballB,
} SimEffectPhase;

typedef enum SimEffectGeometryKind {
  kSimEffectGeometry_None = 0,
  kSimEffectGeometry_Point,
  kSimEffectGeometry_Segment,
  kSimEffectGeometry_Area,
  kSimEffectGeometry_Scene,
} SimEffectGeometryKind;

typedef enum SimEffectGeometrySpace {
  kSimEffectSpace_RecordLocal = 0,
  kSimEffectSpace_WorldLocal,
  kSimEffectSpace_Screen,
} SimEffectGeometrySpace;

typedef enum SimEffectFlag {
  kSimEffectFlag_Visible = 1u << 0,
  kSimEffectFlag_UserLifecycle = 1u << 1,
  kSimEffectFlag_PostedLifecycle = 1u << 2,
  kSimEffectFlag_VisualComplete = 1u << 3,
  kSimEffectFlag_ActorDone = 1u << 4,
  kSimEffectFlag_RecordLifecycle = 1u << 5,
} SimEffectFlag;

typedef struct SimEffectLocalPoint {
  int16_t x, y;
  /* Authentic pixels above the projected ground. Zero is on the map plane. */
  int16_t height;
} SimEffectLocalPoint;

enum {
  /* Retained path length. Sized so a whole eruption throw fits: the longest
   * measured flight is 93 script frames, which is about 130 producer builds,
   * and 32 samples at a stride of 4 spans 128. Short enough that the renderer
   * can still afford a puff or two per sample. */
  kSimEffectTrailSamples = 32,
  /* Producer builds between retained samples. The head (index 0) is always
   * this tick's position, so the fireball's flame stays on the fireball; only
   * the tail behind it is thinned. Without a stride the path would cover 32
   * builds of a 130-build flight and the smoke would be a stub rather than
   * the trajectory. */
  kSimEffectTrailStride = 4,
  /* Producer builds a travelling effect runs before it starts leaving a path.
   *
   * Every fireball in the fountain launches from the SAME point -- the
   * crater mouth -- and a trail's oldest samples carry its biggest, most
   * spread-out puffs, so eight throws pile eight clouds of smoke on the one
   * pixel the volcano is meant to be erupting out of. Holding retention back
   * for the first stretch of a flight starts each path a little way along its
   * arc and leaves the crater clear.
   *
   * A LOOK DIAL, and it only moves the smoke: the fireball itself is drawn
   * from the effect's live position, so it still leaves the crater. Because
   * the ring holds 128 builds and no flight is longer, the gap persists for
   * the whole flight rather than closing up as the path fills.
   *
   * The delay is JITTERED per throw over `Jitter` further builds, so the
   * eight paths do not all begin at the same radius and draw a clean ring
   * around the mouth. Measured over the captured eruption, the base alone
   * starts a tail a median 62 authentic pixels up and 19 along; the top of
   * the range about 97 and 42. */
  kSimEffectTrailLaunchDelay = 24,
  kSimEffectTrailLaunchJitter = 24,
};

/* How the volcanic eruption's fireballs are pathed.
 *
 * The ROM has no altitude to reproduce: the sim town is a flat top-down map,
 * and a fireball is a sprite translating across it at a constant 8 pixels a
 * tick (run 20260818-073455). Which of these applies is decided by the view
 * and is deliberately NOT a separate setting. A flat path drawn inside a
 * projected town is faithful to nothing -- it is the 2D motion of a top-down
 * map shown from an angle that reveals it has no height, which is exactly the
 * "sliding along the ground" the fountain exists to fix. The authentic
 * picture is the authentic VIEW, which the player already selects by turning
 * the enhanced town off. */
typedef enum SimEruptionPath {
  /* Exactly the ROM's own map motion: no height, no launch offset, and the
   * staged spawn queue left wherever the ROM parks it. Every view that
   * presents the ROM's own framebuffer gets this. */
  kSimEruptionPath_Authentic = 0,
  /* A fountain in world space. Fireballs leave the crater, arc over the town
   * and converge onto their authentic landing cell. Enhanced view only. */
  kSimEruptionPath_Ballistic,
  kSimEruptionPath_Count,
} SimEruptionPath;

/* One byte of a class-$01 actor script, by absolute bank-$0A address. Pure
 * indirection so the walkers below can be driven from the live cart in the
 * sprite hook and from captured script bytes in a test. */

/* Resolve the plan above for the record sitting at `cursor`.
 *
 * The descent is walked FORWARD from the cursor and always answered; the
 * crater is walked backward from `base` and answered only when that replay
 * succeeds. The two are deliberately independent, because they fail for
 * different reasons: the forward walk is a straight run to the landing
 * command, while the backward walk cannot cross the jump that makes the
 * script loop, so a record on its second pass has a readable descent and an
 * unreadable crater. Tying the descent to the crater's success is what made
 * an earlier arc resolve for six fireballs out of a fountain of thirty. */
/* Which authored eruption fireball frame a composition is, or
 * kSimEffectPhase_None. Published so the renderer can find the art for one
 * fireball on another fireball's object: every one of them wears the same two
 * compositions, the atlas repacks every frame so a rectangle cannot be cached
 * across frames, and a record whose own art the sprite window dropped can
 * still borrow an identical entry from a sibling in the same frame. */
SimEffectPhase Sim3D_VolcanoFireballPhase(uint16_t composition);

SimEruptionFlightPlan SimEruptionScript_ResolveFlight(
    SimEruptionScriptFetch fetch, void *context, uint16_t base,
    uint16_t cursor, int wait_frames);

/* The eruption fireball's presentation arc, published because it is part of
 * the effect/object height contract rather than an internal tuning detail:
 * the enhanced view supplies this altitude outright, since the sim town is a
 * flat top-down map with no authentic height for the family to carry.
 *
 * WHAT THE ARC REPLACES. The ROM runs the eruption in three phases: a
 * fireball is thrown up out of the crater, it leaves the top of the map and
 * is parked one row above it, and then it drops straight down the column it
 * is due to land in. Only the third phase is on screen for most of its life,
 * and drawn honestly it is a vertical drop out of the sky -- which is the
 * complaint this exists to answer. The projected view therefore draws NONE of
 * the three: it draws one throw from the crater to the impact cell and hides
 * the record until that throw starts.
 *
 * WHAT IT HAS TO MATCH. Two things, and only two: how many fireballs land,
 * and where. Both hold by construction here. Every record that resolves a
 * descent flies exactly one throw, so the count is the ROM's own; the throw
 * is parameterised by the descent left and ends at the cell the script lands
 * on, so the arc reaches zero offset and zero height on the landing frame and
 * the ground fire can never appear where the fireball was not.
 *
 * WHY THE CLOCK IS PIXELS. The descent is measured in map pixels of $03 and
 * nothing else. An earlier attempt drove the same curve from a frame count
 * taken over the whole script while sizing it by the descent alone; the two
 * were different quantities, the progress term pinned at zero for most of the
 * flight, and the fireball sat at the crater and then snapped. One authored
 * quantity, used for both ends, is what makes that unrepresentable. */
enum {
  /* THE THROW IS SIZED BY ITS FLIGHT TIME, NOT BY ITS DISTANCE.
   *
   * The ROM does not throw a fireball and let it fly. It throws one, parks it
   * above the map for a countdown, and then drops it -- and the countdown is
   * most of the life cycle: 76 frames of a 93-frame flight on the measured
   * record. An arc that only covers the two moving phases therefore has to
   * hide the fireball for two thirds of its life and bring it back at the
   * apex, which is the disappear-and-reappear it used to show.
   *
   * So the whole flight time is given to the arc and the LAUNCH is sized to
   * fill it. That is what a thrown thing actually does: with gravity fixed,
   * the time of flight sets the apex and the apex sets the time, so
   * apex = T^2 / k. A fireball with a long countdown ahead of it is simply
   * lobbed higher and travels slower, and the countdown is spent climbing
   * instead of parked. Distance plays no part -- horizontal speed covers
   * that, exactly as it does in the real thing, which is also why two
   * fireballs launched together to different cells look like one volley.
   *
   * The divisor is the gravity term and is THE DIAL: chosen so the measured
   * 93-frame flight peaks near 150 map pixels, well clear of the town and
   * readable from a low camera. */
  kSimEruptionArcGravityDivisor = 58,
  /* Floor and ceiling in authentic pixels above the map plane. The floor is
   * deliberately far below the shortest real throw: an earlier floor of 80
   * overrode the sizing on sixteen of the captured eruption's 24 throws,
   * reaching five times the whole ground track on the shortest, which is not
   * an arc but a rocket -- and that is what read on screen as a jet straight
   * up out of the crater followed by a fireball dropping straight down. */
  kSimEruptionArcApexMin = 24,
  kSimEruptionArcApexMax = 320,
  /* The crater MOUTH, relative to the cell the script launches from.
   *
   * The script's crater is a flat map cell, (144,128) in every eruption
   * script. The volcano the player sees is the six-cell `kAitosVolcano`
   * mountain stamp standing on that cell, and its lava blob is up at the
   * summit -- so a fireball leaving the authored cell leaves the mountain's
   * foot, not its mouth. Both numbers come from the model rather than from
   * taste, using the mapping `MountainPlanePoint` uses to place the crater
   * glow itself:
   *
   *   stamp cell_y 8, six cells tall -> baseline source row (8+6)*16 = 224
   *   crater centre 8*16 + kCraterCentreOffsetY(11)                  = 139
   *   rise = 224 - 139                                               = 85
   *   lift = rise * face_height_scale(0.30) * kVolcanoHeightScale(1.12) = 28
   *   drawn row = 224 - rise * face_depth_scale(0.62)                = 171
   *   drop = 171 - the script's own crater row (128)                 = 43
   *
   * The relief raises the summit and pushes it down-map at the same time, so
   * both are needed or the launch sits behind the mountain it is coming out
   * of. Note the drop is measured from the SCRIPT's crater row, not from the
   * mountain baseline the relief is measured against; taking it from the
   * baseline put the launch ten pixels below the mouth, which showed up from
   * the default camera and not from a horizontal one -- pitch is what decides
   * how much of a map-row error reaches the screen.
   *
   * This whole derivation is only the FALLBACK. When the projected view is
   * running it reports the mouth it actually drew, through
   * SimRenderMetadata_SetEruptionCraterAnchor, and that is exact including
   * the camera-facing lean this cannot model. Retune the pair together with
   * the volcano model, never separately. */
  kSimEruptionCraterLift = 28,
  kSimEruptionCraterDrop = 43,
  /* Never draw the eruption fireball's billboard where the ROM put it.
   *
   * Not a debugging switch and not a loss of art: the projected view replaces
   * the ROM's three-phase fireball routine outright, so the record's own
   * position is the wrong place for a sprite and drawing there puts a second,
   * contradictory fireball on screen. The ART is reused --
   * DrawSimEffectFireballHeads redraws the same atlas entry at the arc head,
   * turned onto the heading -- which is why this withholds a PLACEMENT rather
   * than suppressing a graphic. Named rather than inlined so the object pass
   * says why it hides something unconditionally. */
  kSimEruptionWithholdFireballBillboard = 1,
};

/* One retained world position from an earlier logic tick, in the same
 * authentic town pixels as SimEffectInstance::world_x/world_y. Index 0 is the
 * position this tick and index i is exactly i ticks ago:
 * samples are pushed once per producer build, never on a repeated capture of
 * the same immutable build, and the whole path is dropped whenever lifetime
 * continuity breaks -- so a recycled record slot cannot inherit a stranger's
 * path and a renderer never has to infer motion from a wall clock. */
typedef struct SimEffectTrailPoint {
  uint16_t world_x, world_y;
  /* Authentic pixels above the map plane at that tick, so a tail left by
   * something in flight follows the arc through the air instead of being
   * smeared along the ground under it. */
  int16_t height;
} SimEffectTrailPoint;

typedef struct SimEffectGeometry {
  uint8_t kind;
  uint8_t space;
  union {
    SimEffectLocalPoint point;
    struct {
      SimEffectLocalPoint start, end;
    } segment;
    struct {
      int16_t x, y, width, height, elevation;
    } area;
  } data;
} SimEffectGeometry;

/* Presentation-ready semantic emitter, derived on the game thread from one
 * captured source record. The record address is only a slot; generation makes
 * recycled occupants distinct. `age_ticks` and `phase_ticks` are authentic
 * logic-tick ages and are idempotent when one build is captured more than once.
 * Pulse fields identify a contiguous visible emission within the longer
 * lifecycle, so renderer particles never infer birth from a wall/global clock.
 * World position remains in authentic town pixels and geometry is local to it. */
typedef struct SimEffectInstance {
  uint32_t generation;
  uint32_t pulse_generation;
  uint16_t record_address;
  uint16_t composition;
  uint16_t world_x, world_y;
  uint16_t age_ticks;
  uint16_t phase_ticks;
  uint16_t pulse_ticks;
  uint16_t ticks_since_visible;
  SimEffectGeometry geometry;
  /* Recent published path, newest first, valid for trail_count entries. Index
   * 0 is this tick; index n is about n * kSimEffectTrailStride ticks old, and
   * nothing is retained for the first kSimEffectTrailLaunchDelay ticks. Only
   * kinds whose art actually travels populate it; a stationary emitter leaves
   * trail_count zero rather than publishing a pile of identical points. */
  SimEffectTrailPoint trail[kSimEffectTrailSamples];
  uint8_t trail_count;
  /* Direction this effect is travelling, in authentic map pixels plus height,
   * per unit of flight progress. Published so the renderer can project it and
   * turn art onto the trajectory -- a fireball drawn upright while flying a
   * ballistic arc reads as a sprite being slid around rather than thrown.
   *
   * On the EFFECT rather than on the object because the art that uses it is
   * drawn from the effect: the eruption's billboard is withheld and its
   * sprite is placed at the arc head instead, which is also the only place
   * where the heading and the smoke behind it are guaranteed to agree. Zero
   * for everything the ROM places, which is drawn upright as always. */
  int16_t travel_x, travel_y, travel_height;
  uint8_t travel_valid;
  uint8_t source_index;
  uint8_t kind;
  uint8_t phase;
  uint8_t color_family;
  uint8_t flags;
} SimEffectInstance;

bool Sim3D_IsLightningMiracleComposition(uint16_t composition);
const char *Sim3D_EffectKindName(SimEffectKind kind);
const char *Sim3D_EffectPhaseName(SimEffectPhase phase);
const char *Sim3D_EffectColorName(SimEffectColorFamily color);
const char *Sim3D_EffectGeometryName(SimEffectGeometryKind kind);
const char *Sim3D_EffectSpaceName(SimEffectGeometrySpace space);

/* D4a caster selection. Kept beside the classifier rather than in the renderer
 * so "what casts a shadow" stays one data question: a world-tier object with
 * usable atlas art that D3c did not mark MapPlane or NoShadow. Ground-anchored
 * classes still cast — a zero height simply puts the silhouette under the
 * actor's own feet. */
/* Cloud-shroud cover at one point, 0..1.
 *
 * Pure, and deliberately separate from the renderer: the shroud explains the
 * boundary of the complete real-OAM plus synthetic-part actor channel, so
 * "how covered is this point" is a statement about the sprite-drawable
 * rectangle, not about clouds.
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
                          int margin_left, int margin_right,
                          int margin_top, int margin_bottom,
                          int lead, int corner, int lift_inset);

/* Whether a record should carry cover this frame, and how much.
 *
 * A record earns cover when the sprite window is what removed it, never when
 * the game did: a composition the ROM declined to draw, a record outside the
 * finite town, or a destroyed projectile is legitimately absent and putting a
 * cloud over it would assert something false. Returns 0 for those. */
float Sim3D_SourceCullCover(const SimSourceRecord *source,
                            int margin_left, int margin_right,
                            int margin_top, int margin_bottom,
                            int lead, int corner, int lift_inset);

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
  /* Raw lifecycle words stay diagnostic; effects[] below is the only
   * presentation-facing interpretation. */
  uint16_t miracle_kind;
  uint16_t miracle_user_active;
  uint16_t miracle_posted_active;
  uint16_t miracle_visual_complete;
  uint16_t miracle_actor_done;
  bool world_navigation_state_valid;
  SimWorldNavigationFrame world_navigation;
  /* INIDISP master brightness captured with the navigation OAM composition.
   * Partial values remain enhanced frames: presentation fades the complete
   * host world before drawing Palace/UI pixels that the PPU rasterizer has
   * already brightness-adjusted. */
  uint8_t world_navigation_brightness;
  /* Full-world scene derived during capture from the state above and the owned
   * developed-map serial. Presentation consumes this value copy; invalid means
   * authentic Mode 7 must own the frame. */
  SimWorldNavigationScene world_navigation_scene;
  uint32_t build_serial;
  uint32_t integrity_flags;
  bool atlas_valid;
  uint16_t atlas_width, atlas_height;
  uint16_t atlas_used_width, atlas_used_height;
  bool separated_valid;
  uint8_t separated_status;
  /* Physical CPU planes authored for this frame. Logical OBJ ranks remain in
   * diagnostic_layer_mask when semantic billboards replace their raw pixels;
   * this mask exists to prevent uploads or fallback draws from sampling an
   * unproduced (and therefore potentially stale) surface. */
  uint16_t separated_plane_mask;
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
  /* Simulation-town perspective camera only. World navigation keeps these
   * zero and consumes world_navigation_scene's forced top-down affine map. */
  int16_t projection_pitch_mrad, projection_yaw_mrad;
  uint16_t projection_distance_x100;
  /* Audited town relief magnitude, independent of actor/model height. */
  uint16_t landscape_height_pct;
  /* Resolved presentation tuning: percent of each classified virtual height.
   * Copied here so one frame cannot mix old and new tuning values. */
  uint16_t height_scale_x100;
  /* Resolved false for the Off preset. The game thread then skips extraction
   * and publishes serial zero, preserving the authentic background tiles. */
  uint8_t background_voxel_enabled;
  /* Holds every windmill in the town for the "no wind" event rather than only
   * the ones the event stamped. Extras enhancement; see ledger §61. */
  uint8_t background_voxel_wind_hold;
  /* Player-selected procedural background-model performance target. */
  uint8_t background_voxel_detail;
  /* Fixed keeps that detail for every model. Adaptive treats it as a ceiling
   * and selects a cheaper cached mesh for small projected silhouettes. */
  uint8_t background_voxel_lod;
  /* Independent background-model cost classes. Keeping them in the immutable
   * frame payload avoids reading live settings from the render thread. */
  uint8_t background_voxel_shading;
  uint8_t background_voxel_style;
  uint8_t background_voxel_facing;
  uint8_t background_voxel_render_scale;
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
  int16_t cloud_clear_x0, cloud_clear_x1;
  int16_t cloud_clear_y0, cloud_clear_y1;
  /* The live widescreen margins the emitter used this frame, published so the
   * renderer can evaluate the cull predicate on the same window the emitter
   * did rather than reconstructing it from cloud_clear_*. */
  int16_t sprite_margin_left, sprite_margin_right;
  int16_t sprite_margin_top, sprite_margin_bottom;
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
  /* Independent $09 effect gates. Compatible numeric atmosphere tuning is
   * shared, but these never depend on the simulation-town master. */
  uint8_t world_navigation_lighting;
  uint8_t world_navigation_clouds;
  uint8_t world_navigation_backdrop;
  uint8_t world_navigation_haze;
  /* Whether the lit window's bottom edge is inset by the maximum draw lift. */
  uint8_t cull_lift_inset;
  /* Atmospheric backdrop gradient strength, percent. */
  uint8_t backdrop_strength_pct;
  /* Synthetic horizon height, percent of viewport from the top. */
  uint8_t backdrop_horizon_pct;
  /* Resolved world-underlay placement. `underlay_serial` changes whenever the
   * baked image would differ, so the presentation path rebuilds its texture
   * from the frame it is drawing rather than from live module state. Zero
   * means the module has nothing usable and the layer must not draw. */
  uint32_t underlay_serial;
  uint8_t underlay_origin_tile_x, underlay_origin_tile_y;
  /* Persistent full-resolution town ground accumulated from verified frames.
   * Changes whenever the canvas image does; zero means nothing to draw. */
  uint32_t town_canvas_serial;
  /* Presentation-only building/tree extraction derived from that canvas.
   * The scene and pixel buffers remain module-owned; this serial prevents a
   * queued frame from sampling a different build. */
  uint32_t background_voxel_serial;
  /* Authentic-pixel column of the captured texture that holds SNES x = 0.
   * Resolved on the game thread so present-time code never re-derives the
   * widescreen margin width. */
  uint16_t underlay_screen_x0;
  uint16_t emitted_oam_count;
  uint16_t claimed_oam_count;
  uint16_t synthetic_part_count;
  uint16_t synthetic_part_overflow_count;
  uint8_t world_oam_first, world_oam_count;
  uint8_t world_record_occupancy;
  uint8_t source_count;
  uint8_t zero_oam_source_count;
  uint16_t object_count;
  bool effect_metadata_valid;
  uint8_t effect_count;
  uint8_t effect_visible_count;
  uint8_t effect_overflow_count;
  SimSourceRecord sources[kSimMaxSourceRecords];
  SimRenderObject objects[kSimMaxRenderObjects];
  SimEffectInstance effects[kSimMaxEffectInstances];
} SimFrameData;

/* Game-thread handoff between the semantic record producer and the atlas
 * builder. The builder works on a value copy, then atomically commits only if
 * the producer serial/count still match. */
typedef struct SimAtlasBuildInput {
  uint32_t build_serial;
  uint16_t object_count;
  uint16_t part_count;
  SimRenderObject objects[kSimMaxRenderObjects];
  SrPpuObjPart parts[kSimMaxResolvedParts];
} SimAtlasBuildInput;

/* Pure dependency resolver.  implemented_features is a capability mask, not
 * a user request.  D1 intentionally passes zero so every visual path remains
 * authentic while requested/effective diagnostics are exercised. */
SimRenderFeatureMask Sim3D_ResolveFeatureMask(
    SimRenderFeatureMask requested_features,
    SimRenderFeatureMask implemented_features,
    SimViewKind view, bool master_enabled, bool metadata_valid);

/* Game-thread producer API used by the faithful SIM composition leaves. */
/* True when this call began a new per-frame build. */
bool SimRenderMetadata_BeginRecord(
    uint16_t record_address, bool world_record, bool alternate_attributes,
    uint16_t composition, uint16_t world_x, uint16_t world_y,
    uint16_t type, uint16_t semantic_state, uint16_t status,
    uint16_t oam_cursor_before);
/* Publish the open record's raw +$06 word. Kept separate from BeginRecord so
 * older synthetic producers that do not model polymorphic fields default to
 * zero and therefore fail closed for classifiers that require it. */
void SimRenderMetadata_RecordWord06(uint16_t value);


/* The flight this record's script authors, resolved by the producer that can
 * reach the cart. Handed over rather than read here so this unit stays free of
 * the CPU/cart plumbing, exactly as the velocity above is. */
void SimRenderMetadata_RecordFlightPlan(SimEruptionFlightPlan plan);

/* Where the projected view actually drew the volcano's crater mouth, in
 * authentic map pixels plus a height above the map plane.
 *
 * Handed over by the renderer, which is the only place that knows: the mouth
 * belongs to a voxel mountain model whose relief raises the summit, pushes it
 * down-map, and leans it toward the camera. Deriving it here from the stamp's
 * authored geometry gets close and then drifts the moment any of those is
 * retuned -- so the arc asks the model instead, and the model answers with the
 * same point it drew the crater glow and the smoke plume on.
 *
 * One frame stale by construction, which is invisible: the mouth only moves
 * when the camera does. `valid` false restores the derived fallback, so a
 * town with no volcano, or a detail level that draws no mountains, still
 * launches from somewhere sensible. */
void SimRenderMetadata_SetEruptionCraterAnchor(
    bool valid, int16_t map_x, int16_t map_y, int16_t height);
/* The emitter's biased composition origin for the record now open. Separate
 * from BeginRecord so the D1 producer contract and its callers are unchanged;
 * a record without this call simply has no cull-lead anchor. */
void SimRenderMetadata_RecordAnchor(int16_t base_x, int16_t base_y);
void SimRenderMetadata_RecordPart(uint16_t oam_cursor, uint16_t attributes);
void SimRenderMetadata_RecordExactOamPart(const SrPpuObjPart *part);
void SimRenderMetadata_RecordSyntheticPart(uint16_t oam_cursor,
                                           const SrPpuObjPart *part);
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
    SimFrameData *dst, const uint8 *wram, bool town_master_enabled,
    bool world_navigation_enabled,
    SimRenderFeatureMask requested_features,
    uint32_t diagnostic_layer_mask,
    SimRenderFeatureMask implemented_features);

/* Deterministic metadata evidence.  The trace is inert unless
 * AR_SIM3D_D1_TRACE names an output JSONL file. */
void SimRenderMetadata_TraceFrame(uint32_t host_frame,
                                  const SimFrameData *frame,
                                  const uint8_t *rgba, int width, int height,
                                  int pitch);
/* True when that file is open, so producers can skip diagnostic-only work
 * nothing is going to read. Safe to call before the first TraceFrame. */
bool SimRenderMetadata_TraceArmed(void);
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
