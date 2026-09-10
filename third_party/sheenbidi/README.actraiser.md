# SheenBidi

Unmodified `Headers/`, `Source/` and `LICENSE` from SheenBidi 3.0.0:
https://github.com/Tehreer/SheenBidi/tree/cfe430e7375a7845b679adae9d51dac6deaa8858

The upstream codeload archive for that revision has SHA-256
`9546f423820d4618c704a30c51c191b0b002aa79100e0ec34cb2cf461fd6353e`.

Apache-2.0, see `LICENSE` and the copyright notices in each source file.
The optional experimental text API is disabled. Both builds use the separate
translation units listed in `snesbuild.ini`, not `SB_CONFIG_UNITY`: the Go
builder tracks source and header mtimes, not `.c` files included by a unity
source. A regression guard compares the manifest with this pinned source tree.
No external runtime library is needed.

This dependency supplies Unicode bidi resolution and script itemization, not
glyph shaping, font selection or game layout. The SDL text backend owns the
integration; the portable rasterizer ABI does not expose SheenBidi or SDL types.
