#include "presentation_view.h"
#include "actraiser_game.h"

PresentationViewDecision PresentationView_Resolve(
    const FrameSlot *slot, RenderComparisonView comparison) {
  PresentationViewDecision result = {.scene = kPerformanceScene_Native};
  if (!slot) return result;
  result.visible = !(slot->inidisp & 0x80) && (slot->inidisp & 15) != 0;
  if (comparison == kRenderComparison_Authentic) return result;
  if (slot->diorama_active) {
    result.scene = kPerformanceScene_Action;
    return result;
  }
  if (ActRaiser_IsActionMap(slot->diorama_map_group, slot->diorama_map_number))
    result.scene = kPerformanceScene_ActionFlat;
  if (!slot->sim.master_enabled) return result;
  switch (slot->sim.view) {
    case kSimView_Enhanced:
      if (slot->sim.separated_valid &&
          (slot->sim.effective_features & (kSimFeature_GroundProjection | kSimFeature_SeparatedComposite)) ==
              (kSimFeature_GroundProjection | kSimFeature_SeparatedComposite))
        result.scene = kPerformanceScene_Town;
      /* Disabling the separated/ground stage is an explicit flat profile. */
      else if (!slot->sim.separated_valid &&
          (slot->sim.requested_features & kSimFeature_SeparatedComposite))
        result.unexpected_native = result.visible;
      break;
    case kSimView_WorldNavigation: result.scene = kPerformanceScene_World; break;
    case kSimView_SkyPalace: result.scene = kPerformanceScene_Palace; break;
    case kSimView_AuthenticFallback: result.unexpected_native = result.visible; break;
    default: break;
  }
  return result;
}
