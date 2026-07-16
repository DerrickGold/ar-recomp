#ifndef HD_REPLACEMENTS_H
#define HD_REPLACEMENTS_H

#include <stdbool.h>
#include "types.h"

/* Manifest-driven HD graphics replacements (game-assets/hd/manifest.ini).
 *
 * Each [replace:<name>] entry declares a substitution "plane" — the tool used
 * to swap the graphic. Planes deliberately have different capability tiers:
 *
 *   screen  host-overlay substitution of a screen-locked, untransformed
 *           graphic via the generic PPU capture API (works today)
 *   mode7   canvas-space texture override sampled through the Mode-7 matrix
 *           (parsed + reserved; needs the engine override hook)
 *   tiles   hash-keyed HD tile pack (parsed + reserved; needs the N-x
 *           RGBA-sideband renderer path)
 *
 * The gate mini-language is a comma-separated conjunction of comparisons:
 *
 *   when = wram[0018]==0x00, wram[0019]==0x00, mode==7, m7==identity
 *
 * Operands: wram[<hex addr>] (byte), mode (BG mode 0-7), m7a/m7b/m7c/m7d
 * (Mode-7 matrix elements, raw 16-bit), m7 (only "identity"). Operators:
 * == and !=. Values parse with strtoul base 0 (0x for hex).
 *
 * Ownership mirrors the overlay contract: this module owns parsing and the
 * per-frame gate/capture policy (game side); the host owns image decoding,
 * textures, binding, and final composition (main.c). With no texture loaded
 * (headless, missing file) an entry never requests a capture, so emulated
 * output is untouched. */

typedef enum HdPlane {
  kHdPlane_Screen = 0,
  kHdPlane_Mode7 = 1,
  kHdPlane_Tiles = 2,
} HdPlane;

typedef enum HdConditionKind {
  kHdCond_WramByte = 0,  /* g_ram[address] vs value */
  kHdCond_BgMode = 1,    /* (bgmode & 7) vs value */
  kHdCond_M7Element = 2, /* (uint16)m7matrix[address] vs value */
  kHdCond_M7Identity = 3,
} HdConditionKind;

typedef struct HdCondition {
  uint8 kind;
  uint8 negate; /* 0: ==, 1: != */
  uint16 address;
  uint16 value;
} HdCondition;

enum {
  kHdMaxReplacements = 16,
  kHdMaxConditions = 8,
  kHdMaxName = 48,
  kHdMaxPath = 512,
};

typedef struct HdReplacement {
  char name[kHdMaxName];
  HdPlane plane;
  int source; /* PpuOverlaySource for the screen plane */
  int x0, y0, x1, y1; /* screen-space rect, x1/y1 exclusive */
  char image[kHdMaxPath]; /* resolved relative to the manifest */
  bool brightness_mod; /* default true: follow INIDISP master brightness */
  HdCondition conditions[kHdMaxConditions];
  int condition_count;

  /* Host-owned: SDL_Texture* once the image decoded; NULL keeps the entry
   * fully inert (no capture request, authentic rendering). */
  void *texture;
  /* Game-policy result, rebuilt every emulated frame. */
  bool active;
} HdReplacement;

extern HdReplacement g_hd_replacements[kHdMaxReplacements];
extern int g_hd_replacement_count;

/* Parse a manifest. Returns the number of entries loaded; 0 with no error
 * output if the file simply does not exist. Safe to call once at startup. */
int HdReplacements_Load(const char *path);

/* Per-frame game policy: evaluate every screen-plane entry's gate and request
 * overlay captures for the winners. Call from the frame hook after the other
 * capture policies (HUD split, magic OAM) so source conflicts are detected
 * rather than clobbered. */
void HdReplacements_EvaluateFrame(void);

#endif /* HD_REPLACEMENTS_H */
