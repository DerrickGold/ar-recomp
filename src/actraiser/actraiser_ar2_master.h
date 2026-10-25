#ifndef AR_ACTRAISER_AR2_MASTER_H
#define AR_ACTRAISER_AR2_MASTER_H
/* ActRaiser 2 master: plays the action stages as ActRaiser 2's master.
 *
 * The movement and attack state machine is a C transcription of the master's
 * own ActRaiser 2 script ($82:8076-$88FF), driven by AR2's animation timing and
 * motion programs read from the player's AR2 dump (actraiser_ar2_master_assets).
 * Everything else stays native ActRaiser: the player slot, terrain collision
 * ($8F14/$8FE7), hurt/knockback/death, invulnerability, timer expiry, magic,
 * platforms, camera and score. Swings feed AR1's own enemy-hit resolver, so
 * damage is AR1's player attack (+2A: 1, or 2 with the sword power-up).
 * The shield stops the enemy shots listed in kShieldShots.
 * Phase: game (native CPU hooks) and presentation (sprite parts, OBJ upload).
 * Tests: tests/actraiser_ar2_master_test.c, tests/actraiser_ar2_master_runtime_test.c */

#include <stdbool.h>
#include <stdint.h>

#include "actraiser/actraiser_ar2_master_assets.h"
#include "snesrecomp/game/cpu.h"

/* Session startup: read a Workshop-extracted character pack. Missing or
 * invalid assets leave the original Master active. */
void ActRaiserAr2Master_Load(const char *asset_path);
void ActRaiserAr2Master_Shutdown(void);
bool ActRaiserAr2Master_Available(void);
const Ar2MasterAssets *ActRaiserAr2Master_Assets(void);

/* hle_func_if on every AR1 ground/air control handler the dispatcher can hold
 * in the player's +12 ($9832..$9BEE). */
bool ActRaiser_Ar2MasterControlEntry(CpuState *cpu);
RecompReturn ActRaiser_Ar2MasterControl(CpuState *cpu);
/* hle_func_if $8A3C: AR1's attacker-versus-victim pass, wrapped so the
 * player's union box covers the active AR2 attack box. */
bool ActRaiser_Ar2MasterHitPassEntry(CpuState *cpu);
RecompReturn ActRaiser_Ar2MasterHitPass(CpuState *cpu);
/* hle_func_if $8B67: per-part attack test when the attacker is the player. */
bool ActRaiser_Ar2MasterHitTestEntry(CpuState *cpu);
RecompReturn ActRaiser_Ar2MasterHitTest(CpuState *cpu);
/* hle_func_if $8970: the native player contact pass, preceded by the shield
 * stopping known enemy shots. AR_AR2_SHIELD_TRACE=1 logs every object the pass
 * can touch (also with the ActRaiser master, for surveying shot families). */
bool ActRaiser_Ar2MasterContactEntry(CpuState *cpu);
RecompReturn ActRaiser_Ar2MasterContact(CpuState *cpu);

/* Game update, before the action-sprite scan: commit the character selection
 * and prepare one pose for every draw pass and the following NMI upload.
 * Advance native-owned animations even when the player is culled. */
void ActRaiserAr2Master_UpdatePresentation(const CpuState *cpu);

/* Sprite builder: read the prepared parts without advancing animation or
 * changing controller state. These replace this action object's
 * native composition, if it is the player and the AR2 master owns it. Offsets
 * are from the native draw origin (object position minus its live +0A/+0C
 * extents); attributes are taken before the object's own +28 XOR. */
enum { kActRaiserAr2MasterMaxDrawParts = 40 };
typedef struct ActRaiserAr2MasterPart {
  int x[2], y[2]; /* [1] when flipped on that axis */
  uint16_t attributes;
  bool large;
} ActRaiserAr2MasterPart;
typedef struct ActRaiserAr2MasterDraw {
  unsigned count;
  ActRaiserAr2MasterPart parts[kActRaiserAr2MasterMaxDrawParts];
} ActRaiserAr2MasterDraw;
bool ActRaiserAr2Master_DrawObject(const CpuState *cpu, uint16_t object,
                                   ActRaiserAr2MasterDraw *out);

/* NMI, immediately after the native OAM upload: copy the drawn frame's tiles
 * onto the OBJ tiles only ActRaiser's own master uses, and keep palette 4 in
 * AR2 colours (following the native statue fades). */
void ActRaiserAr2Master_UploadVideo(CpuState *cpu);

#endif
