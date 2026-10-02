#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D u_texture;

// Immutable probe packet, see tools/ppu_gpu/packet.h. Input is RGBA8 bytes,
// never filtered artwork. texelFetch and integer math preserve exact bits.
uint word(uint at) {
    uvec4 b = uvec4(round(texelFetch(u_texture, ivec2(at & 255u, at >> 8u), 0) * 255.0));
    return b.r | (b.g << 8u) | (b.b << 16u) | (b.a << 24u);
}
vec4 color(uint argb) {
    return vec4((argb >> 16u) & 255u, (argb >> 8u) & 255u, argb & 255u, argb >> 24u) / 255.0;
}
uint vram(uint address) {
    uint packed = word(5648u + ((address & 32767u) >> 1u));
    return (packed >> ((address & 1u) * 16u)) & 65535u;
}
void main() {
    uint height = word(1u);
    uvec2 p = uvec2(v_uv * vec2(word(0u), height * 6u));
    uint band_index = p.y / height;
    uint bg = band_index / 3u, band = band_index % 3u;
    uint desc = 16u + (bg * 352u + p.y % height) * 8u;
    o_color = vec4(0.0);
    if (p.x < 64u || p.x >= word(2u) + 64u) return;
    uint x = p.x - 64u;
    if (band == 0u) o_color = color(word(desc + 4u));
    if (x < word(desc + 2u) || x >= word(desc + 3u)) return;
    uint fine = x + word(desc);
    uint tile = word(word(desc + 5u) + (fine >> 3u));
    if ((tile & 65536u) == 0u || ((tile >> 17u) & 3u) != band) return;
    uint tx = fine & 7u, ty = word(desc + 1u);
    if ((tile & 16384u) != 0u) tx = 7u - tx;
    if ((tile & 32768u) != 0u) ty = 7u - ty;
    uint address = (tile & 1023u) * 16u + ty;
    uint lo = vram(address), hi = vram(address + 8u), shift = 7u - tx;
    uint pixel = ((lo >> shift) & 1u) | (((lo >> (shift + 8u)) & 1u) << 1u) |
                 (((hi >> shift) & 1u) << 2u) | (((hi >> (shift + 8u)) & 1u) << 3u);
    if (pixel != 0u) o_color = color(word(22032u + bg * 256u + ((tile >> 10u) & 7u) * 16u + pixel));
}
