#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D packet;
layout(std140, set = 3, binding = 0) uniform Context { vec4 plane; } settings;
uint word(uint index) {
    uvec4 bytes = uvec4(round(texelFetch(packet, ivec2(index % 256u, index / 256u), 0) * 255.0));
    return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}
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
void main() {
    uvec2 p = uvec2(v_uv * settings.plane.zw);
    o_color = vec4(0.0);
    uint source = uint(settings.plane.x), band = uint(settings.plane.y);
    if (p.x >= word(source == 2u ? 4u : 0u) || p.y >= word(source == 2u ? 5u : 1u)) return;
    uint row = source * 352u + p.y, meta = 8u + row * 8u;
    if (source == 2u) {
        uint alias = word(meta + 4u);
        if (p.x >= (alias & 65535u) && p.x < (alias >> 16u)) {
            uint at = uint(int(word(meta + 5u)) + int(p.x));
            source = word(6u); p = uvec2(at % 640u, at / 640u);
            row = source * 352u + p.y; meta = 8u + row * 8u;
        }
    }
    if (word(meta) == 0u) return;
    uint pixels = word(meta + 7u), colors = word(meta + 6u);
    if (pixels == 0u) {
        if (band == 0u && p.x >= word(meta + 2u) && p.x < word(meta + 3u)) o_color = argb(word(meta + 1u));
        return;
    }
    if (word(meta) == 4u && word(meta + 5u) != 0u) {
        uint edit = word(meta + 5u) + (p.x / 8u) * 9u + band * 3u;
        if ((word(edit + 2u) & (1u << ((p.x & 7u) + 24u))) != 0u) {
            o_color = cellColor(edit, meta, colors, band, p.x);
            return;
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
            o_color = argb(word(colors + (info & 7u) * 16u + value));
        else if (band == 0u && p.x >= word(meta + 2u) && p.x < word(meta + 3u)) o_color = argb(word(meta + 1u));
        return;
    }
    if (word(meta) == 2u) {
        uint cell = pixels + (p.x / 8u) * (source == 2u ? 3u : 9u) + band * 3u;
        o_color = cellColor(cell, meta, colors, band, p.x);
        return;
    }
    uint value = word(pixels + p.x);
    uint code = (value >> (band * 9u)) & 511u;
    if (code == 257u) o_color = vec4(0.0, 0.0, 0.0, 1.0);
    else if (code == 258u) o_color = argb(word(meta + 1u));
    else if (code != 0u && code <= 256u) o_color = argb(word(colors + code - 1u));
    else if (code == 0u && band == 0u && (value & 0x80000000u) == 0u &&
        p.x >= word(meta + 2u) && p.x < word(meta + 3u)) o_color = argb(word(meta + 1u));
}
