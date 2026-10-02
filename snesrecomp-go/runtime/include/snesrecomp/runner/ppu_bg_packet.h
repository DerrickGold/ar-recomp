#ifndef SNESRECOMP_RUNNER_PPU_BG_PACKET_H
#define SNESRECOMP_RUNNER_PPU_BG_PACKET_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Optional, owned scanout command stream, separate from emulated state.
 * Raw tile rows and their scanline palettes preserve HDMA/VRAM/CGRAM changes
 * without retaining live pointers. Unsupported policies keep CPU ownership.
 * Words are native integers; byte-texture adapters upload little-endian data.
 * Only the used arena prefix is copied or uploaded. */
enum {
    SR_PPU_BG_PACKET_WIDTH = 640,
    SR_PPU_BG_PACKET_HEIGHT = 352,
    SR_PPU_BG_PACKET_HEADER_WORDS = 8,
    SR_PPU_BG_PACKET_ROW_WORDS = 8,
    SR_PPU_BG_PACKET_ROWS = 3 * SR_PPU_BG_PACKET_HEIGHT,
    SR_PPU_BG_PACKET_ARENA_BASE = SR_PPU_BG_PACKET_HEADER_WORDS +
        SR_PPU_BG_PACKET_ROWS * SR_PPU_BG_PACKET_ROW_WORDS,
    /* Three bands, each eight-pixel cell has four raw bitplanes, three
     * palette bitplanes, and black/backing/transparent/edit masks. */
    SR_PPU_BG_PACKET_CELL_WORDS = 9,
    SR_PPU_BG_PACKET_PIXEL_STRIDE = SR_PPU_BG_PACKET_WIDTH / 8 *
        SR_PPU_BG_PACKET_CELL_WORDS,
    SR_PPU_BG_PACKET_WORDS = SR_PPU_BG_PACKET_ARENA_BASE +
        SR_PPU_BG_PACKET_ROWS * (SR_PPU_BG_PACKET_PIXEL_STRIDE + 256),
    SR_PPU_BG_PACKET_TILES = 1,
    SR_PPU_BG_PACKET_VALIDATE_TILES = 2,
};

typedef struct SrPpuBgPacket {
    /* Caller initializes request_flags before scanout. Owned sources have
     * omitted CPU rows; consumers must use this packet or materialize it. */
    uint32_t request_flags;
    uint32_t owned_sources;
    uint32_t last_palette[3];
    uint32_t words[SR_PPU_BG_PACKET_WORDS];
} SrPpuBgPacket;

/* Header words: capture width/height, valid source bits, used word count,
 * skybox width/height, skybox alias source (BG1/BG2), reserved.
 * Row metadata (three sources, fixed 352-row stride):
 *   format, backing ARGB, backing begin/end, tile phase or alias span,
 *   alias source offset, palette offset, pixel/tile offset.
 * Formats: 0 unsupported; 1 three packed 9-bit palette codes per pixel;
 * 2 eight-pixel cells with raw/palette bitplanes and policy masks;
 * 3 compact native tile rows (raw four planes + palette/band/flip/lane mask);
 * 4 compact native rows plus a lazy edit-cell overlay at metadata word 5.
 * Format 1 codes: 0 transparent, 1..256 palette, 257 black, 258 backing;
 * bit 31 suppresses backing. Source 2 uses format 2 with one band and can
 * alias a BG interval; a zero data offset means alias/backing only.
 * Source bits 0/1/2 name BG1/BG2/independent skybox. */
static inline unsigned SrPpuBgPacket_Row(unsigned source, unsigned y) {
    return source * SR_PPU_BG_PACKET_HEIGHT + y;
}
static inline uint32_t *SrPpuBgPacket_Meta(SrPpuBgPacket *p, unsigned source, unsigned y) {
    return p->words + SR_PPU_BG_PACKET_HEADER_WORDS +
        SrPpuBgPacket_Row(source, y) * SR_PPU_BG_PACKET_ROW_WORDS;
}
static inline unsigned SrPpuBgPacket_RowCapacity(unsigned source) {
    return source == 2 ? SR_PPU_BG_PACKET_WIDTH / 8 * 3 : SR_PPU_BG_PACKET_PIXEL_STRIDE;
}
static inline unsigned SrPpuBgPacket_RowWords(const uint32_t *meta, unsigned source) {
    return meta[0] >= 3 ? (SR_PPU_BG_PACKET_WIDTH / 8 + 1) * 2 : SrPpuBgPacket_RowCapacity(source);
}
static inline void SrPpuBgPacket_Begin(SrPpuBgPacket *p, unsigned width, unsigned height) {
    memset(p->words, 0, SR_PPU_BG_PACKET_ARENA_BASE * sizeof(uint32_t));
    memset(p->last_palette, 0, sizeof(p->last_palette));
    p->owned_sources = 0;
    p->words[0] = width; p->words[1] = height; p->words[3] = SR_PPU_BG_PACKET_ARENA_BASE;
}
static inline size_t SrPpuBgPacket_Size(const SrPpuBgPacket *p) {
    const unsigned used = p->words[3];
    return offsetof(SrPpuBgPacket, words) + sizeof(uint32_t) *
        (used >= SR_PPU_BG_PACKET_ARENA_BASE && used <= SR_PPU_BG_PACKET_WORDS ? used : SR_PPU_BG_PACKET_WORDS);
}
static inline uint32_t *SrPpuBgPacket_AllocatePixels(SrPpuBgPacket *p, unsigned source, unsigned y) {
    uint32_t *meta = SrPpuBgPacket_Meta(p, source, y);
    if (!meta[7]) {
        const unsigned count = SrPpuBgPacket_RowWords(meta, source), at = p->words[3];
        if (at < SR_PPU_BG_PACKET_ARENA_BASE || at > SR_PPU_BG_PACKET_WORDS - count) return NULL;
        meta[7] = at; p->words[3] += count;
        memset(p->words + at, 0, count * sizeof(uint32_t));
    }
    return p->words + meta[7];
}
static inline int SrPpuBgPacket_SetPalette(SrPpuBgPacket *p, unsigned source, unsigned y,
        const uint32_t colors[256]) {
    if (!p->words[3]) p->words[3] = SR_PPU_BG_PACKET_ARENA_BASE;
    unsigned at = p->last_palette[source];
    if (!at || memcmp(p->words + at, colors, 256 * sizeof(uint32_t))) {
        at = p->words[3];
        if (at > SR_PPU_BG_PACKET_WORDS - 256) return 0;
        memcpy(p->words + at, colors, 256 * sizeof(uint32_t));
        p->words[3] += 256; p->last_palette[source] = at;
    }
    SrPpuBgPacket_Meta(p, source, y)[6] = at;
    return 1;
}
static inline uint32_t *SrPpuBgPacket_AllocateEdits(SrPpuBgPacket *p, uint32_t *meta) {
    if (!meta[5]) {
        const unsigned count = SR_PPU_BG_PACKET_PIXEL_STRIDE, at = p->words[3];
        if (at < SR_PPU_BG_PACKET_ARENA_BASE || at > SR_PPU_BG_PACKET_WORDS - count) return NULL;
        meta[5] = at; p->words[3] += count;
        memset(p->words + at, 0, count * sizeof(uint32_t));
    }
    return p->words + meta[5];
}
static inline uint32_t SrPpuBgPacket_CellColor(const SrPpuBgPacket *p,
        const uint32_t *meta, const uint32_t *cell, unsigned band, unsigned x) {
    const unsigned bit = x & 7u;
    const uint32_t lanes = cell[0] >> bit, palette = cell[1] >> bit;
    const unsigned value = (lanes & 1u) | ((lanes >> 7) & 2u) |
        ((lanes >> 14) & 4u) | ((lanes >> 21) & 8u);
    const unsigned base = ((palette & 1u) | ((palette >> 7) & 2u) |
        ((palette >> 14) & 4u)) * 16u;
    if (cell[2] & (1u << bit)) return UINT32_C(0xff000000);
    if (value) return p->words[meta[6] + base + value];
    if (cell[2] & (1u << (bit + 8))) return meta[1];
    return band == 0 && !(cell[2] & (1u << (bit + 16))) &&
        x >= meta[2] && x < meta[3] ? meta[1] : 0;
}
static inline uint32_t SrPpuBgPacket_Color(const SrPpuBgPacket *p,
        unsigned source, unsigned band, unsigned x, unsigned y) {
    if (source == 2) {
        const uint32_t *view = p->words + SR_PPU_BG_PACKET_HEADER_WORDS +
            SrPpuBgPacket_Row(source, y) * SR_PPU_BG_PACKET_ROW_WORDS;
        if (x >= (view[4] & 65535u) && x < (view[4] >> 16)) {
            const unsigned at = (unsigned)((int32_t)view[5] + (int32_t)x);
            source = p->words[6]; y = at / SR_PPU_BG_PACKET_WIDTH;
            x = at % SR_PPU_BG_PACKET_WIDTH;
        }
    }
    const unsigned row = SrPpuBgPacket_Row(source, y);
    const uint32_t *meta = p->words + SR_PPU_BG_PACKET_HEADER_WORDS +
        row * SR_PPU_BG_PACKET_ROW_WORDS;
    if (!meta[7]) return band == 0 && x >= meta[2] && x < meta[3] ? meta[1] : 0;
    const uint32_t *pixels = p->words + meta[7];
    if (meta[0] == 4u && meta[5]) {
        const uint32_t *edit = p->words + meta[5] + (x / 8u) * SR_PPU_BG_PACKET_CELL_WORDS + band * 3u;
        if (edit[2] & (1u << ((x & 7u) + 24u)))
            return SrPpuBgPacket_CellColor(p, meta, edit, band, x);
    }
    if (meta[0] >= 3u) {
        const unsigned column = x + meta[4], fine = column & 7u;
        const uint32_t *tile = pixels + (column / 8u) * 2u;
        const unsigned bit = tile[1] & 32u ? fine : 7u - fine;
        const uint32_t raw = tile[0] >> bit;
        const unsigned value = (raw & 1u) | ((raw >> 7) & 2u) |
            ((raw >> 14) & 4u) | ((raw >> 21) & 8u);
        if (((tile[1] >> 3) & 3u) == band && (tile[1] & (1u << (fine + 8))) && value)
            return p->words[meta[6] + (tile[1] & 7u) * 16u + value];
        return band == 0 && x >= meta[2] && x < meta[3] ? meta[1] : 0;
    }
    if (meta[0] == 2u) {
        const uint32_t *cell = pixels + (x / 8) * (source == 2 ? 3 : SR_PPU_BG_PACKET_CELL_WORDS) + band * 3;
        return SrPpuBgPacket_CellColor(p, meta, cell, band, x);
    }
    uint32_t encoded = pixels[x];
    const unsigned code = (encoded >> (band * 9)) & 511u;
    if (code == 257u) return UINT32_C(0xff000000);
    if (code == 258u) return meta[1];
    if (code > 258u) return 0;
    if (code) return p->words[meta[6] + code - 1];
    return band == 0 && !(encoded & UINT32_C(0x80000000)) &&
        x >= meta[2] && x < meta[3] ? meta[1] : 0u;
}

#endif
