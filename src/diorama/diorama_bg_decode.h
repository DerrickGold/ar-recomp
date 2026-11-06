#ifndef AR_DIORAMA_BG_DECODE_H
#define AR_DIORAMA_BG_DECODE_H

#include "snesrecomp/runner/ppu_bg_packet.h"

/* Materialize one row of an already valid, immutable capture packet. Width
 * and coordinates have the same bounds as SrPpuBgPacket_Color. Keep complex
 * ownership/alias policies in that reference decoder; only compact BG rows
 * without a full-add record use the tile-span path below. No state survives
 * the row, so HBlank palette, scroll, backing and edit changes stay local. */
static inline void DioramaBgDecode_Row(const SrPpuBgPacket *packet,
    unsigned source, unsigned band, unsigned y, unsigned width, uint32_t *out) {
  const uint32_t *meta = packet->words + SR_PPU_BG_PACKET_HEADER_WORDS +
      SrPpuBgPacket_Row(source, y) * SR_PPU_BG_PACKET_ROW_WORDS;
  const unsigned table = packet->words[7];
  if (source >= 2 || band >= 3 || (meta[0] != 3 && meta[0] != 4) ||
      !meta[7] || (table && packet->words[table + y])) {
    for (unsigned x = 0; x < width; ++x)
      out[x] = SrPpuBgPacket_Color(packet, source, band, x, y);
    return;
  }

  const uint32_t *tiles = packet->words + meta[7];
  const uint32_t *palette = packet->words + meta[6];
  const unsigned phase = meta[4];
  const unsigned backing_begin = band == 0 ? meta[2] : 0;
  const unsigned backing_end = band == 0 ? meta[3] : 0;
  const uint32_t backing = meta[1];
  for (unsigned x = 0; x < width;) {
    const unsigned column = x + phase, fine = column & 7u;
    const uint32_t *tile = tiles + (column / 8u) * 2u;
    const uint32_t raw = tile[0], info = tile[1];
    const unsigned end = width - x < 8u - fine ? width : x + 8u - fine;
    const unsigned selected = ((info >> 3) & 3u) == band;
    const unsigned visible = (info >> 8) & 255u;
    const uint32_t *colors = palette + (info & 7u) * 16u;
    unsigned lane = fine;
    for (; x < end; ++x, ++lane) {
      uint32_t color = x >= backing_begin && x < backing_end ? backing : 0;
      if (selected && (visible & (1u << lane))) {
        const unsigned bit = info & 32u ? lane : 7u - lane;
        const uint32_t bits = raw >> bit;
        const unsigned value = (bits & 1u) | ((bits >> 7) & 2u) |
            ((bits >> 14) & 4u) | ((bits >> 21) & 8u);
        if (value) color = colors[value];
      }
      out[x] = color;
    }
  }

  /* Edit cells are aligned to capture x, not the fine-scrolled native tile.
   * Their ownership bit replaces even a transparent/black/backing result.
   * Reuse the reference cell policy and preserve all palette alpha bits. */
  if (meta[0] == 4 && meta[5]) {
    const uint32_t *edits = packet->words + meta[5];
    for (unsigned x = 0; x < width; x += 8u) {
      const uint32_t *cell = edits + (x / 8u) * SR_PPU_BG_PACKET_CELL_WORDS + band * 3u;
      const unsigned owned = cell[2] >> 24;
      if (!owned) continue;
      const unsigned count = width - x < 8u ? width - x : 8u;
      for (unsigned lane = 0; lane < count; ++lane)
        if (owned & (1u << lane))
          out[x + lane] = SrPpuBgPacket_CellColor(packet, meta, cell, band, x + lane);
    }
  }
}

#endif
