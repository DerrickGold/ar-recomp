#include "sim/menu/sim_menu_capture.h"

#include "actraiser/actraiser_sim_menu.h"
#include "actraiser/actraiser_localization_compose_regions.h"
#include "actraiser_game.h"
#include "app/input_map.h"
#include "app/session_fatal.h"
#include "app/settings.h"

void SimMenu_CaptureFrame(SimMenuFrame *frame, const SnesRunnerApi *api,
                           SrRunnerHandle *runner) {
  ActRaiserSimMenu_CopyModel(&frame->model);
  ActRaiserSimMenu_CopyHelp(&frame->help);
  frame->preserved_bg3_region =
      (ArTextCellRegion)ACTRAISER_COMPOSE_REGION_TOWN_STATUS;
  frame->scale_percent = (uint8_t)g_settings.sim_menu_scale_percent;
  if (!api || !runner || frame->model.phase == kSimMenu_Closed ||
      frame->model.phase == kSimMenu_Native)
    return;

  InputMap_GameActionHint(frame->describe_binding,
      sizeof(frame->describe_binding), kInputAction_SimDescribe);
  if (frame->model.phase == kSimMenu_Confirm)
    frame->model.yes = g_ram[kActRaiserWram_MenuChoiceScratch] == 0;
  if (!SimMenuArt_Capture(frame, api, runner))
    SessionFatal_Request("SIM menu artwork capture failed after its native preflight.");
}
