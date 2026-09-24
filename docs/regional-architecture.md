# Regional rules architecture

For player-facing options, see [Regional settings](regional-settings.md).
ROM findings belong in the [technical reference](regional-differences-technical.md);
the [HLE binding index](research-symbol-map.md#regional-hle-bindings) maps current
native entries to their C owners. This note covers implementation boundaries
that are not obvious from filenames.

## Ownership

- [Overlay](../src/settings_overlay/regional): row definitions bind literal label
  and help keys to narrow setting IDs or explicit preset actions. Translations live in the
  [UI catalog](../installer/internal/interfacecatalog/messages.json).
- [Policy](../src/regional): pure rules grouped into action, towns, interaction
  and presentation. `regional_profiles.c:VisitProfileMembers` is the single
  typed group-membership inventory, shared by expansion and summaries.
- [Session](../src/regional/session): requested/effective rules, campaign
  identity, activation, persistence and replay fingerprints.
- [Game integration](../src/actraiser/regional): edit authority, lifecycle,
  population conversion and donor uploads. Shared native adapters remain in
  `src/actraiser` with their native owners.
- [Media](../src/regional/media): validated donor formats and drawing residency.
  [The installer](../installer/internal/regionalmedia) owns extraction.

The same feature stem is used for policy, native adapter and test filenames.
Native entry addresses are declared in `recomp/bank*.cfg`; adapter headers
state their guards and return/state contracts. A donor ROM address is data,
never an alias for a US callable entry.

## Editing and activation

The overlay receives a copied `ActRaiserRegionalRulesView`, not a session
pointer or CPU state. The runtime supplies fresh campaign/replay authority to
the editor on every request. Cached summaries and the view's `editable` flag
are display data, not permission to edit.

The overlay's mid-game warning uses `ActRaiserRegional_PreviewProfile` for
bulk presets and `ActRaiserRegional_PreviewRules` for narrow choices.
Preview and apply share the editor's read-only preparation, including actual
support values and history availability. Preview publishes no session, intent,
WRAM or save changes. Confirmation revalidates the original campaign/revision;
the overlay-local warning cannot consume a host-owned Continue/Palace decision.
Town-impact labels are scope hints; only the game preview decides whether a
particular switch requires redevelopment. These UI additions change no save IDs.

`actraiser_regional_choices.c` projects individual edit families into copied
source/active/pending summaries. The menu inventory binds literal catalog keys
to these typed families; it never writes policy fields. Narrow previews run
the same validated editor operation on local session/intent copies. Preview
and apply therefore share compatibility, history and replay checks.
Starting health/lives and SP/angel recovery use independent policy leaves.
Population support alone retains its compatible level and story goals;
reporting and the failed-Act-2 Compass prerequisite are separate edits.

Difficulty has a semantic profile of its own. The player-facing choice enables
all five modifiers atomically (or disables them for Original); changing terrain,
placements or base combat leaves it intact. Full gameplay presets deliberately
choose the region's default difficulty. Existing policy records, replay fields
and save versions are unchanged. EU placement filtering also consumes the level,
so a level change marks that row pending when EU placements are selected.

Preset browsing is overlay-local state. Confirm shows the complete replacement
scope before applying or queuing a preset. The local selection is cleared when
the overlay opens again; no unapplied choice is saved with the campaign.

A request does not immediately replace every active rule. Session APIs commit
each family at its owning boundary. Action-room activation is atomic and shares
private primitives with individual family activations. Casts, run inventory,
song uploads and actor-art uploads retain their separate lifetimes.

Population support changes require Palace confirmation. Pending intent binds
to campaign identity and requested values; confirmation uses a fresh,
revision-checked preview. The population-conversion adapter owns the recovery
checkpoint and native-town transaction. Merely editing a setting cannot
rebuild a town or write a save.

## Compatibility and presentation

Menu order and enum ordinals are not save IDs. The session codec binds stable
wire keys to typed fields for both encoding and decoding; see
[Save format](save-format.md). Regional-session tests cover historical versions
and fixed version-69 payload fingerprints.

Missing donor media affects availability, not rule provenance: a Japanese
selection remains Japanese with a partial indicator and native media fallback.
Drawing consumes validated residency metadata; it does not parse packages,
hash images or activate settings per actor. Native animation and collision
remain separate from donor drawing data. See [Media contracts](regional-media.md),
[ROM hooks](rom-map.md#regional-presentation-hooks) and
[RAM ownership](ram-map.md#regional-actor-presentation-ownership).

[Native presentation ownership](native-presentation-ownership.md) traces HUD,
world-navigation and Death Heim sprites from their native producers through OAM
upload, and describes palette-independent terrain classification.

European difficulty is independent of the selected region; runtime pacing
remains 60 Hz. Language, fonts, physical bindings, game fixes and randomizer
settings are outside regional profiles.

The randomizer composes with regional data through numerical adapters. Selected
placement programs are randomized before publishing a room snapshot. At actor
birth, `Randomizer_SpawnStatBasis` recovers pristine initializer values and the
last applied HP/damage percentages for records changed by the ROM pass. The
game adapter selects regional stats from that basis, scales once, and resumes
the existing mode/difficulty adjustment. This avoids both double scaling and
attempting to invert rounded or clamped values. The portable regional policy
still knows nothing about ROM offsets, settings or the randomizer.

The record-ownership bitset is built during randomizer application; birth-time
checks are constant-size, with no allocation or ROM scan. Explicit Tanzra child
HP/damage initializers use the same applied scale, but rewards do not. Replay
identity includes a versioned stat-scale contribution whenever either percentage
differs from 100%; the identity case preserves existing replay fingerprints.

`RandomizerConfig` is a value-only, versioned campaign recipe. Title settings
are a draft; the confirmed title return captures the recipe and performs
regional rolls once. `ArRegionalProfiles_SelectSources` shares the profile
inventory's typed fields and descriptor keys with the pure regional selector,
so there is no second menu/ordinal-to-rule map. Each leaf's roll is keyed by
seed and descriptor key, with population compatibility enforced before the
new session is published. Difficulty and interaction/presentation are excluded.

`ArRegionalSession` carries the recipe through the existing exact-image save
journal, pending story snapshots, imports and recovery copies. Continue never
regenerates rules. The randomizer binds the loaded recipe before gameplay and
uses it for both ROM passes and numerical placement programs; mutable global
settings cannot redirect the active run. Returning to title restores the draft.
No seed is written into SRAM. Recipes missing from old saves are not guessed.
Enabled recipes contribute their canonical bytes under `ARRANDSTATE-R1` to
replay identity, including seeds whose current room happens to look identical.

These seed operations reuse existing seams: `ActRaiser_RegionalTitle` at
US `$02:A622`, the guarded `ActRaiser_RegionalContinue` at `$02:A79F`, and
`ActRaiser_SaveStory` at `$03:A656`. They introduce no ROM entry or native
memory allocation. Initialization/slot attachment releases any prior bound
recipe; accepted New Game captures the current title recipe, while Continue
loads the selected slot's exact-image checkpoint. A pending completed save
owns a copied session, so a later unsaved campaign cannot replace its recipe.
Host persistence and transient/native projections are distinguished in the
[RAM ownership map](ram-map.md#regional-host-state-and-native-projections).
