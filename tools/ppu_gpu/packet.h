#ifndef AR_PPU_GPU_PROBE_PACKET_H
#define AR_PPU_GPU_PROBE_PACKET_H

/* Experimental, owned Mode-1 background job. No GPU handles, borrowed callbacks
 * or native struct dumps cross the upload boundary. RGBA8 texels encode little
 * endian words explicitly in an RGBA8_UNORM texture. This
 * is a probe contract, not a new runner ABI or general scanout replacement. */
#include "snesrecomp/runner/scene_renderer.h"

enum {
  kProbeTextureWidth = 256,
  kProbeRowBase = 16,
  kProbeRowWords = 8,
  kProbeVramBase = kProbeRowBase + 2 * SR_SCENE_HEIGHT * kProbeRowWords,
  kProbePaletteBase = kProbeVramBase + 16384,
  kProbeTileBase = kProbePaletteBase + 512,
  kProbeTilesPerRow = 65,
  kProbeWords = kProbeTileBase + 2 * SR_SCENE_HEIGHT * kProbeTilesPerRow,
  kProbeTextureHeight = (kProbeWords + kProbeTextureWidth - 1) / kProbeTextureWidth,
};
/* The offline shader uses these constants too; fail if the SDK layout changes. */
_Static_assert(SR_SCENE_HEIGHT == 352 && kProbeRowBase == 16 &&
    kProbeRowWords == 8 && kProbeVramBase == 5648 && kProbePaletteBase == 22032,
    "Update ppu_bg_probe.frag.glsl when changing the probe packet layout");

typedef struct PpuGpuProbePacket {
  uint8_t rgba[kProbeTextureWidth * kProbeTextureHeight * 4];
  unsigned width, height; /* Capture pitch (including clear aprons), per-band height. */
} PpuGpuProbePacket;

/* Supports finite Mode-1 world tiles, per-row scroll, hardware/custom bands,
 * capture extents, half-add alpha and fixed subtract. Rejects edits, mosaic,
 * winner-dependent exports and non-live-world edge policies. Failure sets
 * dimensions to zero; partial packet contents must never be submitted. */
bool PpuGpuProbe_Build(const SrSceneFrame *frame, PpuGpuProbePacket *packet);

#endif
