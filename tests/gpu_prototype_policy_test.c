#include "platform/sdl/gpu_prototype_policy.h"

#include <stdio.h>
#include <stdlib.h>

static void Expect(const char *driver, const char *value, bool expected) {
  if (ArGpuPrototype_Resolve(driver, value) != expected) {
    fprintf(stderr, "gpu_prototype_policy_test: driver=%s value=%s expected %d\n",
            driver ? driver : "(null)", value ? value : "(unset)", expected);
    exit(1);
  }
}

int main(void) {
  /* Validated backends default on; everything else keeps the reference path. */
  Expect("metal", NULL, true);
  Expect("vulkan", NULL, true);
  Expect("metal", "", true);
  Expect("vulkan", "auto", true);
  Expect("direct3d12", NULL, false);
  Expect("direct3d12", "auto", false);
  Expect(NULL, NULL, false);
  /* Explicit requests override the backend default in both directions. */
  Expect("direct3d12", "1", true);
  Expect("metal", "0", false);
  Expect("vulkan", "0", false);
  Expect("metal", "yes", false);
  Expect("vulkan", "2", false);
  puts("gpu_prototype_policy_test: PASS");
  return 0;
}
