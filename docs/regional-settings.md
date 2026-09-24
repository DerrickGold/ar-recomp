# Regional settings

Open **Settings → Localization → Regional rules** at the title screen or during
a game. Title-screen choices apply to a new game; **Continue** restores the
saved campaign's rules instead. The draft does not change your existing save.
You can choose spell-scroll costs, miracle SP costs, initial room
time limits, checkpoint-retry score handling, development pacing, construction
waits, Fillmore's fishing target, town recovery, earthquake destruction and
the Master report's score page, town-menu return behavior, message-speed
choices, magic controls, monster reserves, house-loss and act-score feedback,
the action HUD's life-count convention, Source activation, the Magic Skull
wait, story prerequisites, lair respawn delays, town growth reports and level
population goals independently. Population support changes use the confirmed
conversion described below rather than an immediate toggle. A title-screen
draft has no developed towns, so it needs no conversion or recovery copy.
The flag and name beside each choice identify its source rules, not the
language of your game.

## Death Heim arrival

**Death Heim arrival** selects the route after all six towns' second acts
are complete. US/Europe shows the island emerging from the sea and takes you
to the Sky Palace for its announcement. Japan returns you to the current
town; your next Palace visit announces the island without the emergence
animation. The island remains available afterward in either case.

Choose before the final departure. Once that sequence begins, its route is
fixed for this campaign, including if you save in town before the Japanese
announcement. Later changes cannot replay the reveal, hide the island or
repeat its rewards. The description identifies the route already decided.
This option uses the existing artwork and music and needs no Japanese ROM;
regional artwork and independent music choices are separate.

## Action and resource rules

### Death Heim artwork

**Death Heim artwork** chooses whether the first statue has the Japanese
horns or the Western design. It applies at the next room entry or retry, in
both native and enhanced background rendering. Boss rules, terrain and
Death Heim's arrival sequence do not change.

The Japanese choice requires a locally extracted `jp.armedia` package.
Without it, US graphics remain active and the description explains the
fallback. Your requested choice stays saved. See
[regional media installation](regional-media.md#install-death-heim-artwork).

### Scene music

**Scene music** selects the regional track assignment. Japan keeps the
Fillmore theme in both rooms of Act 2's caves; US and Europe use the theme
also heard in Kasandora Act 2 and Marahna Act 1. The boss theme is unchanged.
No Japanese ROM is needed: the same Fillmore music data is in the US ROM.

Changing this option does not interrupt the current track. The next scene's
music declaration applies it, keeping the game's normal handling of tracks
that are already loaded. Regional versions of the music itself are a separate
feature; this setting changes the track assignment, not its instruments or notes.

### Aitos mosaic pattern

**Aitos mosaic pattern** changes the distortion in the final room of Aitos
Act 2. US and German releases share one pattern, Japan has another, and
European English and French share the third. The European choice selects
that third pattern; changing the game's language does not select a pattern.

The choice applies at the next room entry or retry and needs no donor ROM.
It does not change the effect's speed, the room's terrain or its enemies.

### Difficulty rules

**Difficulty rules** selects the US/Japanese or European transformations;
**European difficulty** chooses Beginner, Normal or Expert. These are saved
with your campaign and take effect at the next room entry or retry. Selecting
a level has no effect on rules that still use US/Japanese behavior.

| Rule | US / Japan | European Beginner | European Normal | European Expert |
| --- | --- | --- | --- | --- |
| Eligible enemy HP | In action-only mode, 1 becomes 2 | 2 becomes 1 | Unchanged | 1 becomes 2 |
| Eligible base attack | In action-only mode, 1 becomes 2 | Unchanged | Unchanged | Unchanged |
| Accepted enemy-contact damage | Unchanged | Unchanged | Unchanged | +1 |
| Active updates per countdown unit | 60 | 72 | 60 | 48 |
| Aitos dragon projectile attack | Present | Omitted | Present | Present |
| Plant boss's lower tendril bobs per 48 updates | Two | One | Two | Two |

Enemy-type exemptions still apply. These transformations follow the selected
base HP/attack rules; they are not a multiplier for every boss. Contact damage
does not change terrain hazards. The clock keeps its current countdown and
pause gates, and all choices run at 60 Hz. Existing enemies, projectiles and
attacks are never reset by editing a setting. No donor ROM is needed.

The five internal difficulty rules are independently selectable. Enemy
placement, terrain, initial room time and the action-only campaign's entry
and inventory rules are separate features, not implied by this setting.

### Stage terrain

**Stage terrain** selects US, Japanese or European platforms and terrain at
the next room entry or retry. The visible background and collision data change
together. Japan also uses its matching Fillmore entry and retry heights;
switching back restores the US positions. Nothing moves during an active room.

The Japanese layouts reuse existing US tiles, so no donor ROM is required.
European terrain includes changed solidity flags and small layout changes in
Aitos and Northwall. It is independent of European difficulty. Trap damage,
enemy and pickup placement, and regional artwork are separate choices; terrain
alone is not a complete regional stage preset.

### Enemy and item placement

**Enemy placements** and **Pickup placements** independently select each
region's authored encounters and items. European enemy placement also uses
the selected Beginner/Normal/Expert level; European pickups distinguish Story
and Action Mode. These choices need no donor ROM. Item effects, enemy stats,
terrain and artwork remain separate.

Both choices apply at the next room entry or retry, including that room's
later waves. Changing a setting does not replace live enemies or respawn
collected items. The randomizer transforms the chosen placement set using its
existing settings. For an original regional layout, choose matching terrain
and placements; deliberately mixed layouts need not offer the same routes to
every item.

### Stage traps

**Stage traps** selects the ordered damage boxes placed in each room. US
uses the original US traps. Japan restores its additional or differently
positioned traps, often dealing 24 HP. Europe keeps US positions but changes
many spikes to 24 HP, or 2 HP in the opening forest and Northwall's tree.
Some lethal traps are shared by all releases. Slowing zones are unchanged.

The choice takes effect at the next room entry or retry. It neither changes
an ongoing hit nor moves the player, and needs no donor ROM. Difficulty does
not reduce these authored damage values. Terrain and artwork remain separate:
this option alone does not reproduce the complete Japanese stage layout.

### Enemy behavior

**Enemy movement & recovery** selects the behaviors below. The fourteen
rules remain separate internally.

| Behavior | US / Europe | Japan |
| --- | ---: | ---: |
| Fillmore bird horizontal speed | 3 px/update | 4 px/update |
| Fillmore leaping beast's main jump motion | 2 px/update | 3 px/update |
| Fillmore cave type 0F recovery | 60 updates | 40 updates |
| Fillmore cave type 0E straight / high recovery | 58 / 64 updates | 42 / 48 updates |
| Marahna hooded caster's two wind-ups | 56 / 68 updates | 28 / 40 updates |
| Bloodpool skeletal swordsman's straight / high attack | 62 / 63 updates | 46 / 55 updates |
| Marahna trap-arrow speed | 2 px/update | 3 px/update |
| Kasandora ordinary wall-head extra pause, both variants | 31 updates | None |
| Marahna retracting-head withdrawal | 16 updates | 20 updates |

The next room entry or retry captures the choice, including for enemies that
appear later in that room. Changing settings never restarts an attack or
changes an enemy already in the room. The leaping beast's early launch rows
are unchanged. Artwork, damage, collision boxes and vertical motion are
separate; no Japanese ROM is needed. Trap arrows retain the Western flashing
and collision box when changing this speed option. All counts run at 60 Hz.
The wall-head pause does not affect Pharaoh's sphere-created heads. Existing
pauses finish before a newly selected regional rule takes effect next room.

Marahna's ordinary retracting head restores an extra withdrawal pose under
Japanese rules, using retained US graphics and its original collision bounds.
This is separate from the plant boss and Viper; no donor artwork is required.

Fillmore's tree also uses this group. Japanese and European rules restore its
seed phase: two falling seeds sprout into walking plants before withering.
The tree waits129 updates before firing its usual orbs, compared with64 under
US rules. Existing US graphics contain the required poses, so no donor ROM is
needed. A full enemy pool can prevent one or both seeds from appearing; the
tree proceeds normally and does not retry a failed spawn. A setting change
waits until the next room or retry, without interrupting existing plants.

**Cave fireball emitters** controls Fillmore Act 2's firing interval and launch
points. Both are independently selectable internally and apply on the next
room entry/retry. Existing waits and projectiles are not reset.

| Behavior | US | Japan | Europe |
| --- | ---: | ---: | ---: |
| Firing interval, active updates | 360 | 180 | 255 |
| Left-column launch offset | −8 px | −8 px | +6 px |
| Right-column launch offset | −8 px | −8 px | −22 px |

These offsets are relative to the authored positions. The setting does not
move terrain or change projectile damage, motion, allocation or cleanup.
European intervals use the normal 60 Hz simulation; no donor ROM is needed.

**Bloodpool statue volleys** selects one shot in the US rules or two in the
Japanese/European rules. The second shot has its own wind-up and follows the
first by 16 active updates. The complete cycle is 137 updates for one shot or
153 for two. A full enemy pool drops the affected shot without retrying it.
This applies at the next room entry/retry; changing the request never restarts
a volley or removes a pending shot. Placement, damage and appearance remain
separate. No donor ROM is required, and European counts use normal 60 Hz.

**Kasandora fire enemies** selects the ordinary floating flames' movement and
child attacks. US/European rules rise, hover for 64 updates, then fall before
turning; Japanese rules choose a rise or fall according to facing, then turn
without hovering. Each rise/fall lasts 16 updates, with Japanese vertical
speeds reaching 4 pixels per update instead of 3.

The native random-byte decision has 160 no-child, 82 straight-flame and
14 bouncing-flame inputs in US/Europe, versus 128/82/46 in Japan. These are
counts of possible inputs, not a guaranteed firing rate. A full enemy pool
drops the child. Curve shape, close-player sequence and both thresholds remain
independent internally. Choices apply next room/retry, with no new random
draws, artwork requirement or changes to Pharaoh's blue spheres.

**Score-earned lives** controls the European Action Mode reward. US/Japanese
rules do not grant lives for score. European rules grant one life when an
addition crosses a 20,000-point band; a single large addition still grants
only one. This follows the original overflow behavior: 99 stored spare lives
wrap to 00. Story mode is unaffected. Changes apply in the next action room
or retry and never grant rewards for earlier points or reset current lives.
This rule does not enable Action Mode, change starting attempts, or select
European items or difficulty.

**Action Mode starting stats** selects the allowance for a new Action run:

| Rules | Total attempts | Starting and maximum health |
| --- | ---: | ---: |
| US | 5 | 24 |
| Japan | 3 | 24 |
| Europe | 5 | 8 |

Attempts and health are independent internally. A request never overwrites
the current run's lives or health, and ordinary retries and room changes do
not activate it. The native new-run initializer still clears score, scrolls,
equipped magic and sword power. Story mode is unaffected.

This applies to the first Action run and native Game Over restart. Configure
the first run through the title-screen overlay before accepting its mode.
Mode availability and Game Over routing are controlled separately by
**Action Mode entry**; starting allowances alone do not unlock that mode.

**Action Mode entry** uses the original completion unlock under US/Japanese
rules. European rules make Action available without completing Story and
return to the title after Game Over. Enable access in the title overlay, then
press Start or change the highlighted choice. Continue is only offered when
the native save checksum is valid; no completion marker is written.

The European return retains your requested regional rules for the next run
but resets difficulty to Normal. Choose another difficulty in the overlay
before starting. A US/Japanese Game Over starts another Action run directly.
The return policy is captured when you press Start on Game Over. Neither
route changes the battery save; Continue always restores its own rules.
Mode labels remain owned by the selected language, not the rules preset.

**Action Mode items & spells** selects a new run's inventory model. US and
Japanese rules use shared scrolls and the equipped spell. European rules
collect individual spells and cast the most recently collected one first;
the spell is removed only after its effect and graphics restoration finish.
An earlier room transition retains it. Changing the requested rules cannot
convert the current collection or change an in-flight cast's payment.

Under the European model, the extra-life pickup instead adds one current HP
and one maximum HP, each capped at24. Four pickup kinds grant the four spells;
sword power, full healing and the1,000-point reward retain their shared effects.
Inventory survives retries and room changes. Starting HP remains a separate
setting. The native 256-pickup count wrap to zero is preserved. If that wrap
happens during a cast, completion leaves the collection empty instead of
reproducing the cartridge's out-of-range inventory read.

**Action item artwork** independently selects European small spell-HUD icons
and the health-growth graphic when using this inventory model. It applies on
room entry or retry, not during a live pickup or cast. Any supported European
donor supplies the same pixels; Story items remain unchanged. Missing donors
retain US spell icons and the full-apple health-growth fallback, as the menu
description reports. See [media installation](regional-media.md#install-european-action-mode-item-graphics).
The European model also uses its original pickup sound requests, including
the distinct whole-apple and score-item sounds; those require no donor ROM.
First-run profile selection is available through the title-screen overlay.

**Enemy collision shapes** selects Kasandora Act 2's pose bounds and Marahna's
trap-arrow hitboxes. Japanese sword poses reach 36 pixels toward the blade
instead of 28; Japanese arrows extend 16 pixels on each side instead of 8.
Kasandora's other changed pose heights are included. US and European bounds
match. These rules apply next room/retry, independently of speed, damage and
artwork; they need no Japanese ROM. A wider invisible arrow hitbox with the
US artwork is therefore a deliberate mixed choice, not an enlarged sprite.

**Aitos platform skulls** selects weapon deflection, kill reward and explosion
proximity. US/European behavior allows weapon damage, awards 200 points on
death, and triggers an explosion at less than 32 pixels horizontally and
64 vertically. Japanese behavior deflects weapons, carries no kill reward,
and requires less than 24 pixels on both axes. Natural explosions never award
points. The choice applies next room/retry; an explosion already started is
not cancelled. Base HP/contact damage and difficulty promotions are separate
settings, so this option alone does not reproduce European skull HP.

**Enemy stats** selects the authored HP and contact-attack tables for
enemies, bosses and object hazards. The 21 changed HP fields and 42 attack
fields remain independent internally. The next room/retry captures the
selection; it never heals or strengthens an enemy already present. Values
are applied before the existing mode's difficulty adjustment, not as a blanket
damage multiplier. Three independently selectable child overrides apply after
inheritance: Tanzra's second-form projectile attack is 3/4/5 in US/Japan/Europe;
one summoned minion has 2/2/1 HP and gives 20/20/10 points. These values are
assigned at the child's activation, not reapplied to a damaged enemy. Terrain
damage and Aitos skull rewards remain separate. European base values give Aitos's platform
skulls 2 HP and attack2, independently of their deflection/proximity setting.

**Platforms during magic** controls three linked platform pieces in Fillmore.
US/Japanese rules keep them following their parent during a spell; European
rules hold their positions until casting ends. Their left, upper and right
pieces remain independently selectable internally. The choice applies next
room/retry, never midway through a cast. This changes no artwork, rider
collision or parent movement, and requires no donor ROM.

**Boss attack patterns** selects these independent internal rules:

| Behavior | US | Japan | Europe |
| --- | ---: | ---: | ---: |
| Original Minotaur idle | 48 updates | 16 updates | 48 updates |
| Original Minotaur throw follow-through | 12 updates | 11 updates | 12 updates |
| Original Minotaur throw wind-up | 29 updates | 29 updates | 21 updates |
| Original Minotaur jump sequence | 54 updates | 54 updates | 50 updates |
| Minotaur axe's horizontal launch distance | 72 px | 48 px | 72 px |
| Wizard's additional post-spread pause | 31 updates | None | 31 updates |
| Ice Dragon's Death Heim wind-up | 118 updates | 106 updates | 118 updates |
| Tanzra's first-form closing | 64 updates | 36 updates | 64 updates |
| Tanzra's second-form countdown | Resumes remaining time | Stays stopped | Resumes remaining time |
| Tanzra's upper-body turn | 37 updates | 38 updates | 37 updates |
| Antlion's horizontal introduction threshold | X2432 | X2304 | X2432 |
| Antlion's post-volley decision | Wait36, then check distance | Check immediately; if far, wait61 | Wait36, then check distance |
| Aitos dragon projectile movement sequence | 1 update | 1 update | 16 updates |
| Sequences before its first offscreen check | 1 | 1 | 5 |
| Tanzra minion's turn | 16 updates | 16 updates | 8 updates |
| Viper lightning inputs at each random decision | 64 of 256 | 128 of 256 | 128 of 256 |
| Viper's first lightning movement | 22 updates | 22 updates | 18 updates |
| Death Heim Viper's first lightning movement | 11 updates | 11 updates | 9 updates |
| Original Viper floor parts' second descent | 22 updates | 22 updates | 15 updates |
| Pharaoh's landing/bounce | 40 updates | 24 updates | 40 updates |
| Death Heim Pharaoh's landing/bounce | 56 updates | 24 updates | 56 updates |
| Pharaoh's sphere-created wall heads | One shot, withdraw | Repeat every136 updates | One shot, withdraw |
| Marahna plant's head cycle | Always exposed | Opens, closes, then protected | Opens, closes, then protected |
| Plant's open-head animation, one repetition | 8 updates | 8 updates | 16 updates, four poses |
| Plant's high / low projectile preparation | 9 /9 updates | 9 /9 updates | 25 /25 updates |
| Plant body height / initial main-boss Y | 208 px /120 | 192 px /128 | 208 px /120 |
| Northwall Act-1 throw preparation | 50 updates | 50 updates | 15 updates |
| Northwall projectile impact | 42 updates, 32 px final width | 42 updates, 32 px final width | 20 updates, 64 px final width |
| Northwall projectile's facing-relative launch offset | −8 px | −8 px | −16 px |
| Northwall impact's downward offset | 0 px | 0 px | 2 px |

Northwall's four rules are independent internally. Its wider impact reuses
US tiles, so it does not need a donor ROM. The animation and collision box
expand together, including when several impacts are active. Like the other
boss rules, changes wait until the next room or retry.

The axe offset and Wizard pause also apply to their Death Heim rematches.
Viper's choice test, original/rematch lightning travel and floor descent are
independent. The choice counts describe random inputs, not bolts per minute.
The floor segment covers 110 pixels in every region; European lightning
travels less far at unchanged velocity. Existing attacks finish with their
room's captured settings. All counts run at 60 Hz, with no donor ROM required.
The Minotaur's rematch timings and the original Northwall Ice Dragon remain
unchanged. The shorter Ice Dragon wind-up removes two stationary holds from
both its head and body, preserving their synchronization and final positions;
it does not speed up its projectiles. Japan's longer Tanzra turn adds one
update of horizontal movement, so it also shifts the unmirrored path four
pixels left. Europe's minion skips two poses and their interim collision
shapes; it is not an accelerated playback of all four. The clock option
never refills the stage timer. These choices take effect next room/retry
without restarting a fight or changing HP, damage, placement
or artwork. Selecting a shorter sequence does change when its poses and their
collision boxes appear. European counts run at normal 60 Hz. No donor ROM is
required. Aitos's dragon projectiles therefore move for at least 80 updates
under the combined European rules before they can be removed for leaving the
screen, versus 1 under US/Japanese rules. Afterward, the native offscreen check
runs after each sequence. The two choices can be mixed internally; neither
changes velocity, collision shape, artwork or the separate Beginner attack gate.

Pharaoh's original landing, rematch landing and head lifetime are independent.
Japanese landing rules remove only the extra stationary hold after the bounce.
Japanese wall heads wait after firing and repeat; US/European heads withdraw
over30 updates in Kasandora or15 in Death Heim, then disappear. A full enemy
pool can drop an arrow without stopping the head's normal sequence. Existing
arrows keep flying after their head disappears. These choices use retained US
animation data and apply next room/retry without clearing live heads or arrows.

The retracting plant cycle opens for12 updates, repeats the selected open-head
animation five times, closes for12, then stays protected for90. The European
head sequence restores a narrower intermediate pose rather than stretching
the two US poses. High and low projectile wind-ups are separate internally.
These changes use retained US animations; selecting them does not change the
body geometry or the difficulty-specific tendril pattern, which have their
own internal rules. The Japanese geometry profile shortens the body by one
tile row, moves its initial anchor, and adjusts the closed head's bounds as
one operation. It keeps the chosen sprite pixels and palettes; selecting
Japanese geometry alone does not supply Japanese artwork. Switching profiles
waits for a fresh encounter and is reversible without starting a new game.

| Cost | US / Europe | Japan |
| --- | ---: | ---: |
| Fire | 1 scroll | 1 scroll |
| Stardust | 1 scroll | 2 scrolls |
| Aura | 1 scroll | 3 scrolls |
| Light | 1 scroll | 4 scrolls |
| Lightning | 10 SP | 12 SP |
| Rain | 20 SP | 16 SP |
| Sunlight | 30 SP | 18 SP |
| Wind | 80 SP | 24 SP |
| Earthquake | 160 SP | 60 SP |

A change applies to the next cast or miracle. A miracle already being chosen
keeps its original price through confirmation, cancellation and completion.
The description shows the selected prices; **Last activated** identifies the
rules used by the most recent transaction. Insufficient resources prevent a
cast without deducting anything.

The initial-time option changes six action-room profiles. Room numbers here
count sections within each region, not acts.

| Room | US / Europe | Japan |
| --- | ---: | ---: |
| Fillmore 1 | 300 | 200 |
| Fillmore 3 | 200 | 100 |
| Bloodpool 1 | 300 | 200 |
| Marahna boss / its Death Heim rematch | 300 | 200 |
| Northwall 1 | 200 | 100 |
| Northwall 2 | 200 | 100 |

Other room limits are unchanged. The choice takes effect when the game next
initializes an action room, including a retry. Changing it never refills or
shortens a countdown already running. The countdown cadence stays unchanged:
choosing Europe does not switch the game to PAL timing.

**Checkpoint score** chooses whether a retry keeps the current action score
(US/Europe) or clears it (Japan). This applies when the game consumes its
checkpoint-retry marker, not when you change the setting. It does not alter
life allowances, act-clear rewards, returning to the Palace after the last
life, or starting another act/run; those paths keep their own native behavior.

**Action HUD lives** changes how the same remaining attempts are labeled.
US/Europe displays 3 for three attempts and 1 for the last; Japan displays
2 and 0 respectively. It takes effect on the next HUD redraw with either
native or enhanced text. Starting lives, retries, extra-life rewards and the
Palace's Master report are unchanged.

**Source activation** chooses when Source of Life and Source of Magic take
effect. US/Europe applies the bonus when you collect the offering; Japan
stores it for **Use Offering**. Source of Life adds one to the persistent
life allowance, not maximum HP. Source of Magic adds a scroll to the persistent
allowance and the working scroll count. The native bonus amounts are unchanged.

The choice applies to newly collected Sources. Previously carried Sources
remain available under either rule, and automatic collection does not consume
one of those older items. Changing the setting grants nothing by itself.
The native eight-slot inventory limit still applies: a full inventory blocks
Take Offering, including automatic Sources.

**Magic controls** uses A or X for the US/European standing cast, or Up + Y
for Japan's ground-attack cast. In the Japanese scheme A and X no longer cast;
Y alone still swings the sword. Airborne and crouching attacks retain their
native behavior. These are game buttons: your physical controller and keyboard
bindings stay unchanged. A change waits until Up, Y, A and X are released, so
holding a button while switching does not accidentally cast. Scroll prices,
spell inventory and the ordinary casting restrictions are separate.

**Level population goals** selects the population needed for each Master level.
Both tables begin at 80, 200 and 400. The fourth goal is 700 in US/Europe or
550 in Japan; the final goal is 4,600 or 3,000 respectively. Earned levels are
never removed. Houses, population support, rewards per level and the level-17
maximum remain unchanged.

The next native level-check sequence uses the selected table throughout its
awards and dialogue. Opening the Master report refreshes its next-level target
without awarding levels or healing. Changing the option itself grants nothing;
lower goals can make you eligible for additional awards at the next normal
check. At maximum level the report continues to show a next goal of zero.

## Population rules and rebuilding

**Population & town conversion** selects these support capacities together
with the region's level and story population goals:

| Structure | US / Europe | Japan |
| --- | ---: | ---: |
| Ordinary field | 32 | 16 |
| Improved field | 48 | 24 |
| Aitos/Marahna factory | 72 | 32 |
| Other supporting structures, including bridges | 32 | 16 |

House occupancy is unchanged. Lower support means towns need more production
buildings and have less room for houses. To avoid leaving an existing town
stuck with an unsuitable layout, a capacity change requires redevelopment:

1. Choose the region, return to the Sky Palace, and finish a menu action.
2. Review the number of houses, fields and factories to remove from each town.
   Cancel leaves the layout and rules unchanged.
3. **Save & apply** saves current progress, creates a recovery folder beside
   your save, removes those buildings, and saves the new rules and town state.

Roads, bridges, landmarks, civilization levels, story progress, sealed lairs,
offerings and earned Master levels remain. There is no SP charge or ordinary
earthquake reward. Towns rebuild through normal construction, with a bounded
growth top-up when needed; this is not instant rebuilding. No Japanese ROM is
needed. The next town visit redraws its surviving structures.

Switching back requires another redevelopment; it does **not** restore the
old layout. To restore that layout, use the complete pre-change recovery copy
as described in [Save formats](save-format.md). A US/Europe switch keeps the
same support capacities, so it does not remove buildings. Japanese support
requires Japanese population goals; those goals cannot be switched separately
to the higher Western requirements while Japanese support remains selected.
Unrelated Compass prerequisites stay unchanged by this conversion.

Pending conversions are not saved or resumed after a restart. Changing another
regional rule invalidates an earlier request; select the population choice
again to review it. Inconsistent town records or unsafe pending work prevent
conversion. A save failure before conversion leaves the layout and rules
unchanged; a failure saving the enhanced player name after a successful
conversion does not repeat or undo the conversion.

## Other town rules

**Town growth reports** chooses how the Cities report classifies development.
US/Europe checks growth, buildings and population; Japan reads stored status
flags. The Japanese construction-status rules use a fixed low-growth threshold
of 4, expect one extra plot per town, count available food plots as construction
attempts, and discard newly computed warning flags. These are reporting rules,
not population caps or changes to how many residents a house holds.

The choice applies to the next complete report or construction calculation.
Selecting it does not rewrite existing flags or structures. A report opened
before a new construction calculation may therefore still reflect flags
produced under the previous rules. The translated labels follow the resulting
status code; this option does not change the selected language.

**Town monster combat** selects the durability and contact damage of newly
spawned town monsters. With ordinary arrows, US/Europe takes 3 hits for a Blue
Dragon and 4 for a Red Demon; Japan takes 2 and 3. Bats still take 1 and Skull
Heads 8. Contact damage for Dragon/Bat/Demon/Skull is 3/1/6/8 in US/Europe and
2/1/3/4 in Japan. SP rewards, movement, targeting and respawn timing are separate.

Existing monsters keep their original combat rules, including after leaving a
town or saving and continuing. Changing back to US does not alter a surviving
Japanese-rule monster. Older saves start their existing monsters with US rules;
only a verified new spawn adopts the selected setting. These rules require no
Japanese ROM or graphics extraction.

**Town monster behavior** controls how newly spawned monsters look for targets
and how Dragons and Bats carry out certain attacks:

| Behavior | US / Europe | Japan |
| --- | --- | --- |
| Blue Dragon target search | Every eligible update | Every eighth eligible update |
| Extra actor update when a Dragon strikes | Yes, including other active actors | No |
| Candidate target cells | Local range, offset within the town | Whole 32-cell map |
| Structure records checked for a candidate | A rotating slice of 16 | All 128 |
| Bat fallback to the angel after no house target | Random result 253–254 | Random result 250–254 |
| Bat abduction wait | 1 eligible update | 60 eligible updates |

The fallback draws from the native 0–254 random range; it does not bypass the
normal targeting checks. Update counts are not seconds, and all choices use
the game's normal 60 Hz clock. Existing monsters keep their behavior when
settings change, including across town visits and save/continue. Combat damage
and respawn delays remain separate options. No Japanese ROM is required.

**Lair respawn delays** changes how long an eligible, empty lair waits before
its next monster appears. US/Europe normally reloads a delay of one town update;
one Aitos lair uses 141, and two Northwall lairs use 100. Japan's lair-specific
delays range from 37 to 275 updates. These are eligible town-service calls,
not seconds: sealed lairs, exhausted reserves, existing monsters and native
pause rules still control whether a countdown advances.

Switching changes the reload table at the next safe town update, leaving every
countdown already running untouched. Prior native delay reductions are retained
for both choices; switching back restores that choice's retained values. This
does not change monster reserves or introduce new events. Old-save histories
are estimates because the save cannot reveal every past reduction; unexplained
values are preserved and prevent this setting from being switched.

**Construction wait** sets the pause between town construction phases: one
town update for US/Europe, or 150 for Japan. These are calls to the town's wait
handler, not seconds. Each town finishes its current wait before using the new
reload. Switching back does not immediately erase an outstanding Japanese wait.
The broader development-cycle pacing, monster updates and recovery rules remain
unchanged; this option alone does not reproduce Japan's complete town pacing.

**Development clock** changes how often the town's development work runs.
US/Europe services it on each eligible master-loop call, with a long cycle of
720 services. Japan services it every fifth call, with a long cycle of 480
services. Japan also updates structure visuals every fifth effect-service call;
those calls share the same divider phase. Monster updates run independently.
The current long cycle finishes before a changed setting takes effect, without
resetting its clocks. Open menus and effects retain their native pause rules;
these counts are not a promise of elapsed seconds. Construction waits, fishing
targets, HP/SP recovery and the host's 60 Hz clock are separate.

**Town recovery** selects the routine HP/SP recovery rules. US/Europe replaces
the pending recovery amounts each long cycle with one tenth of maximum SP and
one quarter of the angel's maximum HP, rounded down. The angel's normal update
drains one queued SP on every fourth frame phase and one HP on every sixteenth.
Japan has no cycle-based SP refill; the angel instead recovers one HP every
60 eligible movement-service calls. Native pauses still apply, including the
Japanese exclusion for angel state 4. These are service counts, not seconds.

The choice applies at the next recovery service. Changing a recovery rule
discards its old pending queue or partial clock without removing HP/SP already
earned. It does not grant an immediate refill. Switching between equal US and
European rules preserves the queue. Level-up, lair-sealing and other rewards
remain independent, as does the selected development clock.

**Earthquake destruction** changes the structure-selection rules for the next
player or monster earthquake. US/Europe protects the highest-tier houses,
improved fields, windmills and factories. Japan uses the native random test for
these categories, so upgraded buildings can also be destroyed. Other native
structure categories retain their respective regional rules; bridges retain
their shared gate-dependent behavior. These rolls use the game's RNG and
normal structure processing, not a separate host random generator.

The whole effect keeps the choices captured when it started. Changing the
setting does not cause an earthquake, change its price, or alter the existing
house-loss contributions and player-versus-monster resource side effects.
It is separate from the planned town-redevelopment reset.

**Master score page** controls the report shared by the Sky Palace and town
menus. US/Europe opens the act-score page when you press B after the Master
report; Y closes it directly. Japan closes the report with either button.
Scores continue to be recorded even when their page is hidden. The choice
applies on the next report opening and never closes an already-open page.
It does not change whether the surrounding command menu remains open.

**Town menu return** controls what happens after Master status, city status,
Progress Log, and Message Speed. US/Europe closes the command menu and resumes
town play; Japan keeps it open at the same selection. The command captures the
choice when accepted, so a change during a report applies to the next command.
Reports still clean up their own text and objects, saving still uses the normal
save transaction, and cancelling Message Speed keeps its previous setting.
Palace menus and other town commands are unchanged. Both Original and Modern
town-menu styles use this rule.

**Message-speed range** offers 0–9 for US/Europe or 0–7 for Japan. The choice
applies the next time the speed selector opens in the Palace or a town; an
open selector keeps its range. If the current speed is 8 or 9, Japan's cursor
starts at 7, but the stored speed is not changed until you confirm. Cancelling
preserves the old value. The numeric row and cursor use the same range with
native or enhanced text and either town-menu style. This changes the offered
choices, not the timing of each speed or the selected language.

**Fillmore fishing** sets the Compass expedition's target to 255 event updates
(US/Europe) or 128 (Japan). These are eligible event updates, not seconds; the
development cycle controls how often they occur. A change takes effect at the
next fishing update without resetting progress. Lowering the target past the
current progress completes the event once through its normal reward path.
It cannot repeat a completed reward. Native Palace visits still restart an
unfinished expedition. Marahna fishing, Northwall's lake search and automatic
versus manual collection of Sources are unchanged.

**Monster reserves** selects the starting stock behind each town's four lairs.
These are the combined starting totals, before kills, miracles, house losses
and action-score settlements change them:

| Town | US / Europe | Japan |
| --- | ---: | ---: |
| Fillmore | 500 | 600 |
| Bloodpool | 330 | 400 |
| Kasandora | 450 | 900 |
| Aitos | 320 | 600 |
| Marahna | 210 | 600 |
| Northwall | 180 | 500 |

The game keeps each alternative count up to date using the events you actually
play through. Switching resumes those retained counts, rather than refilling
lairs or trying to simulate a second playthrough. The change waits for a safe
town update or score settlement after any active miracle or earthquake. A sealed lair stays sealed;
an exhausted but unsealed lair can produce monsters again if its newly selected
count is positive. Existing monsters, spawn clocks, rewards and sealing progress
are not reset. House-loss and score-conversion rules remain separate from this
choice. Missing or inconsistent history prevents a switch instead of guessing
again or overwriting your progress.

**House-loss feedback** changes the amount contributed by each destroyed house.
US/Europe uses 4, 6 or 8 units according to the house's tier; Japan always uses
four. The game distributes these units among unsealed lairs, replenishing their
monster reserves. If all four lairs are sealed, it adds them to the town's
growth balance instead. This does not choose which houses are destroyed or
change how many residents a house supports.

Like monster reserves, this choice activates at a safe town update or score settlement using
retained stock history. An earthquake already running keeps its rule throughout.
Growth already awarded stays awarded: switching does not undo it or grant the
other region's past rewards. Save with the Progress Log to keep the choice.

**Act-score feedback** selects how a completed act contributes to its town.
The formulas below use the stored score: divide the displayed points by ten.

| Rule | US / Europe | Japan |
| --- | --- | --- |
| Converted units | Twice the score divided by 10, rounded down | Subtract 650 (minimum zero), divide by 32 and round down, then multiply by 10 |
| Stock adjustment | Add a quarter of the units to each lair | Subtract a quarter from each lair, stopping at zero |
| Destination | One completed act: stocks; two: growth; other counts: neither | Two completed acts: growth; all other counts: stocks |
| Settlement | At departure, after the final tally | At the clear card, before the final tally |

Stock shares round down. Sealed flags are unchanged. The growth route adds the
converted units once through the native town-growth routine. Changing the option
selects retained stock history at a safe town update or before the next clear;
it never repeats an earlier growth reward. A clear captures its rules and keeps
them through departure, even if you change a setting during the tally. Japan's
earlier award is not repeated at departure. Settlement timing changes future
clears only: every retained stock history uses the score from the one settlement
you actually played, not a second hypothetical tally.

**Magic Skull wait** keeps the US/European 90-frame pause after sealing, or
uses Japan's immediate consumption. The choice is captured when you begin
Use Offering and stays fixed through the picker and completion. This does not
change which lair accepts the Skull, its growth reward, or cancellation: an
invalid or cancelled use retains the item.

**Story prerequisites** changes two population checks:

| Event | US / Europe | Japan |
| --- | ---: | ---: |
| Fillmore's southeastern rock/magic hint | More than 110 people | More than 88 |
| Kasandora's Ancient Tablet discovery | More than 700 people | More than 400 |

The next native event check uses the chosen thresholds. Previously enabled
population prerequisites remain enabled; completed events are not replayed,
and changing the setting does not grant an item. Other story conditions and
event priority still apply.

The same bundle includes Bloodpool's failed-Act-2 check: US/Europe clears the
Compass prerequisite alongside the disputes prerequisite; Japan clears only
the disputes prerequisite. The normal event loop still gives disputes priority.
This is not an option to obtain the Compass early.

## Saving your choices

These choices belong to the current campaign. **Save with the Progress Log to
keep them.** Closing the overlay is not a save. New Game starts with US rules;
Continue restores the choices saved with that campaign. Keep the matching
`.archeckpoint` companion beside your save when copying it to another Recomp
installation. The `.srm` itself remains compatible with SNES emulators. See
[save companions](save-format.md#regional-campaign-checkpoints) for backup and
recovery details.
The confirmed population conversion is an exception: it saves immediately and
also keeps a complete pre-change recovery copy.

On the first normal Continue of an older save, the game asks to estimate its
missing regional monster-lair history. The estimate preserves current US counts
and approximates the other-region counts; earlier kills and house losses cannot
be reconstructed exactly. Accepting records the estimate once in the companion,
without changing your towns, sealed lairs or native SNES save. Cancel returns to
the title. New games track from the beginning and do not need this estimate.
If the estimate cannot be saved, you can retry or cancel; the game does not
silently discard an existing history.

Regional changes are locked during recording and replay, including after
taking over from a replay. Start a normal session to edit them.

These are **individual options, not complete regional presets**. They do not
change spell inventory, action-enemy behavior or stats,
artwork or language. In particular, choosing European costs
does not enable the European Action-mode spell stack. No donor ROM is needed
for these rules. The [regional comparison](regional-differences.md)
describes the wider set of differences separately.
