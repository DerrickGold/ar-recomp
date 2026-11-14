SMAA reference shader and lookup tables from [iryoku/smaa](https://github.com/iryoku/smaa/tree/71c806a838bdd7d517df19192a20f0c61b3ca29d).
See LICENSE.txt for terms. The GLSL wrappers in src/shaders use the SMAA 1x
reference passes. The shader allows the wrappers to override the scene
samples in edge detection and neighborhood blending to keep them inside the
active scene rectangle. The luma edge pass can write zeros for pixels without
an edge, allowing a full-target draw to skip clearing the edge buffer.
