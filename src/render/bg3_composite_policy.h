#ifndef AR_BG3_COMPOSITE_POLICY_H
#define AR_BG3_COMPOSITE_POLICY_H

#include <stdbool.h>

typedef struct ArBg3CompositeCaptureInputs {
  /* True only for the currently supported Sky Palace / simulation text
   * scope. Those scenes need the complete BG3 surface even when the status
   * HUD itself is not split (for example Wide Raw). */
  bool scoped_text_scene;
  /* Flat Diorama also promotes all BG3 rows into screen space. Tilted
   * Diorama deliberately does not set this and keeps native plane ownership. */
  bool flat_diorama;
  int hud_split_height;
  int authentic_height;
} ArBg3CompositeCaptureInputs;

/* Returns the exclusive source row for the one host-owned BG3 capture, or
 * zero when BG3 remains entirely in native scanout. Invalid dimensions fail
 * closed. This policy is renderer-neutral and shared by ordinary, Wide Raw,
 * and flat-Diorama setup. */
int ArBg3Composite_CaptureHeight(
    const ArBg3CompositeCaptureInputs *inputs);

#endif /* AR_BG3_COMPOSITE_POLICY_H */
