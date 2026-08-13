#include "diorama_layer_editor.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"

/* ── level tabs ──────────────────────────────────────────────────────────
 *
 * One tab per ACTION map group. Deliberately explicit rather than derived from
 * the group number: the six kingdoms are ordered, but Death Heim is not a
 * kingdom and the ending group has no diorama at all, so a formula would have
 * to special-case both anyway. */
static const struct {
  uint8_t group;
  const char *name;
} kEditorLevels[kDioramaEditorLevelCount] = {
  { kActRaiserMapGroup_Fillmore,  "Fillmore" },
  { kActRaiserMapGroup_Bloodpool, "Bloodpool" },
  { kActRaiserMapGroup_Kasandora, "Kasandora" },
  { kActRaiserMapGroup_Aitos,     "Aitos" },
  { kActRaiserMapGroup_Marahna,   "Marahna" },
  { kActRaiserMapGroup_Northwall, "Northwall" },
  { kActRaiserMapGroup_DeathHeim, "Death Heim" },
};

uint8_t DioramaLayerEditor_LevelGroup(int level_index) {
  if (level_index < 0 || level_index >= kDioramaEditorLevelCount)
    return kActRaiserMapGroup_Fillmore;
  return kEditorLevels[level_index].group;
}

const char *DioramaLayerEditor_LevelName(int level_index) {
  if (level_index < 0 || level_index >= kDioramaEditorLevelCount) return "";
  return kEditorLevels[level_index].name;
}

int DioramaLayerEditor_LevelIndexOfGroup(uint8_t map_group) {
  for (int i = 0; i < kDioramaEditorLevelCount; i++)
    if (kEditorLevels[i].group == map_group) return i;
  return -1;
}

/* ── strategy authoring ──────────────────────────────────────────────────
 *
 * Starting magnitudes for a shape the plane did not previously carry. Chosen so
 * one keypress is VISIBLE: authoring a shape with depth 0 would look like the
 * editor did nothing, and the player would reasonably conclude the feature is
 * broken rather than that they now need to find the depth row.
 *
 * 0.29 is not arbitrary -- it is the value the shipped manifest already uses
 * for Fillmore act 2's water (diorama-layers.ini), i.e. the one magnitude known
 * to close a real gap in a real room. A thickness starts smaller because a
 * skirt that deep reads as a wall rather than an edge. */
enum { kEditorParamStepPermille = 10 };  /* 0.01 per press on a depth key */
static const float kEditorDefaultTilt = 0.29f;
static const float kEditorDefaultThickness = 0.20f;
static const float kEditorDefaultStack = 0.29f;
static const float kEditorDefaultVoxel = 0.18f;
static const float kEditorDefaultDensity = 14.0f;

DioramaDepthStrategy DioramaLayerEditor_StrategyOfPlane(
    const DioramaPlaneOverride *p) {
  if (!p) return kDioramaDepth_Flat;
  /* The same precedence DioramaLayerOrder_StrategyOf applies to a resolved
   * layer, expressed against the authored keys. Kept in step with it
   * deliberately: if the two disagreed, the row would name one shape and the
   * renderer would draw another.
   *
   * Note the asymmetry with the resolved form: there, a voxel and a stack have
   * both collapsed onto `stack` + stack_solid, so it tests stack_solid. Here
   * they are still distinct keys, so it tests set_voxel. */
  if (p->set_voxel && p->voxel > 0.0f) return kDioramaDepth_Voxel;
  if (p->set_stack && p->stack > 0.0f) return kDioramaDepth_Stack;
  if (p->set_thickness && p->thickness > 0.0f) return kDioramaDepth_Thick;
  if (p->set_bow && p->bow != 0.0f) return kDioramaDepth_Bow;
  if (p->set_rake && p->rake != 0.0f) return kDioramaDepth_Rake;
  return kDioramaDepth_Flat;
}

/* Drop every depth-shape key, leaving order/z/alpha (which are not shapes and
 * which the player tuned separately) untouched.
 *
 * Clearing the VALUE as well as the flag is deliberate and load-bearing. A
 * cleared flag with a stale value behind it round-trips through
 * FormatRoom/ParseLine as absent, so the file is fine -- but the next
 * CycleStrategy back to that shape would silently resurrect the old magnitude
 * instead of the starting one, which reads as the editor remembering something
 * the player cannot see. Zeroing makes "cycle away and back" idempotent. */
static void ClearShapeKeys(DioramaPlaneOverride *p) {
  p->set_rake = false;           p->rake = 0.0f;
  p->set_bow = false;            p->bow = 0.0f;
  p->set_thickness = false;      p->thickness = 0.0f;
  p->set_stack = false;          p->stack = 0.0f;
  p->set_stack_copies = false;   p->stack_copies = 0;
  p->set_stack_density = false;  p->stack_density = 0.0f;
  p->set_stack_direction = false; p->stack_direction = kDioramaStack_Forward;
  p->set_voxel = false;          p->voxel = 0.0f;
  p->set_voxel_copies = false;   p->voxel_copies = 0;
}

void DioramaLayerEditor_SetStrategy(DioramaPlaneOverride *p,
                                    DioramaDepthStrategy strategy) {
  if (!p) return;
  /* Preserve the current magnitude across a cycle where the two shapes are the
   * same quantity, so stepping rake:0.15 then cycling to bow gives bow:0.15
   * rather than snapping back to the default. Read BEFORE the clear. A stack
   * and a voxel are also the same quantity (both fill a depth gap with parallel
   * copies) so they carry across too; a thickness is not a tilt, so it does
   * not. */
  const DioramaDepthStrategy was = DioramaLayerEditor_StrategyOfPlane(p);
  float tilt = 0.0f, fill = 0.0f;
  if (was == kDioramaDepth_Rake) tilt = p->rake;
  else if (was == kDioramaDepth_Bow) tilt = p->bow;
  if (was == kDioramaDepth_Stack) fill = p->stack;
  else if (was == kDioramaDepth_Voxel) fill = p->voxel;

  ClearShapeKeys(p);

  switch (strategy) {
    case kDioramaDepth_Flat:
      /* Nothing to author: flat IS the absence of every shape key. */
      break;
    case kDioramaDepth_Rake:
      p->rake = tilt != 0.0f ? tilt : kEditorDefaultTilt;
      p->set_rake = true;
      break;
    case kDioramaDepth_Bow:
      p->bow = tilt != 0.0f ? tilt : kEditorDefaultTilt;
      p->set_bow = true;
      break;
    case kDioramaDepth_Thick:
      p->thickness = kEditorDefaultThickness;
      p->set_thickness = true;
      break;
    case kDioramaDepth_Stack:
      p->stack = fill != 0.0f ? fill : kEditorDefaultStack;
      p->set_stack = true;
      /* An explicit copies count rather than leaning on Resolve's default: the
       * editor must show the number it is actually drawing, and a row reading
       * "--" while four copies render would be a lie. */
      p->stack_copies = kDioramaStackCopiesDefault;
      p->set_stack_copies = true;
      break;
    case kDioramaDepth_Voxel:
      p->voxel = fill != 0.0f ? fill : kEditorDefaultVoxel;
      p->set_voxel = true;
      p->voxel_copies = kDioramaVoxelCopiesDefault;
      p->set_voxel_copies = true;
      break;
    case kDioramaDepth_StrategyCount:
    default:
      break;
  }
}

DioramaDepthStrategy DioramaLayerEditor_CycleStrategy(DioramaPlaneOverride *p,
                                                      int direction) {
  if (!p) return kDioramaDepth_Flat;
  int current = (int)DioramaLayerEditor_StrategyOfPlane(p);
  int step = direction < 0 ? -1 : 1;
  int next = (current + step + kDioramaDepth_StrategyCount) %
             kDioramaDepth_StrategyCount;
  DioramaLayerEditor_SetStrategy(p, (DioramaDepthStrategy)next);
  return (DioramaDepthStrategy)next;
}

/* ── parameter stepping ──────────────────────────────────────────────────
 *
 * Every bound below mirrors the one diorama_layer_order.c's parser enforces for
 * the same key. That duplication is deliberate rather than sloppy: the parser
 * REJECTS an out-of-range value (it must, since it reads a hand-edited file),
 * whereas a UI must CLAMP -- holding Right at the end of a range should stop,
 * not start failing. Sharing one code path would force one of the two
 * behaviours on the other. The test asserts the pairs agree. */
static float ClampFloat(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static int ClampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

/* Step a float key by hundredths. Rounded to the nearest step rather than added
 * blindly so a value loaded from a hand-edited file (rake:0.293) snaps onto the
 * grid on the first press instead of carrying its remainder forever. */
static float StepFloat(float value, int direction, float lo, float hi) {
  const float step = (float)kEditorParamStepPermille / 1000.0f;
  int units = (int)(value / step + (value >= 0.0f ? 0.5f : -0.5f));
  units += direction < 0 ? -1 : 1;
  return ClampFloat((float)units * step, lo, hi);
}

bool DioramaLayerEditor_StepParam(DioramaPlaneOverride *p,
                                  DioramaEditorParam param, int direction) {
  if (!p || direction == 0) return false;
  const DioramaDepthStrategy strategy = DioramaLayerEditor_StrategyOfPlane(p);

  switch (param) {
    case kDioramaEditorParam_Depth: {
      /* "Depth" is whichever key the active shape uses. One row rather than six
       * because the player is adjusting the magnitude of the shape in front of
       * them; which field holds it is an implementation detail of the file.
       *
       * A STEP THAT REACHES EXACTLY ZERO REMOVES THE SHAPE, rather than storing
       * a zero magnitude with the flag still set. ClearParam(Depth) already had
       * this rule, and stating it there but not here was the bug: a zero-valued
       * shape keeps the room "authored" (RoomIsActive tests the FLAG, not the
       * value) so it is written to diorama-layers.ini as `rake:0` -- an entry
       * with no visible effect, no visible row, and enough to break the
       * unedited-room-is-bit-identical guarantee. Holding Left through zero
       * therefore now lands on genuine flat, exactly as cycling to flat does. */
      float next;
      switch (strategy) {
        case kDioramaDepth_Rake:
          next = StepFloat(p->rake, direction, -1.0f, 1.0f);
          if (next == p->rake) return false;
          break;
        case kDioramaDepth_Bow:
          next = StepFloat(p->bow, direction, -1.0f, 1.0f);
          if (next == p->bow) return false;
          break;
        case kDioramaDepth_Thick:
          next = StepFloat(p->thickness, direction, 0.0f, 1.0f);
          if (next == p->thickness) return false;
          break;
        case kDioramaDepth_Stack:
          next = StepFloat(p->stack, direction, 0.0f, 1.0f);
          if (next == p->stack) return false;
          break;
        case kDioramaDepth_Voxel:
          next = StepFloat(p->voxel, direction, 0.0f, 1.0f);
          if (next == p->voxel) return false;
          break;
        case kDioramaDepth_Flat:
        case kDioramaDepth_StrategyCount:
        default:
          return false;   /* flat has no magnitude; the row is not listed */
      }
      if (next == 0.0f) {
        /* Zero is not a shape, so it must not be stored as one. But a rake and a
         * bow are SIGNED -- a negative one tilts the bottom edge away, which is
         * the right shape for a ceiling -- so simply
         * clearing at zero would make the whole negative half of their range
         * unreachable by stepping. Step THROUGH it instead: one more increment in
         * the same direction, so holding Left on a rake goes 0.02, 0.01, -0.01
         * and never rests on a zero-magnitude shape.
         *
         * The unsigned shapes (thick/stack/voxel) are clamped at 0 by StepFloat,
         * so for them there is nothing beyond zero and clearing is correct: they
         * land on genuine flat, exactly as cycling to flat does. */
        const bool signed_shape = (strategy == kDioramaDepth_Rake ||
                                   strategy == kDioramaDepth_Bow);
        if (!signed_shape) {
          /* Same treatment as ClearParam(Depth): drop every shape key, so the
           * plane is indistinguishable from one never authored. */
          ClearShapeKeys(p);
          return true;
        }
        const float step = (float)kEditorParamStepPermille / 1000.0f;
        next = direction < 0 ? -step : step;
      }
      switch (strategy) {
        case kDioramaDepth_Rake:  p->rake = next;      p->set_rake = true; break;
        case kDioramaDepth_Bow:   p->bow = next;       p->set_bow = true; break;
        case kDioramaDepth_Thick: p->thickness = next; p->set_thickness = true; break;
        case kDioramaDepth_Stack: p->stack = next;     p->set_stack = true; break;
        case kDioramaDepth_Voxel: p->voxel = next;     p->set_voxel = true; break;
        default: return false;
      }
      return true;
    }
    case kDioramaEditorParam_Copies: {
      /* A stack and a voxel have SEPARATE caps (8 vs 24) because a voxel's
       * solidity depends on many close slices while a stack's readability
       * depends on few distinct ones. They also live in separate fields, so the
       * row edits whichever the active shape owns. */
      if (strategy == kDioramaDepth_Voxel) {
        int base = p->set_voxel_copies ? p->voxel_copies
                                       : kDioramaVoxelCopiesDefault;
        int next = ClampInt(base + (direction < 0 ? -1 : 1), 2,
                            kDioramaVoxelMax);
        if (p->set_voxel_copies && next == p->voxel_copies) return false;
        p->voxel_copies = next; p->set_voxel_copies = true; return true;
      }
      if (strategy == kDioramaDepth_Stack) {
        int base = p->set_stack_copies ? p->stack_copies
                                       : kDioramaStackCopiesDefault;
        /* Floor of TWO, not the parser's 1. This is the one place the UI bound
         * must be TIGHTER than the manifest's, so the general rule above ("mirror
         * the parser") has an exception here.
         *
         * The renderer gates the stack pass on `copies > 1` (the stack loop
         * in DrawDiorama, diorama.c),
         * because one copy coincides with the plane's own draw and is skipped as
         * redundant -- so `copies:1` is a stack that renders NOTHING while
         * StrategyOfPlane still reports "stack". Stepping down to it made the row
         * say STACK 0.29 while the shape vanished from the screen: exactly the
         * row-disagrees-with-renderer failure this module exists to prevent.
         *
         * The codebase already floors at 2 in the two other places that derive a
         * count -- StackCopiesForDensity (diorama_layer_order.c) and the voxel
         * arm just above -- so this is the outlier being brought into line. 2 is
         * still accepted by the parser, so the manifest agreement holds. */
        int next = ClampInt(base + (direction < 0 ? -1 : 1), 2,
                            kDioramaStackMax);
        if (p->set_stack_copies && next == p->stack_copies) return false;
        p->stack_copies = next; p->set_stack_copies = true;
        /* An explicit count outranks a density in Resolve, so authoring one
         * here must drop the density -- otherwise the density row would keep
         * showing a value that no longer has any effect. */
        p->set_stack_density = false; p->stack_density = 0.0f;
        return true;
      }
      return false;
    }
    case kDioramaEditorParam_Density: {
      if (strategy != kDioramaDepth_Stack) return false;
      float base = p->set_stack_density ? p->stack_density
                                        : kEditorDefaultDensity;
      float next = ClampFloat(base + (direction < 0 ? -1.0f : 1.0f), 1.0f,
                              100.0f);
      if (p->set_stack_density && next == p->stack_density) return false;
      p->stack_density = next; p->set_stack_density = true;
      /* Mirror of the rule above: a density only takes effect when no explicit
       * count is authored, so choosing one drops the count. */
      p->set_stack_copies = false; p->stack_copies = 0;
      return true;
    }
    case kDioramaEditorParam_Direction: {
      if (strategy != kDioramaDepth_Stack && strategy != kDioramaDepth_Voxel)
        return false;
      int base = p->set_stack_direction ? p->stack_direction
                                        : kDioramaStack_Forward;
      int next = (base + (direction < 0 ? kDioramaStack_DirectionCount - 1 : 1))
                 % kDioramaStack_DirectionCount;
      p->stack_direction = next;
      p->set_stack_direction = true;
      return true;
    }
    case kDioramaEditorParam_Z: {
      /* Unbounded in the parser (any float parses), but the projection places
       * the backdrop at 0.00 and the HUD at 0.95, so a z outside 0..1 puts a
       * plane behind the camera or through the HUD. Clamped to the range the
       * scene actually uses. */
      float base = p->set_z ? p->z : 0.0f;
      float next = StepFloat(base, direction, 0.0f, 1.0f);
      if (p->set_z && next == p->z) return false;
      p->z = next; p->set_z = true; return true;
    }
    case kDioramaEditorParam_Alpha: {
      int base = p->set_alpha ? p->alpha : kDioramaLayerAlphaOpaque;
      /* Five per press: 255 steps of one would take the player an unreasonable
       * hold, and alpha is judged by eye rather than by number. */
      int next = ClampInt(base + (direction < 0 ? -5 : 5), 0,
                          kDioramaLayerAlphaOpaque);
      if (p->set_alpha && next == p->alpha) return false;
      p->alpha = (uint8_t)next; p->set_alpha = true; return true;
    }
    case kDioramaEditorParam_Source: {
      int base = p->set_source ? p->source : kDioramaLayerSource_Captured;
      int next = DioramaLayerOrder_NextSource(base, direction);
      p->source = (uint8_t)next;
      p->set_source = true;
      return true;
    }
    case kDioramaEditorParam_Order: {
      /* Same bound as the parser: slots run 0 .. planes*4-1, which leaves room
       * to interleave planes between the built-in slots. */
      int base = p->set_order ? p->order : 0;
      int next = ClampInt(base + (direction < 0 ? -1 : 1), 0,
                          kDioramaPlane_Count * 4 - 1);
      if (p->set_order && next == p->order) return false;
      p->order = next; p->set_order = true; return true;
    }
    case kDioramaEditorParam_None:
    default:
      return false;
  }
}

void DioramaLayerEditor_ClearParam(DioramaPlaneOverride *p,
                                   DioramaEditorParam param) {
  if (!p) return;
  switch (param) {
    case kDioramaEditorParam_Depth:
      /* Clearing the magnitude of a shape removes the shape: a stack of depth
       * zero is not a shape, and leaving the flag set with a zero value would
       * keep the room "authored" while drawing nothing -- so the room would no
       * longer be bit-identical to unauthored despite looking it. */
      ClearShapeKeys(p);
      break;
    case kDioramaEditorParam_Copies:
      p->set_stack_copies = false; p->stack_copies = 0;
      p->set_voxel_copies = false; p->voxel_copies = 0;
      break;
    case kDioramaEditorParam_Density:
      p->set_stack_density = false; p->stack_density = 0.0f;
      break;
    case kDioramaEditorParam_Direction:
      p->set_stack_direction = false;
      p->stack_direction = kDioramaStack_Forward;
      break;
    case kDioramaEditorParam_Z:
      p->set_z = false; p->z = 0.0f;
      break;
    case kDioramaEditorParam_Alpha:
      p->set_alpha = false; p->alpha = 0;
      break;
    case kDioramaEditorParam_Source:
      p->set_source = false;
      p->source = kDioramaLayerSource_Captured;
      break;
    case kDioramaEditorParam_Order:
      p->set_order = false; p->order = 0;
      break;
    case kDioramaEditorParam_None:
    default:
      break;
  }
}

void DioramaLayerEditor_ClearPlane(DioramaPlaneOverride *p) {
  if (!p) return;
  memset(p, 0, sizeof(*p));
}

void DioramaLayerEditor_Upper(char *out, size_t size, const char *text) {
  if (!out || size == 0) return;
  size_t i = 0;
  if (text) {
    for (; text[i] && i + 1 < size; i++)
      out[i] = (text[i] >= 'a' && text[i] <= 'z') ? (char)(text[i] - 'a' + 'A')
                                                  : text[i];
  }
  out[i] = '\0';
}

/* ── help text ───────────────────────────────────────────────────────────
 *
 * Each shape's line names what it COSTS as well as what it does, because the
 * whole reason five shapes exist is that each trades something different away,
 * and a menu that only said what they do would make the choice look arbitrary.
 * The wording tracks diorama-layers.ini deliberately -- same facts, one source.
 */
const char *DioramaLayerEditor_RowHelp(DioramaEditorRowKind kind,
                                       DioramaEditorParam param,
                                       DioramaDepthStrategy strategy) {
  if (kind == kDioramaEditorRow_ResetRoom)
    return "Clear every override in this room or camera-local section, "
           "restoring its inherited built-in look. The scope stops being "
           "written to diorama-layers.ini entirely.";
  if (kind == kDioramaEditorRow_Header)
    return "The editor authors the room you are standing in. Walk into a stage "
           "on this level to edit it.";

  if (kind == kDioramaEditorRow_Plane) {
    switch (strategy) {
      case kDioramaDepth_Flat:
        return "Flat: a sheet parallel to the screen, as every room shipped. "
               "Left/Right picks a depth shape; B expands its settings.";
      case kDioramaDepth_Rake:
        return "Rake: tilts the plane, top edge keeps its depth and the bottom "
               "comes forward. Right for a surface that recedes -- water, a "
               "floor. COST: the plane's own rows end up at different depths, "
               "so it picks up two parallax rates and shears as the camera "
               "moves. Try bow if that shows.";
      case kDioramaDepth_Bow:
        return "Bow: the same tilt eased on a curve. The top keeps its original "
               "depth and only the bottom bends forward, so the distortion sits "
               "where the geometry needs it. Still a tilt, so still more than "
               "one parallax rate.";
      case kDioramaDepth_Thick:
        return "Thick: extrudes the bottom edge forward into a near face, "
               "leaving the plane's own art square to the camera. COST: the "
               "skirt hangs from the QUAD's edge, so it bands across "
               "transparent art. Use voxel for an island.";
      case kDioramaDepth_Stack:
        return "Stack: parallel repeats filling the gap, fading with distance. "
               "Nothing tilts, so every copy has ONE parallax rate and the "
               "layer keeps its flat motion. COST: copies are discrete, so gaps "
               "remain between slices.";
      case kDioramaDepth_Voxel:
        return "Voxel: dense repeats with no fade, so the layer reads as one "
               "solid object -- and because copies carry the layer's own alpha, "
               "each art island extrudes itself and the silhouette survives. "
               "COST: the most expensive shape here.";
      default:
        break;
    }
  }

  switch (param) {
    case kDioramaEditorParam_Depth:
      return "How far the shape reaches, in the scene's own depth units. The "
             "backdrop sits at 0.00 and the HUD at 0.95, so 0.29 is roughly the "
             "gap between two neighbouring layers.";
    case kDioramaEditorParam_Copies:
      return "How many slices fill the depth. More closes the gaps between them "
             "at one extra draw of the whole layer each -- which is why a stack "
             "and a voxel have different caps.";
    case kDioramaEditorParam_Density:
      return "Slices per unit of depth, so the spacing you SEE stays the same "
             "when the depth changes or the value is reused in another room. An "
             "explicit count overrides this.";
    case kDioramaEditorParam_Direction:
      return "Which side of the plane to fill. Higher depth is nearer the "
             "camera, so forward fills toward you -- what a layer sitting "
             "behind what it should meet needs.";
    case kDioramaEditorParam_Z:
      return "This plane's depth in the projection. Also sets how blurred it is: "
             "depth of field focuses on 0.50, so moving a plane changes its "
             "focus as well as its position.";
    case kDioramaEditorParam_Alpha:
      return "This plane's opacity. Lets a room compensate for a translucency "
             "the capture did not reproduce.";
    case kDioramaEditorParam_Source:
      return "Skybox image source. Captured uses the current room's BG2; ROM "
             "GG/MM BG1 or BG2 reconstructs that action room directly from "
             "the cartridge. It is visible when Diorama skybox is enabled.";
    case kDioramaEditorParam_Order:
      return "Where the plane sits in the paint sequence, back to front. "
             "Separate from depth on purpose, so reordering a plane does not "
             "also refocus it.";
    case kDioramaEditorParam_None:
    default:
      return "";
  }
}

/* ── row building ────────────────────────────────────────────────────────  */

static DioramaEditorRow *PushRow(DioramaEditorRow *out, int capacity,
                                 int *count) {
  if (*count >= capacity) return NULL;
  DioramaEditorRow *row = &out[*count];
  memset(row, 0, sizeof(*row));
  row->plane = -1;
  row->param = kDioramaEditorParam_None;
  (*count)++;
  return row;
}

static void SetText(char *dst, size_t size, const char *text) {
  snprintf(dst, size, "%s", text ? text : "");
}

/* Value column for a plane row: the shape it is using and its magnitude, which
 * together are what the player is comparing between presses. */
static void FormatPlaneValue(char *dst, size_t size,
                             const DioramaPlaneOverride *p) {
  const DioramaDepthStrategy strategy = DioramaLayerEditor_StrategyOfPlane(p);
  char upper[16];
  DioramaLayerEditor_Upper(upper, sizeof(upper),
                           DioramaLayerOrder_StrategyName(strategy));

  switch (strategy) {
    case kDioramaDepth_Rake:  snprintf(dst, size, "%s %.2f", upper, (double)p->rake); break;
    case kDioramaDepth_Bow:   snprintf(dst, size, "%s %.2f", upper, (double)p->bow); break;
    case kDioramaDepth_Thick: snprintf(dst, size, "%s %.2f", upper, (double)p->thickness); break;
    case kDioramaDepth_Stack: snprintf(dst, size, "%s %.2f", upper, (double)p->stack); break;
    case kDioramaDepth_Voxel: snprintf(dst, size, "%s %.2f", upper, (double)p->voxel); break;
    case kDioramaDepth_Flat:
    case kDioramaDepth_StrategyCount:
    default:                  SetText(dst, size, upper); break;
  }
}

/* Append the parameter rows for the selected plane. Only the ones its ACTIVE
 * shape actually uses: a row the renderer would ignore is worse than a missing
 * one, because the player will step it and conclude the editor is broken when
 * nothing changes. */
static void PushParamRows(DioramaEditorRow *out, int capacity, int *count,
                          int plane, const DioramaPlaneOverride *p,
                          uint8_t effective_source) {
  const DioramaDepthStrategy strategy = DioramaLayerEditor_StrategyOfPlane(p);

  struct { DioramaEditorParam param; const char *label; } rows[9];
  int n = 0;
  if (strategy != kDioramaDepth_Flat)
    rows[n].param = kDioramaEditorParam_Depth, rows[n++].label = "depth";
  if (strategy == kDioramaDepth_Stack || strategy == kDioramaDepth_Voxel) {
    rows[n].param = kDioramaEditorParam_Copies;
    rows[n++].label = strategy == kDioramaDepth_Voxel ? "slices" : "copies";
    rows[n].param = kDioramaEditorParam_Direction, rows[n++].label = "direction";
  }
  /* Density is a stack-only alternative to a count, and is listed only once the
   * player has authored one -- offering both at all times invites setting a
   * density that the count silently outranks. */
  if (strategy == kDioramaDepth_Stack && p->set_stack_density)
    rows[n].param = kDioramaEditorParam_Density, rows[n++].label = "density";
  rows[n].param = kDioramaEditorParam_Z, rows[n++].label = "z depth";
  rows[n].param = kDioramaEditorParam_Alpha, rows[n++].label = "alpha";
  if (plane == kDioramaPlane_Backdrop) {
    rows[n].param = kDioramaEditorParam_Source;
    rows[n++].label = "skybox source";
  }
  rows[n].param = kDioramaEditorParam_Order, rows[n++].label = "paint order";

  for (int i = 0; i < n; i++) {
    DioramaEditorRow *row = PushRow(out, capacity, count);
    if (!row) return;
    row->kind = (rows[i].param == kDioramaEditorParam_Direction ||
                 rows[i].param == kDioramaEditorParam_Source)
        ? kDioramaEditorRow_ParamEnum : kDioramaEditorRow_Param;
    row->plane = plane;
    row->param = rows[i].param;
    row->nested = true;
    row->selectable = true;
    SetText(row->label, sizeof(row->label), rows[i].label);

    switch (rows[i].param) {
      case kDioramaEditorParam_Depth: {
        float v = 0.0f;
        switch (strategy) {
          case kDioramaDepth_Rake:  v = p->rake; break;
          case kDioramaDepth_Bow:   v = p->bow; break;
          case kDioramaDepth_Thick: v = p->thickness; break;
          case kDioramaDepth_Stack: v = p->stack; break;
          case kDioramaDepth_Voxel: v = p->voxel; break;
          default: break;
        }
        snprintf(row->value, sizeof(row->value), "%.2f", (double)v);
        break;
      }
      case kDioramaEditorParam_Copies: {
        int v = strategy == kDioramaDepth_Voxel
            ? (p->set_voxel_copies ? p->voxel_copies : kDioramaVoxelCopiesDefault)
            : (p->set_stack_copies ? p->stack_copies : kDioramaStackCopiesDefault);
        snprintf(row->value, sizeof(row->value), "%d", v);
        break;
      }
      case kDioramaEditorParam_Density:
        snprintf(row->value, sizeof(row->value), "%.0f",
                 (double)p->stack_density);
        break;
      case kDioramaEditorParam_Direction: {
        DioramaLayerEditor_Upper(
            row->value, sizeof(row->value),
            DioramaLayerOrder_StackDirectionToken(
                p->set_stack_direction ? p->stack_direction
                                       : kDioramaStack_Forward));
        break;
      }
      case kDioramaEditorParam_Z:
        /* An unauthored knob shows a dash rather than a number: printing the
         * built-in value would make every plane look authored, and the player
         * could not tell what this room actually contributes. */
        if (p->set_z) snprintf(row->value, sizeof(row->value), "%.2f",
                               (double)p->z);
        else SetText(row->value, sizeof(row->value), "--");
        break;
      case kDioramaEditorParam_Alpha:
        if (p->set_alpha) snprintf(row->value, sizeof(row->value), "%u",
                                   (unsigned)p->alpha);
        else SetText(row->value, sizeof(row->value), "--");
        break;
      case kDioramaEditorParam_Source:
        row->effective_source = effective_source;
        DioramaLayerEditor_Upper(
            row->value, sizeof(row->value),
            DioramaLayerOrder_SourceToken(effective_source));
        break;
      case kDioramaEditorParam_Order:
        if (p->set_order) snprintf(row->value, sizeof(row->value), "%d",
                                   p->order);
        else SetText(row->value, sizeof(row->value), "--");
        break;
      default:
        break;
    }
  }
}

int DioramaLayerEditor_BuildRows(const DioramaLayerOrderTable *table,
                                 const DioramaEditorContext *context,
                                 int level_index,
                                 DioramaEditorRow *out, int capacity) {
  if (!out || capacity <= 0 || !context) return 0;
  if (level_index < 0 || level_index >= kDioramaEditorLevelCount) return 0;

  const uint8_t group = DioramaLayerEditor_LevelGroup(level_index);
  const bool live_here = context->room_live && context->map_group == group;
  int count = 0;

  if (!live_here) {
    /* One explanatory row rather than an empty panel. The reason is specific --
     * the editor edits the live room, so the player has to be in it -- and
     * saying so is what stops the tab reading as broken. */
    DioramaEditorRow *row = PushRow(out, capacity, &count);
    if (!row) return count;
    row->kind = kDioramaEditorRow_Header;
    SetText(row->label, sizeof(row->label), "Enter a stage here to edit");
    SetText(row->value, sizeof(row->value), "");
    return count;
  }

  const DioramaRoomOverride *room = DioramaLayerOrder_FindSection(
      table, group, context->map_number, context->section);
  uint8_t effective_source = kDioramaLayerSource_Captured;
  const DioramaRoomOverride *base = DioramaLayerOrder_Find(
      table, group, context->map_number);
  if (base && base->planes[kDioramaPlane_Backdrop].set_source)
    effective_source = base->planes[kDioramaPlane_Backdrop].source;
  if (room && room->planes[kDioramaPlane_Backdrop].set_source)
    effective_source = room->planes[kDioramaPlane_Backdrop].source;

  {
    DioramaEditorRow *row = PushRow(out, capacity, &count);
    if (!row) return count;
    row->kind = kDioramaEditorRow_Header;
    const char *section = DioramaLayerOrder_SectionToken(context->section);
    if (section)
      snprintf(row->label, sizeof(row->label), "Room %02X %s",
               context->map_number, section);
    else
      snprintf(row->label, sizeof(row->label), "Room %02X",
               context->map_number);
    SetText(row->value, sizeof(row->value), "HERE");
  }

  /* Planes in MANIFEST-TOKEN order (bg1, bg1hi, bg2, bg2hi, bg3, obj0..obj3),
   * which is the order an export writes, so the list on screen matches the file
   * the player will open. Iterating plane INDICES instead would interleave the
   * priority bands (backdrop lands mid-list, since its enum value follows the
   * engine sources), which is why this walks the token table.
   *
   * Deliberately NOT paint order either: that changes as the player authors
   * `order`, and a list that reshuffles under the cursor mid-edit is the failure
   * mode to avoid. */
  static const DioramaPlaneOverride kUnauthored;
  const int plane_count = DioramaLayerOrder_PlaneCount();
  for (int i = 0; i < plane_count; i++) {
    const int plane = DioramaLayerOrder_PlaneAt(i);
    const char *token = DioramaLayerOrder_PlaneToken(plane);
    if (!token) continue;   /* not a plane the diorama draws */
    const DioramaPlaneOverride *p = room ? &room->planes[plane] : &kUnauthored;

    DioramaEditorRow *row = PushRow(out, capacity, &count);
    if (!row) return count;
    row->kind = kDioramaEditorRow_Plane;
    row->plane = plane;
    row->selectable = true;
    SetText(row->label, sizeof(row->label), token);
    FormatPlaneValue(row->value, sizeof(row->value), p);

    if (plane == context->selected_plane)
      PushParamRows(out, capacity, &count, plane, p, effective_source);
  }

  DioramaEditorRow *reset = PushRow(out, capacity, &count);
  if (reset) {
    reset->kind = kDioramaEditorRow_ResetRoom;
    reset->selectable = true;
    const char *section = DioramaLayerOrder_SectionToken(context->section);
    if (section)
      snprintf(reset->label, sizeof(reset->label), "Reset %s", section);
    else
      snprintf(reset->label, sizeof(reset->label), "Reset room %02X",
               context->map_number);
    SetText(reset->value, sizeof(reset->value), "RESET");
  }
  return count;
}
