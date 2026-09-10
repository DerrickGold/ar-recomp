#include "bg3_composite_policy.h"

int ArBg3Composite_CaptureHeight(
    const ArBg3CompositeCaptureInputs *in) {
  if (!in || in->authentic_height <= 0 || in->authentic_height > 255)
    return 0;
  if (in->scoped_text_scene || in->flat_diorama)
    return in->authentic_height;
  if (in->hud_split_height <= 0)
    return 0;
  return in->hud_split_height < in->authentic_height
      ? in->hud_split_height : in->authentic_height;
}
