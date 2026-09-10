#include "render/bg3_composite_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
  ArBg3CompositeCaptureInputs in = {
    .hud_split_height = 32,
    .authentic_height = 224,
  };
  assert(ArBg3Composite_CaptureHeight(&in) == 32);

  in.scoped_text_scene = true;
  assert(ArBg3Composite_CaptureHeight(&in) == 224);

  /* Scoped Wide Raw has no split, but its dialogue rows still need the full
   * native-layout capture. */
  in.hud_split_height = 0;
  assert(ArBg3Composite_CaptureHeight(&in) == 224);

  in.scoped_text_scene = false;
  in.flat_diorama = true;
  assert(ArBg3Composite_CaptureHeight(&in) == 224);

  in.flat_diorama = false;
  assert(ArBg3Composite_CaptureHeight(&in) == 0);
  in.authentic_height = 0;
  assert(ArBg3Composite_CaptureHeight(&in) == 0);
  assert(ArBg3Composite_CaptureHeight(NULL) == 0);

  puts("bg3_composite_policy_test: PASS");
  return 0;
}
