#include "sim/menu/sim_menu_capture.h"
#include "actraiser/actraiser_sim_menu.h"
#include "actraiser_game.h"
#include "app/input_map.h"
#include "app/session_fatal.h"
#include "app/settings.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

Settings g_settings;
uint8 g_ram[kActRaiserWramSize];
static SimMenuModel model;
static SimMenuHelpPage help;
static int model_copies, help_copies, hint_copies, art_captures, failures;
static bool art_available = true;
static const SnesRunnerApi api = {.struct_size = sizeof(SnesRunnerApi)};
static SrRunnerHandle *const runner = (SrRunnerHandle *)(uintptr_t)1;

void ActRaiserSimMenu_CopyModel(SimMenuModel *out) {
  *out = model;
  model_copies++;
}
void ActRaiserSimMenu_CopyHelp(SimMenuHelpPage *out) {
  *out = help;
  help_copies++;
}
int InputMap_GameActionHint(char *buffer, int size, InputAction action) {
  assert(action == kInputAction_SimDescribe && size >= 8);
  hint_copies++;
  return snprintf(buffer, (size_t)size, "button");
}
bool SimMenuArt_Capture(SimMenuFrame *out, const SnesRunnerApi *actual_api,
                        SrRunnerHandle *actual_runner) {
  assert(actual_api == &api && actual_runner == runner);
  assert(!strcmp(out->describe_binding, "button"));
  assert(out->model.phase == model.phase && out->help.authored_page == 7);
  if (model.phase == kSimMenu_Confirm)
    assert(out->model.yes == (g_ram[kActRaiserWram_MenuChoiceScratch] == 0));
  art_captures++;
  out->valid = art_available;
  return art_available;
}
void SessionFatal_Request(const char *format, ...) {
  assert(strstr(format, "SIM menu artwork capture failed"));
  failures++;
}

static SimMenuFrame frame;
static void Capture(const SnesRunnerApi *view) {
  memset(&frame, 0, sizeof(frame));
  SimMenu_CaptureFrame(&frame, view, runner);
  assert(!memcmp(&frame.help, &help, sizeof(help)));
  assert(frame.scale_percent == 125);
}

int main(void) {
  g_settings.sim_menu_scale_percent = 125;
  help.active = true;
  help.authored_page = 7;
  for (int phase = kSimMenu_Closed; phase <= kSimMenu_Opening; phase++) {
    model.phase = phase;
    Capture(NULL);
    assert(!frame.valid && !frame.describe_binding[0]);
  }
  assert(!hint_copies && !art_captures && !failures);
  model.phase = kSimMenu_Closed;
  Capture(&api);
  model.phase = kSimMenu_Native;
  Capture(&api);
  assert(!hint_copies && !art_captures && !failures);

  model.phase = kSimMenu_Confirm;
  model.yes = false;
  g_ram[kActRaiserWram_MenuChoiceScratch] = 0;
  Capture(&api);
  assert(frame.model.yes && frame.valid);
  g_ram[kActRaiserWram_MenuChoiceScratch] = 1;
  Capture(&api);
  assert(!frame.model.yes);
  model.phase = kSimMenu_Browse;
  model.yes = true;
  Capture(&api);
  assert(frame.model.yes && frame.valid); /* Scratch byte applies only to confirmation. */
  assert(hint_copies == 3 && art_captures == 3 && !failures);

  art_available = false;
  Capture(&api);
  assert(!frame.valid && failures == 1);
  assert(model_copies == help_copies);
  puts("SIM menu capture: gating, native confirmation, snapshot and failure reporting passed");
  return 0;
}
