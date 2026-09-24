# Regional media packages

Regional graphics and music sequences are extracted locally from a supported, unmodified ROM.
They are not included in the repository or release downloads. Gameplay rules
that use numerical regional tables do not require these packages.

In the ActRaiser Builder, open **Assets → Regional media** and select a
supported ROM. **Extract & install** identifies the release, extracts only
the reviewed media, and installs it in the correct game data folder. You can
also select a private `.armedia` file extracted previously. The list reports
which donors are installed or need re-extraction. Identical files are left
alone; replacing a different existing donor requires confirmation. The
builder never replaces the US build ROM or selects gameplay rules for you.

The command-line alternative is:

```sh
actraiser-builder regional-media --rom ar-jp.sfc --out jp.armedia
```

The output must be a new file; extraction never overwrites an existing file.
It accepts the same five exact, headerless releases as language extraction.
The package contains ROM-derived graphics and music data and must remain private. It is not
a language pack and should not be shared as an `.arlang` publication.

Supported resources include Death Heim's BG2 character sheet, the Japanese title background, town
symbols and decoration, the European Action Mode health-growth pickup, and
its smaller spell-HUD icons, and two Japanese music sequences.
European English, German and French contain identical copies of those icons.
The US package contains the baseline Death Heim resource; the Japanese package
also contains three small town-art resources, a matched title-background bundle,
and the two sequences.

Japanese extraction also includes the action actor drawing resource described
below, included in the **Regional artwork** group.

Open **Settings → Regions → Artwork & music** for **Regional artwork** and
**Regional music**. **Presets → Presentation** sets both together. The sections below
describe their individual resources, not extra settings rows. Missing resources
show **partial**; they do not change a requested Japanese preset to Custom.

## Install Japanese enemy artwork

After extracting the Japanese donor, select **Regional artwork → Japan**
in Regional rules. US and Europe use Western artwork. A change takes effect
at the next stage entry, not between rooms: some boss rooms reload only part
of the sprite set and must keep the stage's matching graphics and palette.
Missing donor data leaves US artwork visible and is reported in the help.

This changes drawing parts, graphics and palettes, not enemy health, attacks,
animation timing or collision. The plant boss keeps the geometry selected by
the gameplay rules. Native sprites and enhanced/widescreen sprite rendering
share the same drawing data. Re-extract older development packages if the
builder reports missing resources.

## Install Death Heim artwork

Create `game-assets/regions` in the game's data folder, then extract the
Japanese ROM directly to `game-assets/regions/jp.armedia`. Restart the game
and select **Regional artwork → Japan** in the menu above.
The Japanese choice restores the first statue's horns at the next room entry
or retry. US and European choices use the original US artwork. Boss behavior
and the island's arrival sequence are separate settings.

The game reads packages at startup. Missing, modified or incompatible files
leave the US graphics in place; the settings description reports a missing
Japanese donor while retaining your requested choice. Removing a package
does not remove a saved preference. No ROM is needed again after successful
extraction. The builder's **Refresh installed files** button also detects
packages you installed manually, without repeatedly scanning during gameplay.

Reserved package filenames identify exact donor releases: `us.armedia`,
`jp.armedia`, `eu-en.armedia`, `de.armedia`, and `fr.armedia`. A package with
the wrong release for its filename is rejected before it can occupy that
donor's slot. Renaming a US file to `jp.armedia` cannot supply Japanese art.

## Install European Action Mode item graphics

Extract a European English, German or French ROM into the corresponding
filename above, then restart. Select **Regional artwork → Europe** in the
menu above. When using the European Action inventory, this supplies the
smaller spell icons and health-growth pickup. It does not change pickup
effects, switch the inventory model, or alter Story-mode item graphics.
The choice takes effect on room entry or retry.

All three European releases supply identical item graphics, regardless of
your language pack. Without a valid donor, spells use the retained US icons
and health growth uses a full apple. The requested setting is retained and
the menu reports the fallback.

## Install Japanese town artwork

After extracting the Japanese donor, **Regional artwork** covers follower
symbols, Skull Head lair artwork and pyramid decoration together.
Japan selects the Japanese images; US and Europe select the
baseline images. Changes take effect the next time a town is entered. Missing
donors leave the US images in place and are reported in the setting's help.

Japan uses a skull for anger and a yellow cross for death. Its early Skull
Head lairs use a six-pointed star instead of a diamond. The later graphics
bank is identical across regions and is not replaced. The Japanese pyramid
has the eye decoration in both banks. These choices change character pixels,
not follower behavior, lair reserves, town progress, palettes or events.

The replacements feed native VRAM, so the enhanced rendering paths that use
the original town canvas and sprites see the same images. Custom 3D models
are separate: this does not add an eye to the enhanced pyramid model.
Older development-era Japanese `.armedia` files must be re-extracted when
the builder reports that their reviewed resource set is incomplete.

## Install Japanese title artwork

Select **Regional artwork → Japan** after installing the Japanese donor.
The original emblem and Japanese logo lettering are applied together with
their matching map and colors the next time the title screen loads. US and
Europe use the Western artwork. Without the donor, the setting is retained
but the US background remains visible.

The title's spin, fades and color animation still use the native renderer.
Menu translations, copyright text and gameplay modes are separate; choosing
this artwork does not select a Japanese language pack or unlock a game mode.
Like other regional rules, this preference belongs to the campaign. A fresh
launch starts with the US new-game defaults; returning to the title from a
campaign carries its requested choices. Continue still restores that save's
rules rather than adopting edits from the new-game title draft.

## Install Japanese music sequences

After installing the Japanese donor, select **Regional music → Japan**
in the customization menu above. This changes two songs: the theme used in the
Western Fillmore caves, Kasandora Act 2 and Marahna Act 1, and the Northwall
theme. US and Europe use the Western sequences. The same group selects
Fillmore Act 2's scene-to-track assignment. Internally these remain independent
members; choosing one group does not couple either of them to gameplay rules.

Changes apply when an affected song is next uploaded. They do not interrupt
the current song or force a reload between rooms that share it. Missing donor
data leaves the US sequence playing and is reported in the settings help.
External music replacements still take priority.

Only the note-sequence data is replaced. Playback uses the US sound driver,
samples and clock, so this is not a claim of bit-identical Japanese audio.
No audio data is written into the ROM or native save file.

## Data contract

`.armedia` is a bounded binary resource container, not an executable patch
format. All integers are little-endian. Its version 1 header is 48 bytes:

| Offset | Size | Meaning |
| --- | ---: | --- |
| 0 | 8 | `ARMEDIA` followed by a zero byte |
| 8 | 2 | Format version, currently 1 |
| 10 | 1 | Exact release: 1 US, 2 Japan, 3 European English, 4 German, 5 French |
| 11 | 1 | Number of resource records, at most 16 |
| 12 | 4 | Complete file size, at most 2 MiB |
| 16 | 32 | Source ROM SHA-256 |

Each directory record immediately after the header contains a four-byte
resource ID, four-byte absolute file offset and four-byte byte count.
Records have ascending IDs. Resource bytes follow the directory contiguously;
gaps, overlaps, trailing data and unknown records are rejected.

| ID | Resource | Bytes |
| --- | --- | ---: |
| 1 | Death Heim BG2, decoded SNES 4bpp characters | 8192 |
| 2 | European Action Mode health-growth pickup, SNES 4bpp characters | 128 |
| 3 | European Action Mode HUD source windows, SNES 4bpp characters | 768 |
| 4 | Japanese follower anger/death symbols, eight 8×8 4bpp characters | 256 |
| 5 | Japanese early Skull Head lair symbol, four 8×8 4bpp characters | 128 |
| 6 | Japanese pyramid eye detail, one 8×8 4bpp character | 32 |
| 7 | Japanese Mode 7 title: characters, map, palette | 33024 |
| 8 | Japanese song-table entry 9, sequence at ARAM `$1200` | 2197 |
| 9 | Japanese song-table entry 12, sequence at ARAM `$1200` | 1325 |
| 10 | Japanese action actor drawing resources | 480388 |

The HUD resource retains the overlapping 256-byte native room-entry windows
at 128-byte strides, including the empty fifth window. Dirty HUD updates use
only the appropriate 128 bytes. These are source graphics, not instructions
to copy 768 bytes into VRAM at once. The health-growth graphic comes from
Action item 1 at `$06:A880`, not Story item 1 at `$06:A080`.

The title resource is one indivisible bundle: 16384 chunky character bytes,
16384 row-major map bytes (128×128 cells, without the source's dimension
header), and 256 palette bytes (128 little-endian BGR555 colors). It contains
no font, copyright script or animation program. The music resources contain
only the changed sequence blocks, not SPC firmware, instruments or upload
scripts. Song-table entries are zero-based; Music Mode calls them 10 and 13.

Both the builder and game validate the complete required resource set,
sizes, donor identity and per-resource SHA-256 against reviewed extraction
facts. A modified or incomplete package fails validation instead of supplying
partially compatible media. There are no paths, host pointers, collision boxes
or actor programs in the container. Musical note programs retain their native
ARAM-relative sequence references.

### Actor drawing resource

Resource 10 starts with `ARACTOR1` and a little-endian 32-bit record count.
Its 115 records are sorted by scene, kind and slot. Each 16-byte record
contains a 16-bit scene (`room << 8 | area`), an 8-bit kind and slot,
32-bit payload offset and size, and four reserved zero bytes. Offsets are
relative to this resource. Payloads are contiguous, with no trailing data.

| Kind | Slot | Payload |
| --- | --- | --- |
| 1: characters | 0 or 1 | 8192 bytes for native VRAM word `$3000` or `$4000` |
| 2: palette | 0 | 128 bytes for native CGRAM colors `$80–BF` |
| 3: pictures | 0 or 1 | Drawing tables associated with animation workspace `$4000` or `$5000` |

Character slots and animation slots are different namespaces. A boss may
use the first character bank; it must not be assigned the second bank merely
because its animation data uses `$5000`. Records identify actual asset
declarations, not invented per-enemy ownership or automatic scene inheritance.

A picture table has a 16-bit count, two zero bytes and `count + 1` 32-bit
offsets relative to the table. The final offset equals the table size. Each
picture has a 16-bit part count, two zero bytes, then 12-byte parts: one size
flag (0 for 8×8, 1 for 16×16), one zero byte, four signed 16-bit drawing
offsets (normal/flipped X, normal/flipped Y), and 16-bit native attributes.
Offsets are relative to the actor's world anchor, before native draw bias.
Each flipped offset uses the corresponding opposite anchor, as the native
animation reader does; asymmetric pictures cannot reuse the unflipped anchor.
The table is limited to 256 pictures, each containing 1–128 parts.

The extractor converts the original drawing anchors into these offsets and
discards collision headers, animation programs and timing. Missing picture
ordinals are not clamped or wrapped: regional exceptions need explicit
semantic mappings. In particular, PAL's added Northwall impact geometry
belongs to the boss rules, not this artwork resource.

## Code ownership

The game-specific Go packages `gamerom` and `gameassets` own retail ROM
identification and the bounded native asset-script reader. Language,
workshop-art and regional-media tools share those owners. `regionalmedia`
owns extraction and the package contract; its catalog generates the C
validation facts without embedding graphics or music bytes. None of this belongs to
the game-agnostic `snesbuild` tool.

The portable C parser in `src/regional` borrows immutable bytes and performs
no file or renderer operations. The desktop owner in `src/host` reads and
retains validated packages at startup. Duplicate donor loads are rejected
until restart so a consumer cannot keep a dangling resource view. Parsing
and hashing are load-time work, never frame-time work.
