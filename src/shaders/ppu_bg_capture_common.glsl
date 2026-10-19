// Shared immutable PPU packet decoding for compute and fragment backends.
vec4 argb(uint c) {
    return vec4((c >> 16u) & 255u, (c >> 8u) & 255u, c & 255u, c >> 24u) / 255.0;
}
vec4 cellColor(uint cell, uint meta, uint colors, uint band, uint x) {
    uint bit = x & 7u;
    uint lanes = word(cell) >> bit, palette = word(cell + 1u) >> bit;
    uint flags = word(cell + 2u);
    uint pixel = (lanes & 1u) | ((lanes >> 7u) & 2u) |
        ((lanes >> 14u) & 4u) | ((lanes >> 21u) & 8u);
    uint base = ((palette & 1u) | ((palette >> 7u) & 2u) |
        ((palette >> 14u) & 4u)) * 16u;
    if ((flags & (1u << bit)) != 0u) return vec4(0.0, 0.0, 0.0, 1.0);
    if (pixel != 0u) return argb(word(colors + base + pixel));
    if ((flags & (1u << (bit + 8u))) != 0u ||
        (band == 0u && (flags & (1u << (bit + 16u))) == 0u &&
         x >= word(meta + 2u) && x < word(meta + 3u))) return argb(word(meta + 1u));
    return vec4(0.0);
}
vec4 rawColorAt(uvec2 p, uint source, uint band) {
    vec4 result = vec4(0.0);
    if (p.x >= word(source == 2u ? 4u : 0u) || p.y >= word(source == 2u ? 5u : 1u)) return result;
    uint row = source * 352u + p.y, meta = 8u + row * 8u;
    if (source == 2u) {
        uint alias = word(meta + 4u);
        if (p.x >= (alias & 65535u) && p.x < (alias >> 16u)) {
            uint at = uint(int(word(meta + 5u)) + int(p.x));
            source = word(6u); p = uvec2(at % 640u, at / 640u);
            row = source * 352u + p.y; meta = 8u + row * 8u;
        }
    }
    if (word(meta) == 0u) return result;
    uint pixels = word(meta + 7u), colors = word(meta + 6u);
    if (pixels == 0u) {
        if (band == 0u && p.x >= word(meta + 2u) && p.x < word(meta + 3u)) result = argb(word(meta + 1u));
        return result;
    }
    if (word(meta) == 4u && word(meta + 5u) != 0u) {
        uint edit = word(meta + 5u) + (p.x / 8u) * 9u + band * 3u;
        if ((word(edit + 2u) & (1u << ((p.x & 7u) + 24u))) != 0u) {
            result = cellColor(edit, meta, colors, band, p.x);
            return result;
        }
    }
    if (word(meta) >= 3u) {
        uint column = p.x + word(meta + 4u), fine = column & 7u;
        uint tile = pixels + (column / 8u) * 2u, info = word(tile + 1u);
        uint bit = (info & 32u) != 0u ? fine : 7u - fine;
        uint raw = word(tile) >> bit;
        uint value = (raw & 1u) | ((raw >> 7u) & 2u) |
            ((raw >> 14u) & 4u) | ((raw >> 21u) & 8u);
        if (((info >> 3u) & 3u) == band && (info & (1u << (fine + 8u))) != 0u && value != 0u)
            result = argb(word(colors + (info & 7u) * 16u + value));
        else if (band == 0u && p.x >= word(meta + 2u) && p.x < word(meta + 3u)) result = argb(word(meta + 1u));
        return result;
    }
    if (word(meta) == 2u) {
        uint cell = pixels + (p.x / 8u) * (source == 2u ? 3u : 9u) + band * 3u;
        result = cellColor(cell, meta, colors, band, p.x);
        return result;
    }
    uint value = word(pixels + p.x);
    uint code = (value >> (band * 9u)) & 511u;
    if (code == 257u) result = vec4(0.0, 0.0, 0.0, 1.0);
    else if (code == 258u) result = argb(word(meta + 1u));
    else if (code != 0u && code <= 256u) result = argb(word(colors + code - 1u));
    else if (code == 0u && band == 0u && (value & 0x80000000u) == 0u &&
        p.x >= word(meta + 2u) && p.x < word(meta + 3u)) result = argb(word(meta + 1u));
    return result;
}
// Keys preserve SNES rank and source; band is a separate presentation choice.
uint fullAddSample(uint source, uvec2 p, uint ranks) {
    uint meta = 8u + (source * 352u + p.y) * 8u;
    uint column = p.x + word(meta + 4u), fine = column & 7u;
    uint tile = word(meta + 7u) + (column / 8u) * 2u, info = word(tile + 1u);
    uint bit = (info & 32u) != 0u ? fine : 7u - fine;
    uint raw = word(tile), value = (raw | (raw >> 8u) | (raw >> 16u) | (raw >> 24u)) & (1u << bit);
    uint band = (info >> 3u) & 3u;
    uint rank = value != 0u && (info & (1u << (fine + 8u))) != 0u ? (info >> 16u) & 15u : 0u;
    if (word(meta + 5u) != 0u) {
        uint cells = word(meta + 5u) + (p.x / 8u) * 9u;
        uint owned = 1u << ((p.x & 7u) + 24u);
        if ((word(cells + band * 3u + 2u) & owned) != 0u) rank = 0u;
        for (uint editBand = 0u; editBand < 3u; ++editBand) {
            uint cell = cells + editBand * 3u;
            if ((word(cell + 2u) & owned) == 0u || cellColor(cell, meta, word(meta + 6u), editBand, p.x).a == 0.0) continue;
            uint editRank = (ranks >> (source * 8u + (editBand == 1u ? 4u : 0u))) & 15u;
            if (editRank > rank) { rank = editRank; band = editBand; }
        }
    }
    return (rank << 8u) | band;
}
uint fullAddWinner(uint row, uvec2 p) {
    uint state = word(row + 4u + p.x), ranks = word(row + 1u);
    uint mainKey = state & 255u, subKey = (state >> 8u) & 255u, band = 0u;
    for (uint source = 0u; source < 2u; ++source) {
        uint sampleKey = fullAddSample(source, p, ranks);
        if ((sampleKey >> 8u) <= 1u) continue;
        uint key = ((sampleKey >> 8u) << 3u) | source;
        if ((state & (1u << (16u + source))) != 0u && key > mainKey) mainKey = key;
        if ((state & (1u << (18u + source))) != 0u && key > subKey) { subKey = key; band = sampleKey & 255u; }
    }
    return (mainKey & 7u) < 5u && (word(row) & (1u << (8u + (mainKey & 7u)))) != 0u
        ? subKey | (band << 8u) : 0u;
}
vec4 colorAt(uvec2 p, uint source, uint band) {
    if (p.x >= word(source == 2u ? 4u : 0u) || p.y >= word(source == 2u ? 5u : 1u)) return vec4(0.0);
    if (source == 2u) {
        uint view = 8u + (2u * 352u + p.y) * 8u, alias = word(view + 4u);
        if (p.x >= (alias & 65535u) && p.x < (alias >> 16u)) {
            uint at = uint(int(word(view + 5u)) + int(p.x));
            source = word(6u); p = uvec2(at % 640u, at / 640u);
        }
    }
    uint table = word(7u), row = table != 0u && source < 2u ? word(table + p.y) : 0u;
    if (row == 0u) return rawColorAt(p, source, band);
    uint meta = 8u + (source * 352u + p.y) * 8u, edits = word(meta + 5u);
    if ((word(row) & (1u << source)) == 0u) {
        uint owner = (word(row + 3u) & (1u << source)) != 0u ? 18u : 16u;
        if ((word(row + 4u + p.x) & (1u << (owner + source))) != 0u) return rawColorAt(p, source, band);
        return band == 0u && p.x >= word(meta + 2u) && p.x < word(meta + 3u) ? argb(word(meta + 1u)) : vec4(0.0);
    }
    uint winner = fullAddWinner(row, p);
    bool keep = (winner & 255u) > 15u && (winner & 7u) == source && (winner >> 8u) == band;
    if (keep) return rawColorAt(p, source, band);
    if (edits != 0u && (word(edits + (p.x / 8u) * 9u + band * 3u + 2u) &
        (1u << ((p.x & 7u) + 24u))) != 0u) return vec4(0.0);
    return band == 0u && p.x >= word(meta + 2u) && p.x < word(meta + 3u) ? argb(word(meta + 1u)) : vec4(0.0);
}
