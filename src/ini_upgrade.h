#ifndef INI_UPGRADE_H
#define INI_UPGRADE_H

#include <stdbool.h>
#include <stddef.h>

/* Upgrade-safe merging of a shipped INI default into the user's live copy.
 *
 * THE BUG THIS EXISTS FOR. A user extracts a new bundle over their existing
 * install to upgrade. Extraction overwrites whatever the archive carries, and
 * the bundle used to ship the LIVE files -- config.ini, diorama-layers.ini and
 * game-assets/manifest.ini -- so the upgrade silently destroyed their display
 * settings, their authored diorama rooms, and their asset mappings. Measured, not
 * assumed: extracting a new bundle over a played install replaced all three.
 * settings.ini and saves/ survived only because nothing ships them.
 *
 * WHY A MERGE NEEDS THE FILES SEPARATED FIRST. Nothing can merge after the fact:
 * by the time any of our code runs, extraction has already replaced the user's
 * file and the old content is gone. So the bundle now ships its copies to
 * `utils/defaults/`, extraction only ever overwrites THOSE, and the live file is
 * never in the archive. That gives a merge both inputs -- the shipped default and
 * the user's file -- which is what makes any of the below possible.
 *
 * WHAT THE MERGE DOES. Two-way, conservative, and biased entirely toward not
 * losing the user's work:
 *
 *   - a key the user's file already has, in that section, is LEFT ALONE, value
 *     and inline comment intact -- even if the shipped default now differs. The
 *     user's edit always wins.
 *   - a key that is NEW in this version is appended to its section, with the
 *     shipped value, so upgrades actually deliver new settings instead of
 *     requiring a hand-edit -- but see kIniUpgrade_Records below, because
 *     "absent" only means "new" for some files.
 *   - a section that is new entirely is appended whole.
 *   - everything the user wrote -- comments, blank lines, ordering, and any
 *     section or key we do not ship -- survives byte-for-byte.
 *
 * Keys are matched SECTION-SCOPED and case-insensitively on the name only.
 * Section scoping is load-bearing: config.ini has `Fullscreen` in both
 * [Graphics] and [KeyMap] with completely different meanings, so a global
 * key match would treat them as one and drop a real setting.
 *
 * TWO KINDS OF SECTION, and the merge cannot guess which it is holding.
 *
 * config.ini's sections are NAMESPACES: a fixed set of [Graphics]/[KeyMap]/...
 * whose keys are settings. A key absent from one plausibly means the user's file
 * predates it, so appending it is right.
 *
 * diorama-layers.ini's [layers:GG:MM] and manifest.ini's [replace:*]/[music:*]
 * are RECORDS: one section per authored thing, and its keys ARE the thing. An
 * absent key there usually means the user DELETED it -- cleared a plane in the
 * layer editor, pruned an override they did not want. Appending it back:
 *
 *   - silently reverts the deletion on the next launch, which no amount of
 *     re-doing fixes (each cycle just re-appends);
 *   - re-states the section header to place the key, and both record parsers
 *     start a FRESH entry at every '[' -- so the append fabricates a stub entry
 *     with one key that fails validation and is dropped with a warning, forever;
 *   - never converges: the file grows a duplicate header per cycle.
 *
 * So a record file gets kIniUpgrade_Records, which appends only WHOLE sections
 * the user does not have at all and never a key into a section they do. A record
 * the user pruned stays pruned; a record new in this version still arrives.
 *
 * The merge is deliberately never destructive: it only ever ADDS. Nothing the
 * user has is rewritten or removed. But note that for a record file "only adds"
 * is not sufficient to be safe -- adding a key back is how a deletion is undone,
 * which is why the mode exists.
 *
 * Pure: no I/O, so it is fully testable without a bundle. Same shape as
 * DioramaLayerOrder_MergeManifest, which solved the sibling problem for the
 * layer editor's own save.
 */

/* How to treat a section: see the discussion above. Namespaces get per-key
 * appends, records only whole-section ones. */
typedef enum {
  kIniUpgrade_Namespaces = 0, /* config.ini: sections are key namespaces */
  kIniUpgrade_Records,        /* layers/manifest: sections are records */
} IniUpgradeSectionKind;

/* Merge `shipped` into `live`, writing the result to `buffer`.
 *
 * snprintf contract: returns the byte count that WOULD be written and never
 * writes past `size`, so a caller sizes its buffer by calling once with size 0.
 * `live` may be NULL or empty, in which case the result is `shipped` verbatim --
 * that is the first-run case, seeding a live file from the template.
 *
 * `out_added` (optional) receives the number of keys and sections appended, so a
 * caller can log "3 new settings added" and stay silent when there is nothing to
 * do -- an upgrade that changed nothing should not announce itself. */
size_t IniUpgrade_Merge(const char *live, const char *shipped,
                        IniUpgradeSectionKind kind,
                        char *buffer, size_t size, int *out_added);

/* True when merging would change anything. Lets a caller avoid rewriting a file
 * (and touching its mtime) on every single launch, which is the common case. */
bool IniUpgrade_NeedsMerge(const char *live, const char *shipped,
                           IniUpgradeSectionKind kind);

#endif /* INI_UPGRADE_H */
