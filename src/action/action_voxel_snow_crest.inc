/* Compile-time native summit crest strips; no geometry or source mutation.
 * Caller proves reviewed summit ownership, original native source, masks,
 * static CHR, matching palette/priority and current source revision.
 * Each column starts at its actual exposed native top pixel. It contains
 * at most 16 consecutive opaque pixels moving inward/down the native front.
 * No donor search, color averaging, palette heuristic or geometry change.
 */
#ifndef STAGED_SUMMIT_CREST_SKIN_H
#define STAGED_SUMMIT_CREST_SKIN_H
#include <stdbool.h>
#include <stdint.h>
typedef struct SummitCrestColumn {
  uint16_t colors[16]; /* Exact native RGB555, bit15 marks valid source ink. */
  uint8_t length;      /* Continuous original ink before native alpha; 1..16. */
} SummitCrestColumn;
static bool SummitCrest_Build(const SummitCrestColumn columns[16],
    uint16_t exposed_columns, uint16_t pixels[256]) {
  if (!columns || !pixels || !exposed_columns) return false;
  /* Validate before writing, so a rejected current source cannot leave a
   * partially refreshed atlas while an earlier valid image remains cached. */
  for (unsigned x=0;x<16;++x) if (exposed_columns&(1u<<x)) {
    const unsigned n=columns[x].length;
    if (!n || n>16) return false;
    for (unsigned y=0;y<n;++y)
      if (!(columns[x].colors[y]&0x8000)) return false;
  }
  for (unsigned y=0;y<16;++y) for (unsigned x=0;x<16;++x) {
    if (!(exposed_columns&(1u<<x))) {pixels[y*16+x]=0;continue;}
    const unsigned n=columns[x].length;
    /* A narrow native tip can end before row16. Hold its last original
     * edge ink; do not introduce F3 white, sample sky, or change support. */
    pixels[y*16+x]=columns[x].colors[y<n?y:n-1];
  }
  return true;
}
#endif
