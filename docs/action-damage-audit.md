# Enemy damage and sword beam audit

Audited 2026-09-27 against the US ROM and the current generated game code.

No duplicate same-pass damage was found. A sword and beam hitting the same enemy
in one collision pass subtract **2 total**, not 4, when both have attack 2.
However, the native beam survives contact and can deal another 2 after the
enemy's hit timer expires. A controlled wide-target fixture reproduced **4 total
damage from one beam**, with hits nine object updates apart, in both the recomp
and the original ROM. The ranged-sword setting also raises melee damage from 1
to 2 because it enables the original sword power-up.

Follow-up testing reproduced the reported **Fillmore Act 2 Minotaur** behavior
in a running original-ROM encounter: one beam reduced HP **24 → 22 → 20**, nine
updates apart, with the player outside melee reach. The production generated
object loop and collision resolver reproduced that sequence and matched native
WRAM. This establishes that the reported two-hit behavior is authentic; it does
not establish which earlier build behaved differently. Combat behavior was left
unchanged, as requested.

## Minotaur encounter follow-up

A local Snes9x state from the existing native Minotaur research was restored.
The ROM was unmodified. Controlled setup writes enabled the sword power-up
(`$E4 = $80`) and placed the player at X=160 facing left; the existing boss was
at X=68. One Y-button press on relative frame 4 created one beam. Boss AI,
movement, animation, hitboxes, HP subtraction, and protection timers then ran
normally. This was a controlled encounter, not a recording of the user's fight.

| Relative frame | Boss HP | Beam X | Hit timer |
| --- | --- | --- | --- |
| 11 | 22 | 152 | 8 |
| 19 | 22 | 88 | 0; still protected |
| 20 | 20 | 80 | 8 |

The beam occupied slot `$0920` throughout. At each impact, replaying collision
with only the beam attack enabled still removed 2 HP; enabling only the sword
removed none. Starting from the native pre-impact state, 14 consecutive
production object-update/collision pairs also reproduced both hits. Those 28
calls plus six attacker-isolation calls matched the original routines across
WRAM with the same stack/mailbox exclusions as the main regression below.

For the historical check, the recompiler and configuration from pre-regional
commit `8041338b` (2026-09-21) were exported into scratch storage and built.
Separately emitted M=0/X=0 bodies for `$8915`, `$8A3C`, `$8B67`, `$8E2F`, `$9CF2`,
`$9D1C`, `$9DC8`, and the Centaur entry `$AD51` were byte-for-byte identical to
current emissions. The ranged-sword setting also used the same `$E4 = $80`
write. The Minotaur entry differs at the regional axe-offset interception;
regional animation settings can change its timing. This source comparison is
not a replay of a complete older executable, and does not prove an old bug was
fixed. No historical damage regression was demonstrated.

## Ownership and collision contract

The action enemy-hit resolver is generated from the ROM, rather than replaced
by an HLE damage implementation. Its dependencies and adjacent HLE adapters were
reviewed as follows:

| Owner | Verified behavior |
| --- | --- |
| `src/actraiser/actraiser_cheats.c`, `$E4` | Ranged sword pins `$80`. It does not subtract enemy HP or clear enemy hit timers. |
| `$00:9DC8–9DE0` | Nonzero `$E4` selects melee attack 2; zero selects 1. |
| `$00:9CF2–9D1B` | Beam creator explicitly sets attack 2, attacker flag 1, handler `$9D1C`, and the selected original animation. |
| `$00:8A3C` | Scans attackers, then victims. Victim status mask `$6C00` and flags mask `$2429` reject ineligible records, including already-hit flag `$0008`. |
| `$00:8B67` | After the broad rectangle check, checks eligible composition parts and returns on the first overlap. Six beam parts do not produce six HP subtractions. Signed beam extents are retained. |
| `$00:8AE8–8B12` | Victim flag `$0800` deflects before subtraction. Otherwise sets hit flag `$0008` **before** subtracting attacker attack once from HP. A surviving victim gets timer `+$26 = 8`. |
| `$00:8915`, particularly `$8930–8940` | Decrements the hit timer once per object update and retains the hit flag while the result is nonnegative. Starting at 8, it clears on update 9 when the timer reaches `$FFFF`. |
| `$00:9D1C` | Moves a living beam at 8 pixels/update and retires it on lifetime expiry or outside-activation flag `$0400`. Contact itself does not consume the beam. |
| `$00:8B14–8B47` | Ordinary enemies clamp lethal HP to zero and enter `$8892`; bosses enter `$A54A`. Repeated same-pass attacks cannot award ordinary-enemy score again. |
| `actraiser_action_motion.c` | Regional collision adapters affect guarded enemy families; player/beam animation uses the original decoder. |
| `actraiser_actor_stats.c`, `actraiser_difficulty.c` | Regional/randomizer HP and attack selection occurs at guarded births; the difficulty contact hook concerns damage **to the player**. Neither adds an enemy sword-damage subtraction. |
| `actraiser_widescreen_sprites.c` | Activation changes preserve the other object flags, including hit protection. Drawing is separate from the enemy damage pass. |

There is no per-projectile list of previously hit enemies. Protection belongs to
the victim. Another attack, including the same still-overlapping beam, becomes
eligible when that protection expires. A beam can also damage different enemies
in one pass; each victim gets its own hit flag and HP subtraction.

## Executed checks

`tests/action_damage_generated_test.py` and its C fixture link the production
generated callers, native collision parts, animation decoder, object loop, and
return dispatcher. The fixture substitutes only the program entry and audio
interrupt delivery; BRK/COP preserve their native sound-request memory writes.

All **296 generated routine calls** matched the original US routines executed
in Snes9x. Comparisons cover all 128 KiB of WRAM except the fixture's hardware
stack and oracle result/counter area (`$1E00–1F1F`). The generated fixture also
requires normal returns, balanced stacks, and zero diagnostic trap warnings.

Coverage includes:

- Both beam states (`$13/$14`) and horizontal facings over 45 victim positions
  each, using the actual ROM compositions and signed collision bounds.
- Normal and powered melee attacks, plus simultaneous sword/beam overlap with
  the beam ordered both before and after the player in the actor pool.
- Two consecutive collision passes without an object update: still only one hit.
- All early victim rejection bits, post-contact deflection, and attacker gates.
- HP 0/1/2/3, ordinary-enemy versus boss death, single score awards, two victims,
  and expired/offscreen beam retirement.
- Two 20-update sequences using the original object loop and beam movement:

| Object updates since first hit | Narrow target HP | Wide target HP | Wide target hit timer |
| --- | --- | --- | --- |
| 0 | 30 | 30 | 8 |
| 1–7 | 30 | 30 | 7 down to 1 |
| 8 | 30 | 30 | 0; still protected |
| 9 | 30 | 28 | Reset to 8 by the second hit |
| 10–19 | 30 | 28 | Counts down; the beam then leaves the target |

Both targets start at 32 HP. The wide target is a controlled stationary rectangle
with horizontal half-width 64, not a claim that every boss pose has those bounds.
The main regression does not replay a full boss AI fight, active spell
interaction, arbitrary mixed regional profile, or presentation timing. The
separate Minotaur encounter follow-up above exercises the specific reported boss.

The five existing focused CTest suites also passed: `actraiser_boss_rules`,
`actraiser_action_motion`, `actraiser_difficulty`, `actraiser_platform_skull`,
and `randomizer_stats`. The new C fixture and changed cheat-source comment pass
the house style rules. The repository-wide style check reports four existing
packed-statement violations in the separately modified
`src/sim/voxels/sim_background_voxel_models.c`; that file was left untouched.

## Reproduction

Build the current `play` preset, then run the regression described in
[`tests/README.md`](../tests/README.md#generated-action-damage-regression-local-rom).
It writes a JSON result report and temporary fixture files into the selected
output directory. The ordinary run needs no emulator.

The audit's optional original-ROM comparison used the existing private native
oracle helper and locally built Snes9x core:

```sh
python3 tests/action_damage_generated_test.py \
  --output build-check/action-damage-audit \
  --oracle-helper development/tools/regional_native_probe.py
```

US ROM SHA-256:
`b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0`.
ROM data and the emulator core are not included in the regression fixtures.
