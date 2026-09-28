# Save persistence ownership

A successful campaign save means SRAM, feature metadata, and the enhanced
player name belong to one complete durable snapshot. No caller must remember
to flush a name companion after saving or before restarting.

| Module | Responsibility |
| --- | --- |
| `save_system.c` | Own the active snapshot, select live versus saved data, validate routing, and publish successful commits. |
| `save_checkpoint.c` | The sole campaign disk writer: journal complete candidate and previous snapshots, then replace native SRAM. |
| `save_name.c` | Validate names and read legacy `.arname` companions. It does not write files. |
| Regional feature codec/coordinator | Build and validate feature payloads; acknowledge publication after storage succeeds. |
| `save_slots.c` | Hold the collection lock, pin slot paths, manage drafts, and publish the collection index. |
| Host/UI adapters | Submit an operation and restart only after it succeeds. |

`SaveSnapshot` contains the complete 8192-byte canonical SRAM image, opaque
feature bytes, and a UTF-8 display name (empty explicitly means native name).
Feature codecs own the meaning of their bytes. Their `SaveCommitHost.build`
callback has no write authority; `validate` checks both candidate and retained
records. `committed` runs only after the entire snapshot is readable from disk.

## Selecting the correct campaign

Story saves freeze SRAM, the name, and feature state at native save completion. Explicit story
snapshots use the current campaign at a quiescent game boundary. Automatic
non-story writes and editor edits preserve the last saved campaign's features.
Imports supply their own snapshot. Metadata-only changes retain durable SRAM
and the saved name, even when live SRAM has session edits or an unsaved New Game
has accepted a different name.

Session-only edits update the auto-persist shadow without committing. A later
name or feature update must not turn those edits into a durable save.

Names accepted by native name entry remain staged until their compatibility
name matches the image being saved. An editor rename clears the old enhanced
name; other edits preserve it. Name-only updates use the same transaction as
feature updates. There is no separate editor-name retry queue.

## Publication and interruption

All active persistent operations use `CommitImage`: read and validate the saved
snapshot, build the candidate, check slot routing, commit the journal/native
pair, then notify feature and slot owners. Backups, campaign exports, and
recovery copies read the same complete snapshot. Recovery writes use the same
journal writer in an exclusively reserved directory.

The writer publishes the journal before the native file. If native replacement
fails or the process stops between replacements, a fresh load selects the
record matching **all** native SRAM bytes. It gets either the previous complete
snapshot or the new complete snapshot, including the name. A metadata-only
change with identical SRAM needs just one atomic journal replacement.

Persistent APIs return success only for a complete snapshot. Failure preserves
the old durable campaign, although a prepared candidate may remain in the
journal. Callers may retry a failed operation; they must not restart as if it
succeeded. Slot-index publication remains retryable after campaign commit.
Optional `.artown` camera bookmarks remain nonfatal and independently retryable;
they are not authoritative campaign data and are excluded from portable exports.

## Journal version 2 and legacy saves

`.archeckpoint` retains the `ARCHECK\0` header, little-endian version/count,
one or two records, and an FNV-1a-64 hash over the preceding bytes. Version 2
records contain a 32-bit feature length, 16-bit UTF-8 name length, 16-bit flags,
8192 SRAM bytes, feature bytes, then name bytes. Flag bit 0 means the record owns
its name, including an explicitly empty one. Other flags are rejected.

Version 1 journals and raw native/INI saves remain readable. Their names come
from validated legacy `.arname` companions. The first successful write upgrades
the journal and captures the previous snapshot's name before replacing SRAM.
Reading does not migrate files. Legacy companions are preserved, but a version
2 record with an authoritative name never falls back to them. Unsupported or
damaged metadata blocks replacement rather than being silently discarded.
An explicit version 2 snapshot without feature bytes remains a valid legacy
campaign in the slot picker. Its journal is still required: losing that file
does not silently downgrade the campaign or discard its enhanced name.

Native `.srm` and lossless `.ini` formats and portable `.arsave` archives are
unchanged. Raw emulator exports intentionally contain SRAM only.

## Enforcement and tests

`tools/check_save_ownership.py` rejects new direct campaign writers in feature,
UI, or adapter code. Offline fixtures use `TestRegional_Save`, which still calls
the same snapshot writer. Raw legacy migration and explicit raw exports remain
documented codec operations inside the storage layer.

`actraiser_save_snapshot` exercises automatic saves, story saves, editor edits,
imports, explicit story snapshots, feature changes, and name-only updates in
both formats. It injects routing, journal, and native-write failures and then
discards runtime state to verify a cold load. It also covers version 1 migration,
first-save retries, and notification ordering. Regional and slot tests cover
real feature payloads, backups, recovery copies, and restart flows.
