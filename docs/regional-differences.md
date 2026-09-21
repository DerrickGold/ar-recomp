# ActRaiser: regional differences

ActRaiser's Japanese and US releases differ in magic costs, enemy attacks,
town development, and presentation. This article compares their mechanics
using the US release as the baseline, with a separate section on European releases.

Findings are supported by ROM analysis or tests of the original game unless
marked **reported** or **under investigation**. Those labels identify claims
that still need verification.

This is a comparison of the original releases, not a list of regional options
already implemented in ActRaiser Recompiled. It contains story and boss spoilers.

## Contents

- [At a glance](#at-a-glance)
- [Action mode](#action-mode)
- [A tour of the action stages](#a-tour-of-the-action-stages)
- [Simulation mode](#simulation-mode)
- [Sky Palace, story, graphics, and music](#sky-palace-story-graphics-and-music)
- [European releases](#european-releases)
- [About the research](#about-the-research)

## At a glance

| Feature | US release | Japanese release |
| --- | --- | --- |
| Action magic cost | One scroll per spell | One to four scrolls, depending on the spell |
| Starting attempts in the action-only mode | Five | Three |
| Score when respawning at an action checkpoint | Retained | Cleared |
| Town development | More frequent development work | Slower development schedule and some longer waits |
| Routine SP recovery | Replenishes part of the meter each construction cycle | No equivalent periodic refill |
| Angel's health recovery | Recovery arrives with construction cycles | Regular, small increments between cycles |
| Earthquake and housing | Highest-tier houses are protected | Houses are subject to random destruction, including upgraded ones |
| Town support and level requirements | More support from fields and other structures; higher later population milestones | Less support; lower later population milestones |
| Initial monsters remaining in lairs | Generally fewer | Generally more, with different spawning delays |

## Action mode

### Casting magic

With the original controls, the Japanese game casts magic through Up plus
the attack button. The US game accepts A or X for magic, separating casting
from sword attacks while holding Up.

Spell costs also differ:

| Spell | US scroll cost | Japanese scroll cost |
| --- | ---: | ---: |
| Magical Fire | 1 | 1 |
| Stardust | 1 | 2 |
| Aura | 1 | 3 |
| Light | 1 | 4 |

In Japan, the more expensive spells consume reserves that could otherwise
fund several casts of Magical Fire.

### Lives, continues, and score

The action-only mode is called Professional! in the US and Special in
Japan. It starts with five playable attempts in the US and three in Japan.

The lives display uses different conventions. The US number includes the
current attempt; Japan displays the spare attempts. A Japanese display of
zero therefore still accompanies a playable last attempt.

On an action checkpoint retry, the US game keeps the accumulated score while
Japan clears it. When the player runs out of attempts in normal play, the
tested return to the Sky Palace preserves the score in both versions;
departing to fight again clears it. Restarting after the action-only mode's
Game Over starts a fresh score in both versions.

### Apples: healing and placement

A half apple restores one quarter of maximum health, rounded down, in
both tested releases. A whole apple restores the missing health. For example,
a half apple restores six points when maximum health is 24. Recovery is
applied gradually, with the same cadence and maximum-health limit in both games.

Placement changes affect how much health is available. One Fillmore Act 1
pickup is a whole apple in the US game and a half apple in Japan. The broader
item-placement comparison is still being matched to individual landmarks.

### Time limits, hazards, and enemy strength

Several action sections have larger starting timer values in the US game.
Confirmed examples include the opening sections of Fillmore and Bloodpool
(300 US versus 200 Japanese), and sections of Northwall Act 1 (200 versus
100). Marahna's Act 2 boss section and its Death Heim counterpart also use
300 versus 200. These are values on the game timer, not measurements in
wall-clock seconds; a limit can belong to one section rather than an entire act.

Enemy health, attack strength, terrain, traps, and pickups also vary. Some
Japanese attacks are considerably stronger, while at least one compared Aitos
enemy attack is weaker. Trap danger depends on placement and collision rules
as well as damage.

Professional/Special mode promotes eligible one-point attack or health values
to two in both releases. Higher values are unaffected by this particular rule.

See the [action-rule evidence](regional-differences-technical.md#action-rules-in-code)
and [layout and collision comparison](regional-differences-technical.md#collision-and-animation-joins)
for the complete measured tables.

## A tour of the action stages

The comparisons below cover the examined attacks and phases. Frame counts
measure active gameplay, excluding pauses.

### Fillmore

**Act 1: tree attacks.** In Japan, the orb-spitting tree also releases two
seeds. They fall to the ground and become moving plant
enemies before the tree fires its two familiar orbs. The US version leaves
out this seed-and-plant phase.

The measured pre-shot hold lasts 129 frames in Japan and 64 in the US. The
seed phase and longer hold occur in both normal and Special mode.

**Act 2: Minotaur timing and axe placement.** The examined idle phase
lasts 48 frames in the US fight and 16 in Japan. The thrown axe also starts
closer to the boss: 48 pixels away horizontally in Japan, versus 72 in the
US, mirrored according to which way the boss faces. Projectile speed, other
phases, and the Death Heim rematch still need their own comparisons.

**Reported:** altered spikes and pickups, fewer active firing statues in
Act 2, and several redrawn enemies. The reported Act 2 theme change is covered
under [Music](#music).

Evidence: [tree attack](regional-differences-technical.md#fillmore-act-1-tree-seed-controller-and-pre-shot-wait),
[Minotaur](regional-differences-technical.md#minotaur-timing-and-room-inheritance).

### Bloodpool

**Act 2: statue volleys.** US statues fire one fireball per volley. Japanese
statues fire two, 16 frames apart. The second shot has its
own firing animation.

The extra shot makes the complete Japanese cycle longer: 153 frames between
the first shots of successive volleys, compared with 137 in the US. Both
facing directions behave this way in normal and Special mode. All ten statue
placements match between versions.

**Reported:** a whole apple added before the Act 1 boss, an upgraded apple in
Act 2, and redraws of several enemies and projectiles.

Evidence: [statue volleys](regional-differences-technical.md#bloodpool-act-2-statues-single-versus-double-volley).

### Kasandora

Terrain and enemy-statistic differences are present in the ROM data, but the
named encounter comparison is still under investigation.

Reported changes include slower wall-head firing and blue-sphere movement in
the US game, a longer opportunity to attack the Pharaoh while it is grounded,
and a different relationship between the boss's spheres, wall heads, and
arrows. A more forgiving landing ledge and changes to apple pickups are also
reported.

### Aitos

The data contains changes to enemy health, attack strength, and a protection
flag; their effects on individual encounters are still under investigation.

**Reported:** the moving-platform skulls become destructible in the US game,
volcanic lava rises more slowly, and the small humanoid enemies and tornadoes
have revised graphics or animations. Act 2 has not yet had a named encounter
comparison.

### Marahna

**Act 1: boss vulnerability.** The US boss keeps its vulnerable head exposed
in the examined attack loop. The
Japanese boss cycles through opening, remaining open, closing, and a protected
closed period. In the measured sequence, those phases last 12, 40, 12, and
90 frames respectively.

The Japanese boss rejects sword damage while closed, including contacts that
damage it while open. This protection occurs in normal and Special mode.
The boss starts with 24 health in both versions.

The measured main boss position is eight pixels lower in Japan, and its body
uses a different arrangement of sprite parts.

**Reported:** changed apples, a removed walking fireball in Act 2, a magic
pickup changed to a 1UP, less lethal spike sections,
and flashing trap arrows.

Evidence: [boss phases, damage protection, and placement](regional-differences-technical.md#marahna-act-1-boss-vulnerability-and-placement).

### Northwall

**Act 2: Ice Dragon wind-up.** The examined wind-up lasts 118 frames in both
regions during the original
Northwall fight. In Death Heim, the US rematch still takes 118 frames, but
the Japanese rematch takes 106. Japan removes a short pause from the sequence,
bringing the next attack forward by 12 frames.

In both regions, the rematch doubles the horizontal speed of its ice balls.
These timing results hold in normal and Special mode.

**Reported:** apple and 1UP changes, a wider platform around a magic pickup,
and fewer lethal spikes around the water-area 1UP.

Evidence: [original fight and Death Heim rematch](regional-differences-technical.md#northwall-act-2-boss-original-versus-death-heim).

### Death Heim

The confirmed Ice Dragon rematch difference is described under
[Northwall](#northwall). Other rematch phases and the final battle's timing
remain under investigation.

The arrival of Death Heim on the world map is discussed
[below](#death-heims-appearance).

## Simulation mode

### Town development and recovery

Towns develop more slowly in Japan. Routine development work runs less often,
construction cycles have different lengths, and some town-state waits are
longer. Monsters, miracles, menus, and recovery use separate timing rules.

In a controlled starting-town comparison, a construction cycle took 732
active frames in the US game and 2,412 in Japan—about 3.3 times as long in that
situation. Developed towns and event interruptions can change that ratio.

Health and SP recovery follow different schedules too:

- **SP:** the US construction cycle starts a gradual refill of one tenth of
  maximum SP, rounded down. Japan has no equivalent periodic refill and relies
  on other sources of SP recovery.
- **Angel health:** the US cycle starts a gradual recovery of one quarter of
  maximum health, rounded down. Japan instead has a separate timer for
  restoring one health point at a time, up to the maximum. In ordinary play
  this produces smaller, more regular recoveries between construction cycles.

Both games require the angel to have health remaining to fire a normal arrow.

Opening the tested town movement menu freezes development, monsters, and
recovery. During a miracle, development can stop while monsters and recovery
continue.

Evidence: [development and recovery](regional-differences-technical.md#scheduler-and-sim-enemies).

### Miracle costs

Lightning costs slightly more in Japan; the other four miracles cost less.

| Miracle | US SP cost | Japanese SP cost |
| --- | ---: | ---: |
| Lightning | 10 | 12 |
| Rain | 20 | 16 |
| Sunlight | 30 | 18 |
| Wind | 80 | 24 |
| Earthquake | 160 | 60 |

The lower Japanese prices accompany the absence of regular construction-cycle
SP recovery.

### Earthquakes and rebuilding

The US earthquake protects the highest-tier houses while removing the lower
tiers. That makes it useful for clearing space for better housing. Japanese
earthquakes instead use a random destruction test for houses, including
upgraded ones. The number of buildings destroyed varies from cast to cast.

The difference extends beyond housing. Several structure categories that the
US rules preserve are subject to random destruction in Japan; field treatment
also depends on the field's type. Bridges have a separate rule and survived
the tested combinations in both releases.

The effect also depends on its cause. A player's earthquake costs SP and can
directly reduce eligible lairs' remaining monster counts. A Skull Head's
earthquake does neither. Both can still destroy houses,
and those losses have consequences for monsters and town growth.

Each destroyed house contributes four, six, or eight points in the US game,
according to its housing tier. Japan contributes four regardless of tier.
These points replenish unsealed monster lairs; when all lairs are sealed,
they contribute to town growth instead. The game distributes each house's
contribution separately. A player's earthquake can therefore reduce a lair's
count and then replenish it during the same effect.

Evidence: [earthquake behavior](regional-differences-technical.md#earthquake-class-policy).

### Housing and population support

Both versions give the same basic house tiers four, six, or eight residents.
Other structures provide *support*: an allowance used to decide whether more
housing can be built. Those support values differ between regions.

For example, a completed ordinary field supplies 32 support in the US game
and 16 in Japan. An upgraded field supplies 48 versus 24. Bridges also count:
32 in the US, 16 in Japan. Other support-producing structure categories differ
as well. Stopped or withered fields do not necessarily contribute support.

The game checks the existing population before admitting another house. Once
approved, a house adds all its residents, even if they take the town beyond
the allowance. Recalculating support leaves existing residents unchanged.

Attainable population also depends on geography, building placement, house
upgrades, events, and population adjustments. Published maximum-population
figures remain unverified in this investigation.

### Population milestones and the Master's level

The regions use different population requirements for later levels. The early
milestones of 80, 200, and 400 agree; later ones diverge. For example, the next
milestone is 700 in the US and 550 in Japan. The final level's requirement is
4,600 versus 3,000.

The maximum-SP progression table is the same in both versions. Earned levels
are retained when population falls below an earlier requirement.

The city report uses different rules to classify development in each release.
Its thresholds determine the report shown, rather than imposing a population
cap. The reported wording differences are covered under
[Menus and artwork](#menus-and-artwork).

Evidence: [support and level tables](regional-differences-technical.md#town-census-and-level-requirements),
[construction and population behavior](regional-differences-technical.md#population-switching-established-boundaries),
[city reports](regional-differences-technical.md#census-construction-and-status-are-separate-contracts).

### Monster lairs and sealing

Japan generally starts with more monsters remaining in each town's lairs.
The totals below sum the four initial lair reserves. Only a portion of those
monsters is active at once, and the reserves can be reduced through combat,
miracles, and other town events.

| Town | US starting total | Japanese starting total |
| --- | ---: | ---: |
| Fillmore | 500 | 600 |
| Bloodpool | 330 | 400 |
| Kasandora | 450 | 900 |
| Aitos | 320 | 600 |
| Marahna | 210 | 600 |
| Northwall | 180 | 500 |

The lairs' positions and monster types match in the compared starting tables.
Spawning delays differ for every lair. Their countdowns advance only when
the town and lair are eligible to spawn a monster.

Defeating a monster subtracts one from its lair's reserve; qualifying miracle
effects can subtract up to ten. Destroyed houses and action-stage score
processing can change the count too.
An exhausted but unsealed lair can consequently gain monsters again.

The people can seal a lair while it still has monsters remaining. The tested
guidance path requires a population of at least ten. Sealing credits the
remaining count toward town growth, retains the stored number, and marks the
lair as sealed. Monsters already flying can remain alive after their lair is
exhausted or sealed.

In both versions, the tested Blue Dragon's returning soul awards one growth
point even if its lair is empty or sealed. Saving or leaving town preserves
that pending reward.

### Action scores and town development

Both versions convert action-stage scores into a value used by town development:

- The US conversion produces two points for each complete ten points of score.
- Japan ignores the first 650 points, then produces ten points for each
  complete block of 32 above that amount.

After the first act, the lair adjustment goes in opposite directions. At a
score of 1,000, the tested US path adds 50 to each lair; Japan subtracts 25
from each, stopping at zero. Once the town has completed its second act, the
score-derived value is directed to growth instead.

Japan processes the score during the stage-clear sequence; the US game does
so when leaving the action stage.

Evidence: [lair counts and delays](regional-differences-technical.md#monster-lairs),
[score and house effects](regional-differences-technical.md#lair-stock-is-not-monotonic),
[sealing and returning souls](regional-differences-technical.md#guidance-sealing-and-delayed-soul-rewards).

### Simulation enemy combat and behavior

Blue Dragons and Red Demons take more hits in the US game, and several
species deal more contact damage. The table uses ordinary one-damage arrows:

| Monster | Arrow hits to defeat, US / Japan | Contact damage, US / Japan | SP reward, both |
| --- | ---: | ---: | ---: |
| Blue Dragon | 3 / 2 | 3 / 2 | 2 |
| Napper Bat | 1 / 1 | 1 / 1 | 1 |
| Red Demon | 4 / 3 | 6 / 3 | 4 |
| Skull Head | 8 / 8 | 8 / 4 | 12 |

Behavior changes are separate from those numbers. The Japanese Blue Dragon
checks for a target less frequently in the examined search phase. The Napper
Bat has different fallback choices and a longer wait in one state. Target
selection also searches differently between regions.

The examined Skull Head targeting and earthquake preparation sequence is
shared, including its timers. Its contact damage and the earthquake's
destruction rules differ.

Evidence: [combat and behavior](regional-differences-technical.md#sim-enemy-state-differences),
[Skull Head behavior](regional-differences-technical.md#skull-head-target-and-earthquake-state-contract).

### Offerings and the Magic Skull

The Magic Skull seals its designated Bloodpool lair through a different path
from guiding the people. It works at the tested population of two, credits
the remaining lair count toward growth, and consumes the item only on success.
It does not grant the guidance path's technology reward or SP refill. These
rules agree in both versions.

The US item sequence adds a 90-frame wait after the effect before consuming
the skull. Japan proceeds without that wait.

**Reported:** the crop offering is represented as rice in Japan and wheat in
the Western release, with a difference in how soon it can be taken again.
Whether that timing comes from a separate restocking rule or the development
schedule remains unresolved.

## Sky Palace, story, graphics, and music

### Death Heim's appearance

After all six towns have completed both acts, the US action-completion code
prepares a separate world-map scene and music selection. Japan returns to the
current town; its later Sky Palace announcement records Death Heim's appearance.

This code supports reports of a US emergence sequence and a Japanese
announcement followed by Death Heim already being present. The complete
visual and musical sequence still needs an end-to-end check.

Evidence: [arrival and music selection](regional-differences-technical.md#death-heim-transition-and-music).

### Music

Of the 17 compared song data sets, 15 match after accounting for their different
locations in the ROM. Two have changed sequence data, and several scenes
select music differently.

**Reported:** Fillmore Act 2 uses the Fillmore theme in Japan and the
Pyramid/Marahna theme in the Western release. This scene-to-song identification
still needs a playback check.

### Story and wording

Reported script differences include:

- The Japanese premise concerns divine power sealed away by Satan and restored
  through the people's worship. The Western story instead describes the
  Master's defeat by Tanzra and his lieutenants, followed by a long sleep.
- References to God and Satan become the Master and Tanzra in the Western text.
- Kasandora's lost man's death and burial are more explicit in Japan.
- The Japanese Bloodpool ending explains that Teddy arranged to be chosen
  for the sacrifice, rather than simply being selected by chance.

### Menus and artwork

Reported presentation differences include:

- The title logo, emblem, Japanese lettering, publisher arrangement, and
  copyright text vary between releases.
- The Western Master-status screen adds a per-act and total-score page.
- City status uses “MAX” in the Western release where Japan uses a
  slower-growth description. The complete display conditions still need review.
- Several action enemies have different faces, clothing, weapons, shading,
  or animations. Marahna's first boss has a particularly substantial redesign.
- Simulation artwork changes include follower emotion/death symbols, the
  skull lair's star-versus-diamond design, and the eye on Kasandora's pyramid.
- Death Heim's first boss statue has horns in Japan that are absent in the
  Western version.

The resource comparison confirms graphics-related differences that still
need to be matched to named objects and scenes. Changes can involve image
data, sprite assembly, or positioning; Marahna's first boss includes the
latter two as well as its reported visual redesign.

## European releases

The European release is **reported** to expose the action-only game from the
start as **Action Mode**, alongside **Story Mode**, and to add difficulty
selection. Title and copyright details also differ.

The English European, French, and German ROMs still need separate gameplay
comparisons. Their difficulty options, timing, and other mechanics are not
covered by the US/Japanese findings above.

## About the research

Last revised: September 20, 2026. Established findings come from comparing
the retail US and Japanese ROM data and code, supplemented by controlled
tests running the original routines and game scenes. Some tests deliberately
set up a particular situation; they are not substitutes for complete playthroughs.

The [technical companion](regional-differences-technical.md) preserves the
exact ROM identities, addresses, measurements, and limitations so readers
can inspect the evidence. Reported leads were paraphrased from the regional
differences section of The Cutting Room Floor's *ActRaiser* article, supplied
as text during the research. They are kept distinct from independently
verified findings.
