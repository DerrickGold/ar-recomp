#version 450
// The packet layout and color semantics match ppu_bg_capture.frag.glsl.
// Read integer words directly instead of unpacking them from an RGBA texture.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer Packet { uint words[]; } packet;
layout(rgba8, set = 1, binding = 0) writeonly uniform image2D lowBand;
layout(rgba8, set = 1, binding = 1) writeonly uniform image2D highBand;
layout(rgba8, set = 1, binding = 2) writeonly uniform image2D farBand;
layout(std140, set = 2, binding = 0) uniform Context { uvec4 plane; } settings;
uint word(uint index) { return packet.words[index]; }
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
vec4 colorAt(uvec2 p, uint source, uint band) {
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
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (any(greaterThanEqual(p, settings.plane.zw))) return;
    uint source = settings.plane.x, bands = settings.plane.y;
    if ((bands & 1u) != 0u) imageStore(lowBand, ivec2(p), colorAt(p, source, 0u));
    if ((bands & 2u) != 0u) imageStore(highBand, ivec2(p), colorAt(p, source, 1u));
    if ((bands & 4u) != 0u) imageStore(farBand, ivec2(p), colorAt(p, source, 2u));
}
