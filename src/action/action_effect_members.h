#ifndef AR_ACTION_EFFECT_MEMBERS_H
#define AR_ACTION_EFFECT_MEMBERS_H
/* Stable native catalogue identities and sparse presentation-only transforms. */
#include "action_effects.h"
#include "action_light_kinds.h"
#include "render/render_types.h"
#include <stddef.h>
typedef struct ActionNativeMemberSource {
  float x, y, width, height;
} ActionNativeMemberSource;
/* Complete water definitions may be authored in any room. */
struct ActionWaterField;
unsigned ActionEffectMembers_WaterLimit(unsigned kind);
unsigned ActionEffectMembers_WaterCount(unsigned kind, const struct ActionWaterField *field);
bool ActionEffectMembers_WaterSource(unsigned kind, unsigned id,
                                    const struct ActionWaterField *field, ActionNativeMemberSource *out);
/* ID is catalogue ordinal + 1; the catalogue is append-only. Witness masks
 * are checked independently: moving light never moves or invents source art. */
unsigned ActionEffectMembers_Count(unsigned kind, unsigned group, unsigned room);
bool ActionEffectMembers_Source(unsigned kind, unsigned group, unsigned room, unsigned id,
                                ActionNativeMemberSource *out);
bool ActionEffectMembers_MovableSource(unsigned kind);
bool ActionEffectMembers_Angled(unsigned kind, unsigned group, unsigned room, unsigned id);
/* Compositor-only callers use the sparse records already in the frame. The
 * catalogue reader is only needed when compiling recipes. */
static inline const ActionNativeMember *ActionEffectMembers_Find(const ActionNativeMembers *members,
    unsigned kind, unsigned index) {
  if (!members || members->count > kActionNativeMemberMax) return NULL;
  for (unsigned i = 0; i < members->count; ++i)
    if (members->records[i].kind == kind && members->records[i].index == index)
      return &members->records[i];
  return NULL;
}

static inline void ActionEffectMembers_Tint(const ActionNativeMember *member, ArRenderVertex2D *vertices,
    int begin, int end, bool multiply) {
  if (!member) return;
  const float gain = multiply ? member->intensity : ActionLightIntensity_Gain(member->intensity);
  const float opacity = ActionLightIntensity_Opacity(member->intensity);
  const float red = ((member->color >> 16) & 255) / 255.f;
  const float green = ((member->color >> 8) & 255) / 255.f;
  const float blue = (member->color & 255) / 255.f;
  for (int i = begin; i < end; ++i) {
    ArRenderColorF *color = &vertices[i].color;
    color->r *= red;
    color->g *= green;
    color->b *= blue;
    if (gain != 1) { color->r *= gain; color->g *= gain; color->b *= gain; }
    if (!multiply) color->a = fminf(1, color->a * opacity);
  }
}
#endif
