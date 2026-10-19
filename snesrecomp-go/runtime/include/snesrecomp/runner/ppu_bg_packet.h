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
    SR_PPU_BG_PACKET_FULL_ADD_ROW_WORDS = 4 + SR_PPU_BG_PACKET_WIDTH,
    SR_PPU_BG_PACKET_WORDS = SR_PPU_BG_PACKET_ARENA_BASE +
        SR_PPU_BG_PACKET_ROWS * (SR_PPU_BG_PACKET_PIXEL_STRIDE + 256) +
        SR_PPU_BG_PACKET_HEIGHT * (1 + SR_PPU_BG_PACKET_FULL_ADD_ROW_WORDS),
    SR_PPU_BG_PACKET_TILES = 1,
    SR_PPU_BG_PACKET_VALIDATE_TILES = 2,
    SR_PPU_BG_PACKET_FULL_ADD = 4,
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
 * skybox width/height, skybox alias source (BG1/BG2), optional full-add row table.
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
/* Validate offsets before a host or GPU consumes an externally supplied packet. */
static inline int SrPpuBgPacket_Valid(const SrPpuBgPacket *p) {
    if (!p) return 0;
    const unsigned used = p->words[3];
    if (used < SR_PPU_BG_PACKET_ARENA_BASE || used > SR_PPU_BG_PACKET_WORDS ||
            (p->words[2] & ~7u) || p->words[0] > SR_PPU_BG_PACKET_WIDTH ||
            p->words[1] > SR_PPU_BG_PACKET_HEIGHT) return 0;
    if (p->words[7]) {
        if (p->words[7] < SR_PPU_BG_PACKET_ARENA_BASE || p->words[7] > used - SR_PPU_BG_PACKET_HEIGHT)
            return 0;
        for (unsigned y = 0; y < p->words[1] && y < SR_PPU_BG_PACKET_HEIGHT; ++y) {
            const unsigned at = p->words[p->words[7] + y];
            if (!at) continue;
            if (at < SR_PPU_BG_PACKET_ARENA_BASE || at > used - SR_PPU_BG_PACKET_FULL_ADD_ROW_WORDS ||
                    (p->words[at] & ~0x3f13u)) return 0;
            for (unsigned source = 0; source < 2; ++source) {
                const uint32_t *meta = p->words + SR_PPU_BG_PACKET_HEADER_WORDS +
                        SrPpuBgPacket_Row(source, y) * SR_PPU_BG_PACKET_ROW_WORDS;
                if (meta[0] < 3 || !meta[7] || (meta[0] == 3 && meta[5])) return 0;
            }
        }
    }
    for (unsigned source = 0; source < 3; ++source) {
        const unsigned height = p->words[source == 2 ? 5 : 1];
        const unsigned width = p->words[source == 2 ? 4 : 0];
        if (width > SR_PPU_BG_PACKET_WIDTH || height > SR_PPU_BG_PACKET_HEIGHT) return 0;
        for (unsigned y = 0; y < height; ++y) {
            const uint32_t *row = p->words + SR_PPU_BG_PACKET_HEADER_WORDS +
                    SrPpuBgPacket_Row(source, y) * SR_PPU_BG_PACKET_ROW_WORDS;
            if (!row[0]) continue;
            if (row[0] > 4 || (row[0] >= 3 && row[4] > 7) || (source == 2 && row[0] != 2) ||
                    row[6] < SR_PPU_BG_PACKET_ARENA_BASE || row[6] > used - 256 ||
                    (row[7] && (row[7] < SR_PPU_BG_PACKET_ARENA_BASE ||
                            row[7] > used - SrPpuBgPacket_RowWords(row, source))) ||
                    (row[0] == 4 && row[5] && (row[5] < SR_PPU_BG_PACKET_ARENA_BASE ||
                            row[5] > used - SR_PPU_BG_PACKET_PIXEL_STRIDE))) return 0;
            if (source == 2 && row[4]) {
                const unsigned first = row[4] & 65535u, end = row[4] >> 16;
                const int64_t start = (int32_t)row[5] + (int64_t)first;
                const int64_t last = (int32_t)row[5] + (int64_t)end - 1;
                if (p->words[6] > 1 || first >= end || end > width || start < 0 ||
                        last >= (int64_t)p->words[1] * SR_PPU_BG_PACKET_WIDTH) return 0;
                for (unsigned alias_y = (unsigned)start / SR_PPU_BG_PACKET_WIDTH;
                          alias_y <= (unsigned)last / SR_PPU_BG_PACKET_WIDTH; ++alias_y)
                    if (!p->words[SR_PPU_BG_PACKET_HEADER_WORDS +
                            SrPpuBgPacket_Row(p->words[6], alias_y) * SR_PPU_BG_PACKET_ROW_WORDS]) return 0;
            }
        }
    }
    return 1;
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
/* Full-add rows retain competing non-BG winners, not a decoded BG image.
 * Header: full-add sources | math sources << 8, BG low/high ranks (4 nibbles),
 * capture begin/end (two 16-bit columns), owner-sub BG bits. Each pixel stores main/sub
 * non-BG keys (rank << 3 | source) in bytes 0/1 and BG main/sub visibility in
 * bits 16..19. Native tile info retains its hardware rank in bits 16..19.
 * CPU fallback and sparse OBJ export use the same immutable packet as GPU. */
static inline uint32_t *SrPpuBgPacket_FullAddRow(SrPpuBgPacket *p, unsigned y) {
    const unsigned table = p->words[7];
    return table && p->words[table + y] ? p->words + p->words[table + y] : NULL;
}
static inline uint32_t *SrPpuBgPacket_AllocateFullAdd(SrPpuBgPacket *p, unsigned y) {
    if (!p->words[7]) {
        const unsigned at = p->words[3];
        if (at > SR_PPU_BG_PACKET_WORDS - SR_PPU_BG_PACKET_HEIGHT) return NULL;
        p->words[7] = at; p->words[3] += SR_PPU_BG_PACKET_HEIGHT;
        memset(p->words + at, 0, SR_PPU_BG_PACKET_HEIGHT * sizeof(uint32_t));
    }
    uint32_t *slot = p->words + p->words[7] + y;
    if (!*slot) {
        const unsigned at = p->words[3];
        if (at > SR_PPU_BG_PACKET_WORDS - SR_PPU_BG_PACKET_FULL_ADD_ROW_WORDS) return NULL;
        *slot = at; p->words[3] += SR_PPU_BG_PACKET_FULL_ADD_ROW_WORDS;
        memset(p->words + at, 0, SR_PPU_BG_PACKET_FULL_ADD_ROW_WORDS * sizeof(uint32_t));
    }
    return p->words + *slot;
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
static inline uint32_t SrPpuBgPacket_RawColor(const SrPpuBgPacket *p,
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

/* Return rank in the high byte and presentation band in the low byte.
 * Authored bands can differ from hardware priority; do not infer native rank
 * from their presentation slot. Edits replace only the bands they own. */
static inline unsigned SrPpuBgPacket_FullAddSample(const SrPpuBgPacket *p,
        unsigned source, unsigned x, unsigned y, unsigned ranks) {
    const uint32_t *meta = p->words + SR_PPU_BG_PACKET_HEADER_WORDS +
        SrPpuBgPacket_Row(source, y) * SR_PPU_BG_PACKET_ROW_WORDS;
    const unsigned column = x + meta[4], fine = column & 7u;
    const uint32_t *tile = p->words + meta[7] + (column / 8u) * 2u;
    const unsigned bit = tile[1] & 32u ? fine : 7u - fine;
    const unsigned value = (tile[0] | (tile[0] >> 8) | (tile[0] >> 16) | (tile[0] >> 24)) & (1u << bit);
    unsigned band = (tile[1] >> 3) & 3u;
    unsigned rank = value && (tile[1] & (1u << (fine + 8))) ? (tile[1] >> 16) & 15u : 0;
    if (meta[5]) {
        const uint32_t *cells = p->words + meta[5] + (x / 8u) * 9u;
        const unsigned owned = 1u << ((x & 7u) + 24u);
        if (cells[band * 3u + 2u] & owned) rank = 0;
        /* Ordinary backing wins a tie with a far-band edit. */
        for (unsigned edit_band = 0; edit_band < 3; ++edit_band) {
            const uint32_t *cell = cells + edit_band * 3u;
            if (!(cell[2] & owned) || !SrPpuBgPacket_CellColor(p, meta, cell, edit_band, x)) continue;
            const unsigned edit_rank = (ranks >> (source * 8u + (edit_band == 1 ? 4u : 0u))) & 15u;
            if (edit_rank > rank) { rank = edit_rank; band = edit_band; }
        }
    }
    return (rank << 8) | band;
}
static inline unsigned SrPpuBgPacket_FullAddWinner(const SrPpuBgPacket *p,
        const uint32_t *row, unsigned x, unsigned y) {
    const uint32_t state = row[4 + x];
    unsigned main = state & 255u, sub = (state >> 8) & 255u, band = 0;
    for (unsigned source = 0; source < 2; ++source) {
        const unsigned sample = SrPpuBgPacket_FullAddSample(p, source, x, y, row[1]);
        if ((sample >> 8) <= 1) continue;
        const unsigned key = ((sample >> 8) << 3) | source;
        if ((state & (1u << (16 + source))) && key > main) main = key;
        if ((state & (1u << (18 + source))) && key > sub) { sub = key; band = sample & 255u; }
    }
    return (main & 7u) < 5u && (row[0] & (1u << (8 + (main & 7u))))
        ? sub | (band << 8) : 0;
}
static inline uint32_t SrPpuBgPacket_Color(const SrPpuBgPacket *p,
        unsigned source, unsigned band, unsigned x, unsigned y) {
    /* A skybox alias follows the same resolved capture as its source. */
    if (source == 2) {
        const uint32_t *view = p->words + SR_PPU_BG_PACKET_HEADER_WORDS +
            SrPpuBgPacket_Row(2, y) * SR_PPU_BG_PACKET_ROW_WORDS;
        if (x >= (view[4] & 65535u) && x < (view[4] >> 16)) {
            const unsigned at = (unsigned)((int32_t)view[5] + (int32_t)x);
            source = p->words[6]; y = at / SR_PPU_BG_PACKET_WIDTH; x = at % SR_PPU_BG_PACKET_WIDTH;
        }
    }
    const unsigned table = p->words[7], offset = table && source < 2 ? p->words[table + y] : 0;
    if (!offset)
        return SrPpuBgPacket_RawColor(p, source, band, x, y);
    const uint32_t *row = p->words + offset;
    const uint32_t *meta = p->words + SR_PPU_BG_PACKET_HEADER_WORDS +
        SrPpuBgPacket_Row(source, y) * SR_PPU_BG_PACKET_ROW_WORDS;
    if (!(row[0] & (1u << source))) {
        const unsigned owner = row[3] & (1u << source) ? 18u : 16u;
        if (row[4 + x] & (1u << (owner + source))) return SrPpuBgPacket_RawColor(p, source, band, x, y);
        return band == 0 && x >= meta[2] && x < meta[3] ? meta[1] : 0;
    }
    const unsigned winner = SrPpuBgPacket_FullAddWinner(p, row, x, y);
    const int keep = (winner & 255u) > 15u && (winner & 7u) == source && (winner >> 8) == band;
    if (keep) return SrPpuBgPacket_RawColor(p, source, band, x, y);
    /* Clear losing native/edited pixels; an untouched transparent-fill
     * backing remains, just as in the CPU capture clear/export sequence. */
    if (meta[5] && (p->words[meta[5] + (x / 8u) * 9u + band * 3u + 2u] &
        (1u << ((x & 7u) + 24u)))) return 0;
    return band == 0 && x >= meta[2] && x < meta[3] ? meta[1] : 0;
}

#endif
