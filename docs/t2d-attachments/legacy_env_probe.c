/* Throwaway probe (T2d guard rail): dumps Settings_UsesLegacyEnvironmentSyntax
 * for every descriptor so the value can be compared byte-for-byte before and
 * after the exclusion-list -> per-row-flag refactor. NOT part of the build;
 * compiled standalone against settings.c. Deleted once T2d is verified. */
#include <stdio.h>
#include "settings.h"

/* Exposed by a temporary non-static shim added to settings.c for this probe. */
bool Settings_UsesLegacyEnvironmentSyntax_Probe(const SettingDesc *desc);

/* Link stubs: the classifier does pure pointer comparisons, so none of these
 * are actually called — they only satisfy the standalone link. */
bool AtomicReplaceFile(const char *a, const char *b) { (void)a; (void)b; return true; }
void Diorama_OnModeChanged(void) {}
int g_gpu_shaders_active, g_ws_active, g_ws_display_extra, g_ws_extra;
const char *InputMap_DescribeRow(void) { return ""; }
int InputMap_FormatBindingField(void) { return 0; }
int InputMap_GamepadCount(void) { return 0; }
const char *InputMap_GamepadName(void) { return ""; }
int InputMap_ParseBindingField(void) { return 0; }
bool Present_EffectRendererSupported(void) { return false; }
bool Present_SimRimMaskSupported(void) { return false; }
void Randomizer_Apply(void) {}
bool Randomizer_IsAvailable(void) { return false; }

int main(void) {
  for (int i = 0; i < g_setting_desc_count; i++) {
    printf("%d\t%s\t%d\n", i, g_setting_descs[i].key,
           Settings_UsesLegacyEnvironmentSyntax_Probe(&g_setting_descs[i]) ? 1 : 0);
  }
  return 0;
}
