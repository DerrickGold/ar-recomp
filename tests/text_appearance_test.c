#include "localization/text_template.h"
#include <stdio.h>
#include <string.h>

static ArTextTreatment treatment;
static bool available = true;

static const ArTextTreatment *Find(void *context, const char *name) {
  (void)context;
  return !strcmp(name, "hud") ? &treatment : NULL;
}

static bool Ink(void *context, const char *binding, uint32_t *rgb) {
  (void)context;
  if (!available || strcmp(binding, "hud.band"))
    return false;
  *rgb = 0xFFD36A;
  return true;
}

#define REQUIRE(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%d: %s: %s\n", __LINE__, #condition, error.message);    \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  ArTextTemplateError error = {0};
  REQUIRE(ArTextTreatment_SetProperty(&treatment, "band", "native:hud.band",
                                      &error));
  REQUIRE(ArTextTreatment_SetProperty(&treatment, "body", "#FFFFFF", &error));
  REQUIRE(ArTextTreatment_SetProperty(&treatment, "shadow", "#000000", &error));
  REQUIRE(ArTextTreatment_SetProperty(&treatment, "shape", "keyline", &error));
  const ArTextAppearanceBindings bindings = {.find_treatment = Find,
                                             .resolve_ink = Ink};
  const ArTextTemplateStyle defaults = {
      .font = "body", .treatment = "hud", .scale_percent = 110, .italic = 2};
  const ArTextTemplateStyle small = {
      .font = "title", .scale_percent = 80, .italic = 1};
  ArTextRunAppearance result;
  REQUIRE(ArTextTemplate_ResolveAppearance(&defaults, &small, true, &bindings,
                                           &result, &error));
  REQUIRE(result.scale_basis == 8800 && !strcmp(result.font_role, "title"));
  REQUIRE(!result.italic && result.slant_ascii_numerals &&
          result.shadow_enabled && result.keyline_shadow);
  REQUIRE(result.band_rgb == 0xFFD36A && result.body_rgb == 0xFFFFFF &&
          !result.shadow_rgb);
  const ArTextRunAppearance prior = result;
  available = false;
  REQUIRE(!ArTextTemplate_ResolveAppearance(&defaults, &small, true, &bindings,
                                            &result, &error));
  REQUIRE(!memcmp(&prior, &result, sizeof(result)));
  const ArTextTemplateStyle solid = {.has_color = true, .color_rgb = 0x123456};
  REQUIRE(ArTextTemplate_ResolveAppearance(&defaults, &solid, true, &bindings,
                                           &result, &error));
  REQUIRE(result.band_rgb == 0x123456 && result.body_rgb == 0x123456 &&
          result.shadow_enabled);
  REQUIRE(result.scale_basis == 11000 && result.italic &&
          !result.slant_ascii_numerals);
  REQUIRE(ArTextTemplate_ResolveAppearance(NULL, NULL, false, NULL, &result,
                                           &error));
  REQUIRE(!strcmp(result.font_role, "body") && result.scale_basis == 10000 &&
          !result.shadow_enabled);
  REQUIRE(result.band_rgb == 0xFFFFFF && result.body_rgb == 0xFFFFFF);
  return 0;
}
