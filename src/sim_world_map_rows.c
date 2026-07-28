#include "sim_world_map_rows.h"

#include <stddef.h>

int SimWorldRows_FirstAdoptedRow(SimWorldRowPolicy policy,
                                 bool on_world_map_screen, int volatile_rows) {
  if (volatile_rows < 0) volatile_rows = 0;
  /* On the world-map screen the shadow demonstrably covers the whole tilemap,
   * so every policy adopts from row 0 there — including RestoreBaseline, whose
   * job is to protect town maps, not to refuse the authentic view. */
  if (on_world_map_screen) return 0;
  switch (policy) {
    case kSimWorldRows_TrustShadow:
      return 0;
    case kSimWorldRows_CoherenceGate:
      /* Rows 0-7 stay skipped on a town map for the same reason as Legacy: the
       * game reuses that strip (the Sky Palace metatile page lands at 7E:C200,
       * which is rows 4-5). The gate's contribution is refusing the WHOLE
       * shadow on an incoherent frame — see SimWorldRows_ShadowIsIncoherent. */
      return volatile_rows;
    case kSimWorldRows_RestoreBaseline:
    case kSimWorldRows_Legacy:
    default:
      return volatile_rows;
  }
}

bool SimWorldRows_ShadowIsIncoherent(SimWorldRowPolicy policy, bool trusted_now,
                                     bool rebuilt_since) {
  if (policy != kSimWorldRows_CoherenceGate) return false;
  /* An untrusted frame adopts nothing anyway, so it is never "incoherent" in the
   * sense that matters — the caller returns before reaching the diff. */
  if (!trusted_now) return false;
  /* Already coherent: a world-map frame has repopulated the buffer since the
   * last foreign owner, so the shadow is the game's own world map again. */
  if (rebuilt_since) return false;
  /* Trusted now, and no rebuild seen since the last foreign owner (or since
   * boot). The buffer is unverified, so the ROM baseline is the best available
   * truth and adopting is exactly the risk this policy refuses. */
  return true;
}

bool SimWorldRows_NeedsFullBaseline(SimWorldRowPolicy policy) {
  return policy == kSimWorldRows_CoherenceGate;
}

bool SimWorldRows_ShouldRestoreBaseline(SimWorldRowPolicy policy,
                                        bool on_world_map_screen) {
  return policy == kSimWorldRows_RestoreBaseline && !on_world_map_screen;
}

void SimWorldRows_BaselineSpan(int volatile_rows, int tiles_per_row,
                               size_t total_bytes, size_t *out_begin,
                               size_t *out_end) {
  size_t begin = 0, end = 0;
  if (volatile_rows > 0 && tiles_per_row > 0) {
    end = (size_t)volatile_rows * (size_t)tiles_per_row;
    if (end > total_bytes) end = total_bytes;
  }
  if (out_begin) *out_begin = begin;
  if (out_end) *out_end = end;
}

const char *SimWorldRows_PolicyName(SimWorldRowPolicy policy) {
  switch (policy) {
    case kSimWorldRows_RestoreBaseline: return "restore-baseline";
    case kSimWorldRows_TrustShadow:     return "trust-shadow";
    case kSimWorldRows_CoherenceGate:   return "coherence-gate";
    case kSimWorldRows_Legacy:          return "legacy";
    default:                            return "legacy";
  }
}
