# Original application icons

These assets were generated for this project using the built-in image-generation
tool, then revised to 16-bit-style pixel art. No game ROM graphics, ripped
sprites or retail artwork were used as inputs. The Builder temple and game sword
have transparent backgrounds; the temple uses three visible corner columns.

## Sources and exports

- `originals/builder-16bit.png`: final floating-temple master.
- `originals/game-16bit.png`: final sword master.
- `assets/{builder,game}/`: checked-in PNG sizes (16–1024), multi-size
  Windows ICO, and macOS ICNS including Retina representations.
- `game.rc`: the same game icon for direct Windows CMake builds.

To refresh the exports after replacing a master, from this directory run:

```sh
go run ./cmd/export
```

The exporter only resizes/encodes the approved artwork, using nearest-neighbor
sampling. Players do not need this tool or an image-generation service:
the Go binaries embed the exported files.

## Packaging

- macOS: `Contents/Resources/<Application>.icns`, referenced by
  `CFBundleIconFile` before ad-hoc signing.
- Windows: actual PE icon resources, ID 3 for the Wails Builder and ID 1 for
  the game. Preserve existing manifest/version resources. Embed before appending
  the Builder payload and before signing; never patch a signed release.
- AppImage/AppDir: a real 256px PNG `.DirIcon`, a matching root
  `<Application>.png` and `Icon=<Application>` desktop entry, plus hicolor PNGs.
  Both native appimagetool and macOS cross-host SquashFS paths validate these.
- Linux Builder window: the same temple PNG is passed to Wails.

The [AppDir specification](https://docs.appimage.org/reference/appdir.html)
requires `.DirIcon`; explicitly staging it avoids relying on appimagetool to
synthesize it (the cross-host path does not run appimagetool).

Tests validate alpha, dimensions, ICO/ICNS containers, extracted AppImage bytes,
macOS bundle icon references, and cross-built Windows amd64/arm64 PE resources.
No Windows/Linux GUI launch is implied by the cross-host packaging tests.

## Generation prompt set

Mode: built-in image generation (not the fallback CLI). The smooth first drafts
were style references only. Each icon was rendered in a separate call.

### Builder initial design

Use case: stylized-concept. Asset type: original desktop application icon for a game builder/workshop, a square 1024x1024 PNG with genuine transparent background. Subject: a small cream-marble classical temple with a simple triangular pediment and chunky columns, on a compact faceted floating stone island with muted sage-green top and a few broad front steps. Inspired only by a hand-drawn software UI's generic floating-temple idea, NOT by any existing game's artwork. Style: beautifully restrained faceted illustration, clean strong edges, substantial forms that remain legible at 32 pixels, subtle dimensional shading, cream and warm gold highlights, slate-blue shadows, muted sage stone surface. Composition: one centered standalone icon, balanced three-quarter view, temple and floating island together occupy about 84% of the square, 8% clear padding on every edge. No scene, no background tile, no clouds, no characters, no particles, no lettering or logos. Original composition only; do not reproduce any retail game graphics, sprites, symbols, or assets. Truly transparent outside the silhouette, no baked checkerboard, no white rectangle, no cast shadow outside the silhouette.

### Game initial design

Use case: stylized-concept. Asset type: original desktop application icon for a fantasy game, a square 1024x1024 PNG with genuine transparent background. Subject: ONE simple bold sword, diagonally ascending from lower-left pommel to upper-right point. Broad straight double-edged blade in pale steel with a few clean slate-blue facets, a substantial warm-gold crossguard and pommel, a short dark slate-blue grip. Style: beautifully restrained faceted illustration, clean strong edges, subtle dimensional shading, very simple geometry and powerful readable silhouette at 32 pixels; designed as a companion to a cream-marble floating-temple workshop icon. Composition: centered standalone sword, balanced in the square, occupies about 84% of the square with 8% clear padding on every edge. No decorative insignia, no runes, no gems, no hands, no shield, no scabbard, no background tile, no scene, no particles, no lettering, no logo. Entirely original design; do not reproduce an existing video game's sword or any retail artwork. Truly transparent outside the silhouette, no baked checkerboard, no white rectangle, no cast shadow outside the silhouette.

### Builder pixel-art revision

Use case: style-transfer. Edit target: the attached original application icon. Change its rendering to authentic early-1990s 16-bit console pixel art, suitable as a desktop application icon. Reinterpret it as a carefully hand-pixeled 64x64 sprite enlarged cleanly with nearest-neighbor to a square 1024x1024 canvas: clearly visible consistent square pixels, stepped pixel diagonals, dark navy pixel outlines, flat clusters of color, a tight palette of about 16 colors, 3-tone ramps and sparing deliberate dithering. No smooth vector edges, no 3D rendering, no painting, no blur, no anti-aliasing, no gradient lighting. Preserve the subject, overall pose and orientation, and essential palette of the reference. Simplify small details for readability at 32px. Keep the complete silhouette centered with about 8% transparent padding. Genuine transparent background outside the sprite, no checkerboard, no background tile, no text, no watermark, no new objects. Must remain original art, not a reproduction or extraction of any existing game's graphics. Subject to preserve: one cream-and-warm-gold classical temple with columns and front steps on a mossy green floating rocky island. Strong blue-purple rock shadows and cream highlights, warm 16-bit fantasy-adventure atmosphere. Make the temple and floating island equally clear in silhouette.

### Game pixel-art revision

Use case: style-transfer. Edit target: the attached original application icon. Change its rendering to authentic early-1990s 16-bit console pixel art, suitable as a desktop application icon. Reinterpret it as a carefully hand-pixeled 64x64 sprite enlarged cleanly with nearest-neighbor to a square 1024x1024 canvas: clearly visible consistent square pixels, stepped pixel diagonals, dark navy pixel outlines, flat clusters of color, a tight palette of about 16 colors, 3-tone ramps and sparing deliberate dithering. No smooth vector edges, no 3D rendering, no painting, no blur, no anti-aliasing, no gradient lighting. Preserve the subject, overall pose and orientation, and essential palette of the reference. Simplify small details for readability at 32px. Keep the complete silhouette centered with about 8% transparent padding. Genuine transparent background outside the sprite, no checkerboard, no background tile, no text, no watermark, no new objects. Must remain original art, not a reproduction or extraction of any existing game's graphics. Subject to preserve: one broad silver sword pointing from the lower left toward the upper right, golden crossguard and pommel, dark blue grip. Use steel-blue blade shadows and bright ivory highlights, chunky 16-bit fantasy-adventure inventory-sprite styling.

### Final Builder perspective correction

Use case: precise-object-edit.
Edit the attached pixel-art temple icon with one precise architectural correction. It has three visible pillars but its rightmost pillar is incorrectly inset too close to the doorway.
MOVE THE RIGHTMOST PILLAR TO THE OUTER FRONT-RIGHT CORNER of the building/platform, directly beneath the right end of the FRONT triangular pediment/roof beam. In this 1280x1280 input, that pillar's center is presently around x=795: relocate the entire column including gold capital, shaft, gold base and square plinth to center around x=916. It must replace the blank cream strip that currently occupies that outer corner. Its top should connect to the beam at that corner and its base should stand on the corner plinth just to the right of the front stairs. Fill its old location with the recessed front wall/doorway background, NOT another pillar. There must still be exactly THREE visible pillars in total: the two existing left-side pillars unchanged, and ONE right-side pillar now AT THE OUTER FRONT-RIGHT CORNER. No pillar immediately beside the doorway, no visible rear-right pillar, no fourth pillar.
The front door should read as centered in the open front bay between the front-left and the newly positioned front-right corner pillars; adjust only the recessed doorway/wall as necessary for this perspective correction.
Preserve the original roof, island, front stairs, 16-bit square-pixel style, dark navy outline, cream/gold/green/blue palette, framing and transparent background. No glow, haze, blur, checkerboard, text, or new decorations.
