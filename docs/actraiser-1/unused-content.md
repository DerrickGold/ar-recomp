# ActRaiser: unused content and oddities

This article collects hidden debugging facilities, developer text,
unexplained artwork and code paths in the retail releases. It distinguishes
working helpers from possible leftovers and identifies where evidence is
still incomplete. The five releases examined are Japanese, US English,
European English, German and French.

Features removed or changed between releases belong in the
[regional differences article](regional-differences.md), even when a ROM
retains their disabled code or artwork. Regional removals and regional-only
credits are not repeated here.

An unexplained animation or empty routine may be a leftover, a placeholder
or something used through a path not yet identified. None of these entries
has been traced to an earlier build or confirmed as a removed feature.

## At a glance

| Entry | Release coverage | Finding |
| --- | --- | --- |
| Music Mode sound test | All five | Counters, music and effects work when entered through a test adapter |
| Action debug controls | US and Japan; corresponding block absent in Europe | Room/area stepping and coordinates tested; supplied activation patch has side effects |
| Development inscription | All five | Japanese asset label retained in the Palace/temple graphics, but omitted from their authored backgrounds |
| Dog and fertilizer | Artwork in all five; named in Japan | Item entries with pictures but no Use effects or identified acquisition route |
| Dragon's Egg | Named in Japan; slot retained in all five | Separate item entry, relabelled Ancient Tablet in the West, with no Use effect |
| Fillmore's extra Magic Skull event | All five | Working reward callback and translated dialogue, but no identified normal trigger |
| Unexplained offering announcements | All five | Fillmore jewel and Aitos magic messages with no identified normal trigger |
| Extra log animation | US and all European releases | A gentle bob with no identified gameplay use |
| Four cave pictures | All five | Sprite definitions with no identified owner |
| Questionable Bloodpool animations | Japan; blank Western counterparts | Invalid picture references with no established normal caller |
| Blank simulation pictures | All five | One picture without animation references; another used by four unselected animation programs |
| Extra Palace music declaration | Western releases | An unexplained reference to silence that is used elsewhere during the ending |
| World-map placement record | US and European Story tables | Invalid action-object entries bypassed by normal scene loading |
| Town-support helpers | All five | Add/subtract routines with no identified caller |
| Lair-spawn delay helper | All five | Shortens spawn waits, but has no identified gameplay caller |
| Empty lair helper | All five | Two lair-processing paths still call a routine that does nothing |
| Northwall scroll condition | All five | An internal rejection branch that its own constant condition never selects |
| Shared fishing progress | All five | Northwall's lake search inherits the counter used by Marahna's fishing expedition |
| Aitos scene flag mismatch | All five | Repeated scene setup interrupts the dying man's pose animation; both story outcomes still work in the tested sequences |

## Debugging facilities

### Music Mode

All five ROMs retain a sound-test menu labelled “Music Mode,” with separate
music and effect counters. Controlled tests confirm these controls:

| Control | Action |
| --- | --- |
| Left / Right | Decrease / increase music selection |
| Up / Down | Decrease / increase effect selection |
| B | Play selected music |
| Y | Play selected effect |
| Select | Close the menu |

Both counters start at 1 and stop at their limits rather than wrapping.
Holding a direction advances its counter once; release it before pressing again.
Japan's music selector ends at 20; the US, European English, German and
French selectors end at 22. All five effect selectors end at 38. These
are selectable IDs, not counts of distinct compositions.

Western music selection 21 loads the same silence used during the ending.
It is a selectable resource, not an automatic stop-on-exit command. The
menu's close path erases its text and restores the display; it does not
request this resource. Japan retains the silent entry in its music table,
but its debug selector cannot reach it.

The tests entered each menu through an in-memory adapter at an action-frame
boundary. Music and effects played, the action timer stayed paused, and
closing the menu allowed movement and the timer to resume. These tests
establish that the retained menus function, not that an ordinary player
can open them in an unmodified release.

Evidence: [sound-test and playback addresses](rom-map.md#debugging-and-unassigned-routines).

### Action debug controls and a bypassed block

The US and Japanese ROMs retain debug controls that wait while R is held,
open Music Mode with R+Start and leave the action scene with X+A. Another
section can step through rooms and areas or display camera and actor coordinates,
but the preceding code bypasses it.

Controlled US and Japanese tests successfully stepped from Fillmore's first
room to its second, and from Fillmore to Bloodpool. The coordinate display
also showed the camera and current actor's positions in hexadecimal. These
are limited checks, not a validation of every room or a normal campaign route.

The supplied debug-enable patch is not a reliable way to use these tools.
It inserts a call into a multiplication helper, where the menu can disturb
an unfinished calculation and run without normal screen updates. In the
tested US and Japanese scenes, the menu accepted commands but returning
left gameplay unresponsive. A separate frame-boundary adapter avoided
those failures. Neither adapter is evidence of the developers' original
activation method.

The corresponding block of controls and supporting tables is absent from all
three European releases: their main loop is followed directly by the next
scene-transition routine. Music Mode itself remains present. No alternative
European activation path has been established.

Evidence: [controller ranges and patch sites](rom-map.md#debugging-and-unassigned-routines).

## A developer's graphics label

All five releases retain a Japanese inscription among the graphics shared
by the Sky Palace and temple:

> 基本パーツ中世<br>
> (まち)

It reads approximately “Basic parts — medieval (town).” The lettering is
drawn into the tiles themselves, rather than stored as dialogue, and its
pixels are identical across all five releases.

The full graphics bank is loaded for both scenes, but neither scene's
authored background definitions selects the inscription's tiles. It appears
to be a graphics-development label left alongside the artwork, rather than
an in-game message. The wording alone does not establish a discarded town
or a different historical setting.

Evidence: [graphics source, tile range and background-reference checks](rom-map.md#developer-inscription-in-the-palace-graphics).

## Item remnants

### Bag and dog item graphics

The inventory artwork includes a dog and a small bag. Japan's item names
identify the bag's contents as fertilizer:

| Picture | Japanese name | Approximate English meaning |
| --- | --- | --- |
| Dog | りっぱなイヌ | Fine dog |
| Bag | ひりょう | Fertilizer |

Both pictures survive unchanged in all five ROMs, with color and grey
menu variants. Japan connects them to their own named inventory entries.
The Western versions instead give those entries the Bomb name and icon,
although they remain separate from the working Bomb item.

Their Use routines do nothing in any release: neither item is consumed and
no effect is applied. Neither appears in the examined offering grants or
lair rewards, and no normal way of obtaining them has been identified.

The fertilizer name identifies the bag's intended contents, but its intended
effect is unknown; a crop-improvement mechanic has not been established.

The dog item is separate from the dogs that walk through towns, which have
a [working scenery animation path](sim-object-catalog.md#spawn-listspecial-composition-groups).

Evidence: [item-icon pointers, handlers and grant-search limits](sim-object-catalog.md#dog-and-fertilizer-item-remnants).

### Dragon's Egg

Japan's item list also contains `りゅうのたまご`, or “Dragon's Egg.” It has
its own inventory slot and icon assignment, separate from the Ancient
Tablet. The Western releases keep the slot but give it the Ancient Tablet's
name and icon. The two entries are not interchangeable: only the original
tablet has a working Use routine.

The Dragon's Egg slot does nothing when used in any of the five releases.
Like the dog and fertilizer, it is absent from the examined offering grants
and lair rewards. No normal acquisition route has been identified.

The icon assigned to the egg in Japan has a counterpart in the Western
graphics, but the artwork differs. The surviving name and menu data do not
explain what the item was intended to do.

Evidence: [item identity, icon data and empty Use routine](sim-object-catalog.md#dragons-egg-item-remnant).

### Fillmore's extra Magic Skull event

Bloodpool supplies the familiar Magic Skull after Teddy returns. All five
releases also retain a separate event assigned to Fillmore: the followers
report finding a skull-shaped statue and offer it to the Master. The message
is translated in the Japanese, German and French versions as well.

The reward code works when its eligibility flag is enabled in a controlled
test. It places one Magic Skull among Fillmore's offerings and schedules a
follow-up that displays the discovery message. Later event passes do not
grant another Skull.

No normal trigger has been identified. Fillmore's population and road event
tables do not select it, nor do the town's periodic checks enable it. The
retained code and dialogue suggest a possible leftover event, but do not
establish that it was ever available in an earlier version or exclude an
unidentified activation path. It is not a second kind of Skull item.

Evidence: [callback ownership, eligibility tests and search limits](regional-differences-technical.md#fillmores-additional-magic-skull-event).

### Other offering announcements

Fillmore has a message about finding a strange jewel, and Aitos has one
announcing the discovery of the Master's magic. Both survive in translation
across all five releases, but no normal trigger has been identified in the
checked event tables and message-setting paths.

Unlike Fillmore's extra Skull event, these messages have no reward code in
their assigned callbacks. The Aitos message also has no matching spell grant
in the examined offering routines. They may be leftovers, but the text alone
does not establish a removed quest, the jewel's identity or a missing spell.

Evidence: [message ownership and limits](regional-differences-technical.md#dialogue-only-slots-and-indirect-message-owners).

## Unexplained artwork and animations

The three action-stage candidates below were also checked during bounded
original-game traces in all five releases. The tests followed the game's
actual picture selections, including animation offsets, and verified which
graphics were loaded. None selected the questioned resources. These were
not complete stage playthroughs, so their absence does not prove that no
gameplay path uses them.

Evidence: [instruction-level selection checks](regional-differences-technical.md#instruction-level-resource-selection-checks).

### Bloodpool's extra log animation

The Western versions store an extra animation for Bloodpool Act 1's logs.
It uses the existing log picture and describes a gentle bob: two pixels
down, then two pixels back up, with pauses between the movements.

The placed logs use other animations. Standing on them does not select this
extra sequence in the checked code, nor does reaching the end of their
ordinary animations.

No normal use has been established in any checked release. That unanswered
ownership question, rather than its absence from Japan, makes it a candidate
for this catalogue.

Evidence: [log controllers and animation resources](regional-differences-technical.md#action-resources-without-a-proven-gameplay-owner).

### Four unexplained cave pictures

Fillmore Act 2 contains four sprite definitions that none of its listed
animations uses. Each describes how to assemble a picture from smaller
tiles, and all four definitions are identical across the five releases.

The checked action-stage and Master sprite collections contain no exact
duplicate of these definitions. Their artwork draws on shared character
graphics, with one picture also using tiles from the cave's own graphics.

The game can select pictures indirectly, so an absent animation reference
does not settle whether they are unused. Their intended owner and appearance
remain uncertain; an isolated render cannot reliably identify them as a
particular enemy or item.

Evidence: [composition survey and shared graphics](regional-differences-technical.md#action-resources-without-a-proven-gameplay-owner).

### Bloodpool's questionable animation entries

Two animations in Japan's Bloodpool Act 2 refer to a picture beyond the end
of the stage's sprite table. Following the reference reaches unrelated data,
not a valid hidden picture.

The Western counterparts use a valid blank picture instead. Each version
stores the same short sequence twice, which is consistent with placeholders,
although their original purpose is unknown. They belong to the ordinary
stage sprites, not the Wizard's separate boss artwork.

No ordinary actor has yet been shown to select the questionable entries in
normal play. They should not be described as a reproduced gameplay bug.

Evidence: [ordinary-slot references and native resolution](regional-differences-technical.md#action-resources-without-a-proven-gameplay-owner).

### Two blank simulation sprite entries

All five releases contain two single-tile pictures that are transparent in
the graphics loaded by every town. One has no references in the listed
animation programs. The other is used by four stationary programs, but none
of the checked behavior-selection calls chooses those programs.

These are possible placeholders rather than recovered pictures of an unknown
creature. Their intended purpose remains unexplained.

Evidence: [ordinary simulation visual identities](sim-object-catalog.md#ordinary-world-visual-identities).

## Skipped paths and code oddities

### Aitos's dying-man scene flag

The Aitos event about a wounded man asking for rain contains a flag mismatch
in all five releases. It checks whether its scene has been prepared, but
records that preparation in a different, persistent event flag. Its own
initialization flag therefore stays unset.

The man has a small, two-pose animation. Recreating the scene keeps returning
him to its starting pose while the simulation runs. In a controlled comparison,
marking the already-created scene as initialized lets the alternate pose
appear. The change is subtle, involving only a few pixels of his sprite.

The story itself still works in the checked sequences. Waiting reaches the
message about his death without rain; sending rain earlier fulfills his last
wish and produces the separate response thanking the Master. Both paths
finish and remove the actor in all five releases, with or without the
comparison flag change.

The evidence supports an original animation bug, not a skipped event or a
blocked rain request. These tests use controlled town and lair state, rather
than full campaign playthroughs, and do not cover every possible casting
time. The issue is separate from the Aitos mountain-event dialogue bug.

Evidence: [flag addresses, actor program and live comparisons](regional-differences-technical.md#aitos-dying-man-scene-flag-mismatch).

### Two towns sharing fishing progress

Marahna's deep-sea expedition and Northwall's lake search use the same
progress counter. Marahna resets it when starting an expedition and finishes
at 128. Northwall keeps the value already there and finishes at 255.

In controlled tests, completing Marahna's expedition left the counter at
128. That value survived a visit to the Sky Palace and loading Northwall,
where the lake search needed only 127 more updates to find its magic. A
comparison starting from zero needed 255 updates for the same reward.
The behavior matches in all five releases.

These are event updates, not seconds: dialogue and regional simulation
timing affect the actual wait. The tests used an explicitly enabled Northwall
event, rather than playing a complete campaign between the two discoveries.
They establish the shared progress and its effect, but not why the developers
used one counter for both towns.

Evidence: [counter ownership, original-scene tests and limits](regional-differences-technical.md#northwalls-lake-search-shares-marahnas-counter).

### A Northwall condition that always passes

The event awarding a scroll near Northwall's great tree contains a rejection
path, but its own opening check can never choose it: the code tests a fixed
nonzero number rather than changing game state. The normal town-event
selector still controls when the event becomes eligible, so this does not
make the scroll freely available from the start.

All five releases retain this construction. It may be a leftover check or
an intentional bypass; no earlier working version or intended condition
has been established.

Evidence: [callback, inactive rejection arm and selector controls](regional-differences-technical.md#northwall-scroll-callbacks-constant-condition).

### Silent music with an unexplained Palace declaration

All five releases include a silent music resource, deliberately selected
during the transition into the credits. Western Music Mode also offers it
as selection 21.

The oddity is a separate reference to it in the Western Sky Palace scene
data. No normal route has been found that activates this particular entry.
The ending loads the silence directly, without using the Palace reference.

The closest known lead is a scene-selection value set when Death Heim
appears. A trace confirms that the game clears this value before loading
the Palace, which receives its normal music selection instead.

No earlier audible song has been recovered from this resource.

Evidence: [silent upload and scene-selection checks](regional-differences-technical.md#extra-palace-music-resource-silent-upload).

### A bypassed world-map placement record

The US action-placement list and all three European Story lists include an
entry for the world-map scene. It contains two otherwise empty records with
object types outside the valid range. Normal world-map loading bypasses the
entry. Japan's list and the European Action Mode lists omit it.

The records do not describe valid action enemies, and their intended purpose
is unknown. Alternative entry paths, including debug scene changes, have
not been exhaustively tested.

Evidence: [world-map placement and routing](regional-differences-technical.md#unused-world-map-placement-root).

### Two town-support adjustment helpers

All five releases retain two small routines that add or subtract
16 from a town's support value. Support is used when deciding whether more
houses can be built; it is not the number of people living in the town or
a strict population ceiling. These routines do not add or remove residents.

Every release uses the same adjustment, despite regional differences in
field and bridge support. No direct caller was found in the checked code
and reference patterns. Whether the routines are reached indirectly, and
what they were intended to serve, remains unknown.

Evidence: [support routines and reference-search limits](regional-differences-technical.md#population-switching-established-boundaries).

### A routine that shortens lair-spawn delays

All five releases retain a routine that reduces the waits between monster
spawns from the current town's four lairs. It divides each delay by four,
rounds down and adds one. It does not itself spawn monsters or change the
number remaining in a lair.

No ordinary gameplay caller has been established. The operation could fit
a difficulty adjustment, an event or a debugging aid, but none of those
purposes is confirmed.

### A called helper that does nothing

Another helper in the lair code consists of an immediate return in all five
ROMs. Each release still calls it from two lair-processing paths. It differs
from the delay helper above: the calls exist, but there is no work to perform.

It may be an unfinished stub or an intentionally empty hook. Without an
earlier implementation, its original purpose cannot be determined.

Evidence for both helpers: [lair routines and caller limits](rom-map.md#debugging-and-unassigned-routines).

## Scope

Last revised: September 21, 2026. These entries come from five retail ROMs,
resource comparisons, bounded disassembly, earlier tile-dump review and
controlled execution of original routines. Reported material and earlier
visual review are identified separately from reproducible local checks.
Deliberately selecting an animation or audio resource tests what it does;
it does not prove that a player can encounter it normally. This is a
catalogue of the investigated candidates, not an exhaustive inventory of
unused bytes or pre-release content.
