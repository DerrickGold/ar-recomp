#ifndef SIM_WORLD_MAP_ROWS_H
#define SIM_WORLD_MAP_ROWS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Policy for the world-map underlay's first 8 tilemap rows (F1 from the
 * 2026-07-26 handback: "still get corrupt world map graphics on the top part",
 * a structured stripe across the top band, and per the tester NOT reproducible
 * every run).
 *
 * Why the rows are special. SimWorldMap_Init seeds the whole 128x128 tilemap
 * from ROM ($06:B341), which is the UNDEVELOPED world -- verified 16172/16384
 * bytes identical to a live capture, the deltas being exactly the player's
 * runtime edits. SimWorldMap_Refresh then adopts the live Mode-7 shadow at
 * 7E:C000 every frame the current map owns it, which is what makes the underlay
 * show towns at their present development state. But it starts at row
 * kSimWorldMapVolatileRows = 8 unless the world-map screen itself is on screen,
 * because on a town map the game reuses that region for something that is not
 * terrain. So rows 0-7 have exactly two possible histories:
 *
 *   - The player never opened the world map this session: the rows still hold
 *     the ROM baseline. Wrong-but-plausible terrain (the undeveloped north
 *     strip), never garbage.
 *   - The player did open it: with first_row == 0 those rows were adopted from
 *     the shadow, including whatever the world-map screen keeps there. If that
 *     is not terrain, the bytes index arbitrary tile art and expand through the
 *     terrain palette into a STRUCTURED pattern -- which is what the screenshot
 *     shows, and which explains why it varies between runs.
 *
 * Only the second history can produce garbage, so the two candidate fixes are
 * genuinely different repairs and not two spellings of one:
 *
 *   Restore  -- treat the adopted rows as untrustworthy and put the ROM baseline
 *               back. Cannot show garbage. Costs any real development in the
 *               north strip, which is Northwall's window (origin y=0).
 *   Trust    -- treat the rows as terrain and keep adopting them on town maps
 *               too, i.e. stop skipping. Shows real development if the region
 *               is genuinely terrain; shows the garbage if it is not.
 *
 * Exactly one of those is right, and which one cannot be decided without a ROM:
 * it depends on what the game actually stores in the first 8 rows on a town
 * map. Both are therefore implemented and selected at runtime so a single
 * on-device session can settle it. See AB-TEST-worldmap-top-band.md.
 *
 * Pure: no ROM, no SDL, no globals. The caller supplies the row range and the
 * baseline; this decides only the policy arithmetic.
 */

typedef enum SimWorldRowPolicy {
  /* Ship-today behaviour: skip rows 0-7 on a town map, adopt them on the world
   * map screen. The baseline persists until a world-map visit replaces it. */
  kSimWorldRows_Legacy = 0,
  /* Fix A: never let the shadow own rows 0-7; re-assert the ROM baseline. */
  kSimWorldRows_RestoreBaseline = 1,
  /* Fix B: always adopt rows 0-7 from the shadow, town maps included. */
  kSimWorldRows_TrustShadow = 2,
  /* Fix C: the whole-map version of A. Treats the FIRST town frame after any
   * untrusted map as incoherent and restores the full 16384-byte baseline
   * instead of diffing against a shadow the previous owner may still hold; only
   * resumes adoption once a world-map frame has demonstrably rebuilt the buffer.
   *
   * This exists because F1 and F2 are one defect in two row ranges.
   * ShadowIsTrustworthy tests the map identity *right now*, but the corruption
   * it guards against is *durable in the buffer*: it correctly refuses adoption
   * while an action stage owns 7E:C000, then permits it on the very first town
   * frame afterwards -- when the buffer still holds that stage's bytes. Nothing
   * asks whether the shadow has been REBUILT since. Once adopted the damage is
   * unrecoverable, because the tilemap is mutated in place and the diff only
   * adopts bytes that DIFFER: tilemap == shadow == garbage forever, no serial
   * bump, bad image republished indefinitely.
   *
   * Rows 8+ (F2) have no other recovery path at all, which is why this is the
   * candidate that supersedes RestoreBaseline rather than complementing it. */
  kSimWorldRows_CoherenceGate = 3,
} SimWorldRowPolicy;

/* True when this frame's shadow must be treated as INCOHERENT -- i.e. the buffer
 * may still belong to a previous, non-world-map owner, so diffing against it
 * would adopt that owner's bytes as terrain.
 *
 * `trusted_now` is ShadowIsTrustworthy for this frame's ($18,$19).
 * `rebuilt_since` records whether a world-map frame has been seen since the last
 * untrusted map -- the only evidence in reach that the game repopulated the
 * buffer, since the module cannot see the game's own write cursor.
 *
 * Note this is deliberately conservative at boot: until the first world-map
 * frame, a town's shadow is unverified, so it is treated as incoherent and the
 * ROM baseline shows instead. That is the correct default -- the baseline is the
 * undeveloped world, which is wrong-but-plausible, whereas an unverified buffer
 * can be another screen's data.
 *
 * Only CoherenceGate ever returns true: the other policies keep today's
 * identity-only behaviour so they remain honest A/B comparisons. */
bool SimWorldRows_ShadowIsIncoherent(SimWorldRowPolicy policy, bool trusted_now,
                                     bool rebuilt_since);

/* First tilemap row Refresh should diff for this frame.
 *
 * `on_world_map_screen` is (map_number == kActRaiserNonActionMap_WorldMap): the
 * one map whose shadow is known to cover rows 0-7. `volatile_rows` is
 * kSimWorldMapVolatileRows.
 *
 * Legacy and RestoreBaseline both skip the rows while adopting (RestoreBaseline
 * additionally rewrites them -- see SimWorldRows_ShouldRestoreBaseline), so the
 * shadow can never introduce non-terrain. TrustShadow always adopts from 0. */
int SimWorldRows_FirstAdoptedRow(SimWorldRowPolicy policy,
                                 bool on_world_map_screen, int volatile_rows);

/* True when this frame must re-assert the ROM baseline over rows 0-7.
 *
 * Only RestoreBaseline does, and only where the shadow could have replaced them
 * -- i.e. NOT on the world-map screen, where the live rows are the authentic
 * thing to show. Returning true every qualifying frame is intentional: the
 * restore is a no-op once the rows already match, because the caller diffs
 * before writing, so a stale row is repaired even if it was adopted many frames
 * earlier (including before this policy was switched on mid-session). */
bool SimWorldRows_ShouldRestoreBaseline(SimWorldRowPolicy policy,
                                        bool on_world_map_screen);

/* Byte range [*out_begin, *out_end) of the tilemap covered by rows
 * [0, volatile_rows). Zero-length when volatile_rows <= 0. Clamps to
 * `total_bytes` so a bad constant cannot walk off the array. */
void SimWorldRows_BaselineSpan(int volatile_rows, int tiles_per_row,
                               size_t total_bytes, size_t *out_begin,
                               size_t *out_end);

/* How much of the baseline a policy needs retained. CoherenceGate needs the
 * WHOLE map, because rows 8+ are what it restores; RestoreBaseline needs only
 * the volatile strip; the passive policies need none. Retaining the full 16 KB
 * unconditionally is simpler and the module already holds a 4 MB pixel buffer,
 * so this exists to document the requirement rather than to save memory --
 * a policy added later that restores rows it never snapshotted would silently
 * restore zeros, which is the failure this makes explicit. */
bool SimWorldRows_NeedsFullBaseline(SimWorldRowPolicy policy);

/* Short token for logs and the settings UI. Never NULL. */
const char *SimWorldRows_PolicyName(SimWorldRowPolicy policy);

#endif /* SIM_WORLD_MAP_ROWS_H */
