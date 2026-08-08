# T2d — Replace the legacy-env exclusion list with a per-row `SettingDesc` flag

**Status: SPEC ONLY — not executed.** The equivalence guard rail and baseline
were built and are attached; the refactor itself was deferred because it is
high-churn (touches ~17 descriptor macros + 74 literal rows across the
258-descriptor table) and load-bearing (parse polarity of every AR_* knob),
and this was reached at the tail of a long session. It IS fully verifiable —
execute it under the attached guard rail.

## Problem

`Settings_UsesLegacyEnvironmentSyntax` (settings.c) is a ~60-clause chain of
`desc->field != &g_settings.<row>` pointer comparisons. A new modern setting
that a future author forgets to add here silently inherits the legacy AR_*
leading-zero/default-polarity parse via `ApplyLegacyEnvironmentValue`. It is
not a current bug — it is a latent maintainability trap.

The distinction is NOT category-derivable (verified: `audio_dialog_blip` and
`music_replacements` are `kSettingCat_Audio` yet stay legacy, while
`audio_enabled/frequency/samples` are modern). So a per-row flag is the right
mechanism.

## Change

Add `bool modern_env;` to `struct SettingDesc` (settings.h). Rewrite the
function body to `return !desc->modern_env;`. Set the flag true for exactly the
60 modern descriptors (see attached baseline).

**Churn reality (why it's gated):** the 60 modern fields are a MIX of literal
rows (e.g. `display_mode`, `audio_enabled`, `sim3d_tilt_x_mrad`) and
macro-generated rows (`BOOL_SETTING`/`INT_SETTING` produce BOTH modern
`sim3d_*` and legacy AR_* rows). So the flag cannot be defaulted per-macro; it
must be set per-invocation. Two options:

1. **Add a `modern_env` parameter** to `BOOL_SETTING`/`INT_SETTING` and the
   other emitting macros, and a positional field to the 74 literal rows. Most
   explicit; largest diff.
2. **Post-init pass** (lower churn, recommended): keep the table as-is, add the
   field defaulting to 0, and in a static initializer loop set
   `modern_env = true` for a small static list of the 60 modern KEYS (looked up
   once at startup). This localizes the change to one keyed list + the struct
   field + the function body — no macro or row edits. It trades a compile-time
   table for a one-time boot loop, which is negligible.

Option 2 also best addresses the original goal: adding a future modern setting
means adding one key to one list, mirroring today's "add one clause."

## Mandatory guard rail (attached, already built)

`docs/t2d-attachments/`:
- `legacy_env_baseline.tsv` — the golden vector: `index<TAB>key<TAB>legacy(0/1)`
  for all 258 descriptors on the PRE-refactor code (198 legacy, 60 modern).
- `legacy_env_probe.c` — standalone harness that dumps the same vector. It
  calls a one-line non-static shim you re-add temporarily to settings.c:
  `bool Settings_UsesLegacyEnvironmentSyntax_Probe(const SettingDesc *d){return Settings_UsesLegacyEnvironmentSyntax(d);}`
  and carries link stubs for the ~15 externs settings.c pulls in (the
  classifier itself calls none of them).

**Procedure:**
1. Re-add the probe shim; build:
   `cc -std=gnu11 -w -Isrc -Irecomp -Ithird_party/stb -Isnesrecomp-go/runtime/src -I/opt/homebrew/include docs/t2d-attachments/legacy_env_probe.c src/settings.c -o /tmp/probe`
2. Do the refactor (option 1 or 2).
3. Re-run `/tmp/probe > /tmp/after.tsv` and
   `diff docs/t2d-attachments/legacy_env_baseline.tsv /tmp/after.tsv`.
   **The diff MUST be empty** — that is the proof the resolved legacy/modern
   classification is byte-for-byte unchanged for every descriptor.
4. Remove the probe shim from settings.c.
5. Run the normal test tier: `cmake -DAR_TESTS_ONLY=ON` + ctest (settings +
   settings_overlay + save_system at minimum).

Do not merge on "it builds and TestLegacySeedEncodings passes" alone — that
test does not cover every field; the empty baseline diff is the real gate.
