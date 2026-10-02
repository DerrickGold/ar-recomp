#ifndef AR_ACTION_EFFECT_MEMBERS_H
#define AR_ACTION_EFFECT_MEMBERS_H
/* Stable native catalogue identities and sparse presentation-only transforms. */
#include "action_effects.h"
#include "render/render_types.h"
typedef struct ActionNativeMemberSource {
  float x, y, width, height;
} ActionNativeMemberSource;
/* ID is catalogue ordinal + 1; the catalogue is append-only. Witness masks
 * are checked independently: moving light never moves or invents source art. */
unsigned ActionEffectMembers_Count(unsigned kind, unsigned group, unsigned room);
bool ActionEffectMembers_Source(unsigned kind, unsigned group, unsigned room, unsigned id,
                                ActionNativeMemberSource *out);
bool ActionEffectMembers_MovableSource(unsigned kind);
bool ActionEffectMembers_Angled(unsigned kind, unsigned group, unsigned room, unsigned id);
const ActionNativeMember *ActionEffectMembers_Find(const ActionNativeMembers *members,
                                                   unsigned kind, unsigned index);
void ActionEffectMembers_Tint(const ActionNativeMember *member, ArRenderVertex2D *vertices,
                              int begin, int end, bool multiply);
#endif
