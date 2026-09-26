#include "settings_overlay/settings_overlay_internal.h"
#include "settings_overlay/settings_overlay.h"
#include "settings_overlay/settings_overlay_palette.h"

#include <string.h>

#include "app/settings.h"
#include "constants.h"
#include "localization/interface_text.h"
#include "localization/unicode_grapheme.h"
#include "render/ui_text_renderer.h"

/* Shared overlay drawing: text shaping/cache, artwork lifecycle, layout math,
 * frames, glyphs and small widgets. Menus and debug panels supply content and
 * navigation; this owner never reads their selection or modal state. */
enum {
  kMinimumLayoutWidth = 464,
  kMinimumLayoutHeight = 208,
  kMaximumScalePercent = 800,
};

static ArRenderDevice *s_render_device;

/* ARGB() is defined in settings_overlay_internal.h (shared with the panel). */

const uint32_t kPanel = ARGB(255, 0, 0, 0);
static const uint32_t kFrameDark = ARGB(255, 45, 63, 78);
static const uint32_t kFrameLight = ARGB(255, 164, 196, 219);
const uint32_t kHighlight = ARGB(255, 22, 57, 83);

/* Colors sampled from the game's own Sky Palace menu CGRAM (runs/.../cgram),
 * so the overlay reads as ActRaiser rather than drifting into a custom scheme:
 *  - the steel blue of the dialog frame carries all passive chrome (titles,
 *    tab strip, rules, scrollbars),
 *  - the menu's selection yellow marks wherever the cursor currently is (the
 *    highlighted section, row, and active tab), exactly like the game's
 *    yellow-bordered selected item slot,
 *  - the warm gold is the game's own highlight text color (CGRAM pal0 #6),
 *    used for the blinking cursor and the restart marker. */
const uint32_t kSteelBlue = ARGB(255, 164, 196, 219);
const uint32_t kSteelDim = ARGB(255, 74, 104, 130);
const uint32_t kSelectYellow = ARGB(255, 255, 230, 0);
const uint32_t kGameGold = ARGB(255, 255, 180, 65);
const uint32_t kMutedText = ARGB(255, 120, 140, 158);
static ArUiTextRenderer s_ui_text;

/* DebugTextStyle is defined in settings_overlay_internal.h (shared with the
 * panel, which passes these roles to DrawDebugTextN). */

/* The scene inspector is a host debugger, not an in-game dialog. Keep its
 * information hierarchy legible independently of the ROM font palette. */
static const uint32_t kDebugTextColors[kDebugTextStyle_Count] = {
    ARGB(255, 218, 229, 238), /* ordinary punctuation/text */
    ARGB(255, 92, 196, 255),  /* field names */
    ARGB(255, 255, 207, 92),  /* addresses and numeric values */
    ARGB(255, 105, 232, 157), /* BG/OBJ targets and source policies */
    ARGB(255, 255, 145, 76),  /* missing candidates and warnings */
    ARGB(255, 119, 139, 154), /* controls and explanatory notes */
};

ArRenderDevice *SettingsOverlayWidgets_RenderDevice(void) { return s_render_device; }

bool SettingsOverlayWidgets_HasTextBackend(void) { return ArUiTextRenderer_IsReady(&s_ui_text); }

bool SettingsOverlayWidgets_Init(ArRenderDevice *device, const uint8_t *rom, size_t size) {
  s_render_device = ArRenderDevice_IsReady(device) ? device : NULL;
  if (!s_render_device) return true;
  return SettingsOverlayArtwork_Init(s_render_device, rom, size);
}

bool SettingsOverlayWidgets_ReloadTextures(const uint8_t *rom, size_t size) {
  if (!s_render_device) return true;
  ArUiTextRenderer_ClearTextures(&s_ui_text);
  SettingsOverlayPalette_ReleaseTexture();
  return SettingsOverlayArtwork_Reload(rom, size);
}

void SettingsOverlayWidgets_Destroy(void) {
  ArUiTextRenderer_Destroy(&s_ui_text);
  SettingsOverlayArtwork_Destroy();
  SettingsOverlayPalette_ReleaseTexture();
  s_render_device = NULL;
}

bool SettingsOverlay_SetTextBackend(const ArTextBackend *backend, const ArTextBackendConfig *fonts,
                                    char *error, size_t error_capacity) {
  return ArUiTextRenderer_Init(&s_ui_text, s_render_device, backend, fonts, error, error_capacity);
}

static ArUiLocale InterfaceLocale(void) {
  /* A port without a ready text backend must keep its menu readable. The
   * persisted choice survives so a later successful font setup can adopt it. */
  return ArUiTextRenderer_IsReady(&s_ui_text) ? (ArUiLocale)g_settings.interface_language
                                              : kArUiLocale_English;
}

ArUiLocale SettingsOverlay_InterfaceLocale(void) { return InterfaceLocale(); }

/* Layout math stays integer (it also feeds the public panel-rect API), so
 * convert only at the portable draw call. */
ArRenderRectF ToRenderRect(ArRenderRectI r) {
  return (ArRenderRectF){(float)r.x, (float)r.y, (float)r.w, (float)r.h};
}

static ArRenderColorF RenderColor(uint32_t color) {
  return (ArRenderColorF){
      .r = (float)((color >> 16) & 0xff) / 255.0f,
      .g = (float)((color >> 8) & 0xff) / 255.0f,
      .b = (float)(color & 0xff) / 255.0f,
      .a = (float)(color >> 24) / 255.0f,
  };
}

static int ScalePosition(int position, int scale_percent) {
  return (position * scale_percent + kPercentScale / 2) / kPercentScale;
}

ArRenderRectI LogicalRect(const MenuLayout *layout, int x, int y, int width, int height) {
  int x0 = layout->origin_x + ScalePosition(x, layout->scale_percent);
  int y0 = layout->origin_y + ScalePosition(y, layout->scale_percent);
  int x1 = layout->origin_x + ScalePosition(x + width, layout->scale_percent);
  int y1 = layout->origin_y + ScalePosition(y + height, layout->scale_percent);
  return (ArRenderRectI){x0, y0, x1 - x0, y1 - y0};
}

static bool FillPixelRectChecked(int x, int y, int width, int height, uint32_t color) {
  if (width <= 0 || height <= 0) return true;
  const ArRenderRectF rect = {
      (float)x,
      (float)y,
      (float)width,
      (float)height,
  };
  return ArRenderDevice_DrawSolidRect(s_render_device, &rect, RenderColor(color),
                                      kArRenderBlendMode_Alpha);
}

static void FillPixelRect(int x, int y, int width, int height, uint32_t color) {
  if (width <= 0 || height <= 0) return;
  const ArRenderRectF rect = {
      (float)x,
      (float)y,
      (float)width,
      (float)height,
  };
  (void)ArRenderDevice_DrawSolidRect(s_render_device, &rect, RenderColor(color),
                                     kArRenderBlendMode_Alpha);
}

void FillLogicalRect(const MenuLayout *layout, int x, int y, int width, int height,
                     uint32_t color) {
  ArRenderRectI rect = LogicalRect(layout, x, y, width, height);
  FillPixelRect(rect.x, rect.y, rect.w, rect.h, color);
}

static bool DrawDialogTileChecked(const MenuLayout *layout, int atlas_column, int atlas_row, int x,
                                  int y) {
  ArRenderRectI source = {
      atlas_column * kGlyphSize,
      atlas_row * kGlyphSize,
      kGlyphSize,
      kGlyphSize,
  };
  const ArRenderRectF destination = ToRenderRect(LogicalRect(layout, x, y, kGlyphSize, kGlyphSize));
  const ArRenderRectF source_f = ToRenderRect(source);
  return ArRenderDevice_DrawTexture(s_render_device, SettingsOverlayArtwork_Get()->dialog_frame,
                                    &source_f, &destination);
}

static bool DrawDialogPanelChecked(const MenuLayout *layout, int x, int y, int width, int height) {
  if (width < 16 || height < 16) return false;
  if (!ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->dialog_frame)) {
    const ArRenderRectI outer = LogicalRect(layout, x, y, width, height);
    const ArRenderRectI middle = LogicalRect(layout, x + 2, y + 2, width - 4, height - 4);
    const ArRenderRectI inner = LogicalRect(layout, x + 4, y + 4, width - 8, height - 8);
    return FillPixelRectChecked(outer.x, outer.y, outer.w, outer.h, kFrameDark) &&
           FillPixelRectChecked(middle.x, middle.y, middle.w, middle.h, kFrameLight) &&
           FillPixelRectChecked(inner.x, inner.y, inner.w, inner.h, kPanel);
  }

  const ArRenderRectI inner = LogicalRect(layout, x + kGlyphSize, y + kGlyphSize,
                                          width - kGlyphSize * 2, height - kGlyphSize * 2);
  if (!FillPixelRectChecked(inner.x, inner.y, inner.w, inner.h, kPanel)) return false;
  for (int tile_x = x + kGlyphSize; tile_x < x + width - kGlyphSize; tile_x += kGlyphSize) {
    if (!DrawDialogTileChecked(layout, 1, 0, tile_x, y) ||
        !DrawDialogTileChecked(layout, 1, 2, tile_x, y + height - kGlyphSize))
      return false;
  }
  for (int tile_y = y + kGlyphSize; tile_y < y + height - kGlyphSize; tile_y += kGlyphSize) {
    if (!DrawDialogTileChecked(layout, 0, 1, x, tile_y) ||
        !DrawDialogTileChecked(layout, 2, 1, x + width - kGlyphSize, tile_y))
      return false;
  }
  return DrawDialogTileChecked(layout, 0, 0, x, y) &&
         DrawDialogTileChecked(layout, 2, 0, x + width - kGlyphSize, y) &&
         DrawDialogTileChecked(layout, 0, 2, x, y + height - kGlyphSize) &&
         DrawDialogTileChecked(layout, 2, 2, x + width - kGlyphSize, y + height - kGlyphSize);
}

void DrawDialogPanel(const MenuLayout *layout, int x, int y, int width, int height) {
  (void)DrawDialogPanelChecked(layout, x, y, width, height);
}

bool SettingsOverlay_DrawGameFrame(ArRenderRectI rect, int scale) {
  if (!s_render_device || scale <= 0) return false;
  const int tile_size = kGlyphSize * scale;
  if (rect.w <= 0 || rect.h <= 0 || rect.w % tile_size != 0 || rect.h % tile_size != 0)
    return false;
  const MenuLayout layout = {
      .output_width = rect.w,
      .output_height = rect.h,
      .scale_percent = scale * kPercentScale,
      .logical_width = rect.w / scale,
      .logical_height = rect.h / scale,
      .origin_x = rect.x,
      .origin_y = rect.y,
  };
  return DrawDialogPanelChecked(&layout, 0, 0, layout.logical_width, layout.logical_height);
}

void DrawGlyph(const MenuLayout *layout, int x, int y, unsigned char ch, TextStyle style) {
  if (ch == ' ') return;
  if (!SettingsOverlayArtwork_Get()->glyph_defined[ch]) ch = '?';
  if (!SettingsOverlayArtwork_Get()->glyph_defined[ch]) return;
  const ArRenderTexture texture = SettingsOverlayArtwork_Get()->fonts[style];
  if (!ArRenderTexture_IsValid(texture)) return;
  const ArRenderRectF source = {
      (float)((ch & 15) * kGlyphSize),
      (float)((ch >> 4) * kGlyphSize),
      (float)kGlyphSize,
      (float)kGlyphSize,
  };
  const ArRenderRectF destination = ToRenderRect(LogicalRect(layout, x, y, kGlyphSize, kGlyphSize));
  (void)ArRenderDevice_DrawTexture(s_render_device, texture, &source, &destination);
}

/* ── Character cells ───────────────────────────────────────────────────────
 *
 * Package names come from whoever authored the pack, in whatever script, so
 * the overlay is handed UTF-8. The compatibility font atlas is ASCII-only,
 * but the counting must be right with either renderer: one cell
 * per extended grapheme cluster, not per byte. A name with an accent then
 * occupies the cells a reader would count, right alignment lands where it
 * should, and truncation can never cut a character in half.
 *
 * The fallback path draws a cluster that is a single ASCII byte; anything else draws
 * the replacement, so a precomposed and a decomposed accent look the same
 * rather than one silently losing its mark. Nothing is transliterated. The
 * package ID in the same row is ASCII by construction and stays readable
 * while a name has no glyphs. The host-injected font path instead shapes the
 * complete bounded UTF-8 run, preserving accents and contextual joining. */
static size_t OverlayNextCell(const char *text, size_t bytes, size_t offset, unsigned char *glyph) {
  uint32_t first = 0;
  size_t next = 0;
  if (!ArUnicodeGrapheme_Next(text, bytes, offset, &first, &next) || next <= offset) {
    /* Malformed bytes cost one cell each and never stop the row: a package
     * name with one bad byte must still be readable and selectable, not
     * vanish. Deciding a cluster boundary reads the following scalar, so a
     * bad byte fails its predecessor too -- recovering a byte at a time is
     * what keeps that local. */
    *glyph = (unsigned char)'?';
    return offset + 1u;
  }
  *glyph = next - offset == 1u && first >= 0x20u && first < 0x80u ? (unsigned char)first
                                                                  : (unsigned char)'?';
  return next;
}

/* Cells `text` occupies, at most `maximum`. */
static int OverlayCellCount(const char *text, int maximum) {
  if (!text || maximum <= 0) return 0;
  const size_t bytes = strlen(text);
  int cells = 0;
  for (size_t offset = 0; offset < bytes && cells < maximum;) {
    unsigned char glyph = 0;
    const size_t next = OverlayNextCell(text, bytes, offset, &glyph);
    if (!next) break;
    offset = next;
    ++cells;
  }
  return cells;
}

/* Keep English's authentic atlases unchanged. Any non-ASCII run (including a
 * package name) goes through the independently owned interface font stack.
 * Grapheme boundaries retain the existing cell-budget/truncation contract;
 * shaping is whole-run, never a collection of isolated Unicode code points. */
static bool MakeUnicodeTextRun(const MenuLayout *layout, int x, int y, const char *text,
                               int max_chars, int cell_width, ArUiTextAlignment alignment,
                               int style, uint32_t tint, ArUiTextRun *out_run) {
  if (!text || max_chars <= 0 || !ArUiTextRenderer_IsReady(&s_ui_text)) return false;
  const size_t bytes = strlen(text);
  /* ASCII remains the common path. Don't repeat the grapheme walk for it. */
  bool candidate = InterfaceLocale() != kArUiLocale_English;
  for (size_t i = 0; i < bytes; ++i)
    if ((unsigned char)text[i] >= 0x80) {
      candidate = true;
      break;
    }
  if (!candidate) return false;
  size_t end = 0;
  int cells = 0;
  bool non_ascii = InterfaceLocale() != kArUiLocale_English;
  while (end < bytes && cells < max_chars) {
    unsigned char glyph;
    const size_t next = OverlayNextCell(text, bytes, end, &glyph);
    if (next > kArInterfaceTextMaximumBytes) return false;
    for (size_t i = end; i < next; ++i)
      if ((unsigned char)text[i] >= 0x80) non_ascii = true;
    end = next;
    ++cells;
  }
  if (!non_ascii) return false;
  ArUiTextRun run = {
      .struct_size = sizeof(run),
      .abi_version = AR_UI_TEXT_RUN_ABI_VERSION,
      .utf8 = text,
      .utf8_bytes = end,
      .bounds = LogicalRect(layout, x, y, cells * cell_width, kGlyphSize),
      .alignment = alignment,
      .tint = RenderColor(tint),
      .language_bcp47 = ArUiCatalog_LocaleTag(InterfaceLocale()),
  };
  if (style >= 0 && style < kTextStyle_Count) {
    run.style_id = kArTextStyle_RetailPaletteBands;
    run.band_rgb = kTextPalettes[style][2] & UINT32_C(0xffffff);
    run.body_rgb = kTextPalettes[style][3] & UINT32_C(0xffffff);
    run.shadow_rgb = kTextPalettes[style][1] & UINT32_C(0xffffff);
    run.shadow_enabled = true;
  }
  *out_run = run;
  return true;
}

static bool DrawUnicodeText(const MenuLayout *layout, int x, int y, const char *text, int max_chars,
                            int cell_width, ArUiTextAlignment alignment, int style, uint32_t tint) {
  ArUiTextRun run;
  return MakeUnicodeTextRun(layout, x, y, text, max_chars, cell_width, alignment, style, tint,
                            &run) &&
         ArUiTextRenderer_Draw(&s_ui_text, &run);
}

void DrawTextN(const MenuLayout *layout, int x, int y, const char *text, int max_chars,
               TextStyle style) {
  if (!text || max_chars <= 0) return;
  if (DrawUnicodeText(layout, x, y, text, max_chars, kGlyphSize, kArUiTextAlignment_Left, style,
                      UINT32_C(0xffffffff)))
    return;
  const size_t bytes = strlen(text);
  int cell = 0;
  for (size_t offset = 0; offset < bytes && cell < max_chars; ++cell) {
    unsigned char glyph = 0;
    const size_t next = OverlayNextCell(text, bytes, offset, &glyph);
    if (!next) break;
    DrawGlyph(layout, x + cell * kGlyphSize, y, glyph, style);
    offset = next;
  }
}

/* ── The game font at output-pixel coordinates ─────────────────────────────
 *
 * For a nested fullscreen mode -- the manual reader -- which is not laid out on
 * the menu's logical grid and so cannot go through DrawTextN's MenuLayout. Same
 * atlas, same glyphs, same colors; only the coordinate space differs. */

int SettingsOverlay_GameTextWidth(const char *text, int scale) {
  if (!text || scale <= 0) return 0;
  if (scale <= kMaximumScalePercent / kPercentScale) {
    const MenuLayout layout = {.scale_percent = scale * kPercentScale};
    ArUiTextRun run;
    int width;
    if (MakeUnicodeTextRun(&layout, 0, 0, text, INT32_MAX, kGlyphSize, kArUiTextAlignment_Left,
                           kText_Normal, UINT32_C(0xffffffff), &run) &&
        ArUiTextRenderer_Measure(&s_ui_text, &run, &width, NULL))
      return width;
  }
  return OverlayCellCount(text, INT32_MAX) * kGlyphSize * scale;
}

void SettingsOverlay_DrawGameText(int x, int y, int scale, uint8_t alpha, const char *text) {
  enum {
    kFontAtlasCellsPerAxis = 16,
    kGameTextGlyphBatchCapacity = 64,
    kVerticesPerGlyph = 4,
    kIndicesPerGlyph = 6,
  };
  if (!text || scale <= 0 || alpha == 0) return;
  if (scale <= kMaximumScalePercent / kPercentScale) {
    const MenuLayout layout = {
        .scale_percent = scale * kPercentScale, .origin_x = x, .origin_y = y};
    if (DrawUnicodeText(&layout, 0, 0, text, INT32_MAX, kGlyphSize, kArUiTextAlignment_Left,
                        kText_Normal, ARGB(alpha, 255, 255, 255)))
      return;
  }
  const ArRenderTexture texture = SettingsOverlayArtwork_Get()->fonts[kText_Normal];
  if (!s_render_device || !ArRenderTexture_IsValid(texture)) return;

  static int32_t indices[kGameTextGlyphBatchCapacity * kIndicesPerGlyph];
  static bool indices_initialized;
  if (!indices_initialized) {
    for (int glyph = 0; glyph < kGameTextGlyphBatchCapacity; glyph++) {
      const int vertex = glyph * kVerticesPerGlyph;
      const int at = glyph * kIndicesPerGlyph;
      indices[at + 0] = vertex + 0;
      indices[at + 1] = vertex + 1;
      indices[at + 2] = vertex + 2;
      indices[at + 3] = vertex + 0;
      indices[at + 4] = vertex + 2;
      indices[at + 5] = vertex + 3;
    }
    indices_initialized = true;
  }

  ArRenderVertex2D vertices[kGameTextGlyphBatchCapacity * kVerticesPerGlyph];
  int glyph_count = 0;
  const float uv_cell = 1.0f / (float)kFontAtlasCellsPerAxis;
  const float glyph_pixels = (float)(kGlyphSize * scale);
  const ArRenderColorF white = {
      1.0f,
      1.0f,
      1.0f,
      (float)alpha / 255.0f,
  };
  const ArRenderDrawState draw_state = {
      .flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Alpha,
  };
  const size_t text_bytes = strlen(text);
  int cell = -1;
  for (size_t offset = 0; offset < text_bytes;) {
    unsigned char ch = 0;
    const size_t next = OverlayNextCell(text, text_bytes, offset, &ch);
    if (!next) break;
    offset = next;
    ++cell;
    if (ch == ' ') continue;
    if (!SettingsOverlayArtwork_Get()->glyph_defined[ch]) ch = '?';
    if (!SettingsOverlayArtwork_Get()->glyph_defined[ch]) continue;

    if (glyph_count == kGameTextGlyphBatchCapacity) {
      (void)ArRenderDevice_DrawGeometryWithState(s_render_device, texture, vertices,
                                                 glyph_count * kVerticesPerGlyph, indices,
                                                 glyph_count * kIndicesPerGlyph, &draw_state);
      glyph_count = 0;
    }

    const float x0 = (float)(x + cell * kGlyphSize * scale);
    const float y0 = (float)y;
    const float x1 = x0 + glyph_pixels;
    const float y1 = y0 + glyph_pixels;
    const float u0 = (float)(ch & 15) * uv_cell;
    const float v0 = (float)(ch >> 4) * uv_cell;
    const float u1 = u0 + uv_cell;
    const float v1 = v0 + uv_cell;
    ArRenderVertex2D *quad = &vertices[glyph_count * kVerticesPerGlyph];
    quad[0] = (ArRenderVertex2D){{x0, y0}, white, {u0, v0}};
    quad[1] = (ArRenderVertex2D){{x1, y0}, white, {u1, v0}};
    quad[2] = (ArRenderVertex2D){{x1, y1}, white, {u1, v1}};
    quad[3] = (ArRenderVertex2D){{x0, y1}, white, {u0, v1}};
    glyph_count++;
  }
  if (glyph_count > 0)
    (void)ArRenderDevice_DrawGeometryWithState(s_render_device, texture, vertices,
                                               glyph_count * kVerticesPerGlyph, indices,
                                               glyph_count * kIndicesPerGlyph, &draw_state);
}

int CappedTextLength(const char *text, int max_chars) { return OverlayCellCount(text, max_chars); }

void DrawTextRight(const MenuLayout *layout, int right, int y, const char *text, int max_chars,
                   TextStyle style) {
  int length = CappedTextLength(text, max_chars);
  if (DrawUnicodeText(layout, right - length * kGlyphSize, y, text, length, kGlyphSize,
                      kArUiTextAlignment_Right, style, UINT32_C(0xffffffff)))
    return;
  DrawTextN(layout, right - length * kGlyphSize, y, text, length, style);
}

/* ── Small font ─────────────────────────────────────────────────────────
 * The 6x8 atlas built from kFallbackFont, drawn with a free-form color via
 * SetTextureColorMod. It is monochrome (no baked outline/shadow), so unlike
 * the 8x8 ROM font it can take an arbitrary color at zero cost — which is
 * what lets the tab bar, description panel, and hint line pick up each
 * section's accent instead of everything being the same white.
 *
 * Three quarters the width of the ROM font per character, so the description
 * panel fits roughly a third more text per line at a size that still reads
 * comfortably at couch distance. */
static void DrawSmallGlyph(const MenuLayout *layout, int x, int y, unsigned char ch,
                           uint32_t color) {
  if (ch == ' ' || !ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->debug_font)) return;
  const ArRenderRectF source = {
      (float)((ch & 15) * kDebugGlyphWidth),
      (float)((ch >> 4) * kDebugGlyphHeight),
      (float)kDebugGlyphWidth,
      (float)kDebugGlyphHeight,
  };
  const ArRenderRectF destination =
      ToRenderRect(LogicalRect(layout, x, y, kDebugGlyphWidth, kDebugGlyphHeight));
  (void)ArRenderDevice_DrawTextureTinted(s_render_device, SettingsOverlayArtwork_Get()->debug_font,
                                         &source, &destination, RenderColor(color));
}

void DrawSmallTextN(const MenuLayout *layout, int x, int y, const char *text, int max_chars,
                    uint32_t color) {
  if (!text || max_chars <= 0 || !ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->debug_font))
    return;
  if (DrawUnicodeText(layout, x, y, text, max_chars, kDebugGlyphWidth, kArUiTextAlignment_Left, -1,
                      color))
    return;
  const size_t bytes = strlen(text);
  int cell = 0;
  for (size_t offset = 0; offset < bytes && cell < max_chars; ++cell) {
    unsigned char glyph = 0;
    const size_t next = OverlayNextCell(text, bytes, offset, &glyph);
    if (!next) break;
    DrawSmallGlyph(layout, x + cell * kDebugGlyphWidth, y, glyph, color);
    offset = next;
  }
}

void DrawSmallText(const MenuLayout *layout, int x, int y, const char *text, uint32_t color) {
  DrawSmallTextN(layout, x, y, text, 512, color);
}

int SmallTextWidth(const char *text) {
  return OverlayCellCount(text, INT32_MAX) * kDebugGlyphWidth;
}

/* Icons are authored at 16x16 but drawn at whatever `size` the caller wants;
 * nearest-neighbour keeps integer multiples crisp. `selected` picks the
 * colored game palette (the highlighted slot) over the grey one, and `alpha`
 * fades an unselected, un-focused nav row so it reads as recessive. */
void DrawOverlayIcon(const MenuLayout *layout, int x, int y, int size, SettingsOverlayIcon icon,
                     bool selected, int alpha) {
  if (!ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->icons) || icon < 0 ||
      icon >= kOverlayIcon_Count)
    return;
  const ArRenderRectF source = {
      (float)(icon * kIconSize),
      selected ? (float)kIconSize : 0.0f,
      (float)kIconSize,
      (float)kIconSize,
  };
  const ArRenderRectF destination = ToRenderRect(LogicalRect(layout, x, y, size, size));
  (void)ArRenderDevice_DrawTextureTinted(s_render_device, SettingsOverlayArtwork_Get()->icons,
                                         &source, &destination,
                                         (ArRenderColorF){1.0f, 1.0f, 1.0f, (float)alpha / 255.0f});
}

/* A slim track with a proportional thumb, drawn in the panel's inner gutter.
 * Replaces the pair of blinking ^ / v glyphs the lists used to carry: those
 * cost a full 8px text cell out of the value column and only said "there is
 * more", never how much more or where you are in it. Draws nothing when the
 * whole list already fits. */
void DrawScrollBar(const MenuLayout *layout, int x, int y, int height, int total, int visible,
                   int top, uint32_t accent) {
  if (total <= visible || visible <= 0 || height <= 0) return;
  FillLogicalRect(layout, x, y, 3, height, ARGB(90, 60, 84, 106));
  int thumb = height * visible / total;
  if (thumb < 6) thumb = 6;
  if (thumb > height) thumb = height;
  int span = total - visible;
  int offset = span > 0 ? (height - thumb) * top / span : 0;
  FillLogicalRect(layout, x, y + offset, 3, thumb, accent);
}

static void DrawDebugGlyph(const MenuLayout *layout, int x, int y, unsigned char ch,
                           DebugTextStyle style) {
  if (ch == ' ' || !ArRenderTexture_IsValid(SettingsOverlayArtwork_Get()->debug_font)) return;
  if (ch >= 128 || !SettingsOverlayArtwork_HasDebugGlyph(ch)) {
    if (ch >= 'a' && ch <= 'z')
      ch = (unsigned char)(ch - 'a' + 'A');
    else
      ch = '?';
  }
  const ArRenderRectF source = {
      (float)((ch & 15) * kDebugGlyphWidth),
      (float)((ch >> 4) * kDebugGlyphHeight),
      (float)kDebugGlyphWidth,
      (float)kDebugGlyphHeight,
  };
  const ArRenderRectF destination =
      ToRenderRect(LogicalRect(layout, x, y, kDebugGlyphWidth, kDebugGlyphHeight));
  (void)ArRenderDevice_DrawTextureTinted(s_render_device, SettingsOverlayArtwork_Get()->debug_font,
                                         &source, &destination,
                                         RenderColor(kDebugTextColors[style]));
}

void DrawDebugTextN(const MenuLayout *layout, int x, int y, const char *text, int length,
                    DebugTextStyle style) {
  if (!text || length <= 0) return;
  for (int i = 0; i < length; i++)
    DrawDebugGlyph(layout, x + i * kDebugGlyphWidth, y, (unsigned char)text[i], style);
}

static bool DebugWordEquals(const char *word, int length, const char *expected) {
  return (int)strlen(expected) == length && !strncmp(word, expected, (size_t)length);
}

static bool DebugWordIsTarget(const char *word, int length) {
  static const char *const targets[] = {
      "BG",        "OBJ",   "M7",     "MODE",   "MODE7", "CENTER",   "WIDE",
      "MARGIN",    "CLAMP", "MIRROR", "REPEAT", "GAP",   "HUD-LEFT", "HUD-CENTER",
      "HUD-RIGHT", "ON",    "OFF",    "TRUE",   "FALSE",
  };
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
    if (DebugWordEquals(word, length, targets[i])) return true;
  return false;
}

static bool DebugLineStartsWith(const char *text, int length, const char *prefix) {
  size_t prefix_length = strlen(prefix);
  return prefix_length <= (size_t)length && !strncmp(text, prefix, prefix_length);
}

void DrawDebugHighlightedLine(const MenuLayout *layout, int x, int y, const char *text,
                              int length) {
  if (DebugLineStartsWith(text, length, "LEFT CLICK")) {
    DrawDebugTextN(layout, x, y, text, length, kDebugText_Dim);
    return;
  }
  if (DebugLineStartsWith(text, length, "NO VISIBLE") || DebugLineStartsWith(text, length, "...")) {
    DrawDebugTextN(layout, x, y, text, length, kDebugText_Warning);
    return;
  }
  if (DebugLineStartsWith(text, length, "CANDIDATES") ||
      DebugLineStartsWith(text, length, "HASHES")) {
    DrawDebugTextN(layout, x, y, text, length, kDebugText_Dim);
    return;
  }

  for (int at = 0; at < length;) {
    unsigned char ch = (unsigned char)text[at];
    DebugTextStyle style = kDebugText_Normal;
    int end = at + 1;
    if (ch == '$') {
      while (end < length &&
             ((text[end] >= '0' && text[end] <= '9') || (text[end] >= 'A' && text[end] <= 'F') ||
              (text[end] >= 'a' && text[end] <= 'f')))
        end++;
      style = kDebugText_Value;
    } else if (ch >= '0' && ch <= '9') {
      while (end < length && text[end] >= '0' && text[end] <= '9')
        end++;
      style = kDebugText_Value;
    } else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_') {
      while (end < length &&
             ((text[end] >= 'A' && text[end] <= 'Z') || (text[end] >= 'a' && text[end] <= 'z') ||
              text[end] == '_' || text[end] == '-'))
        end++;
      style = DebugWordIsTarget(text + at, end - at) ? kDebugText_Target : kDebugText_Label;
    }
    DrawDebugTextN(layout, x + at * kDebugGlyphWidth, y, text + at, end - at, style);
    at = end;
  }
}

/* Word-wrapped small-font paragraph. Returns the number of lines drawn so a
 * caller can place something underneath. The description panel uses this: at
 * 6px per character it fits the longest tooltips in the table without the
 * truncation the 8px menu font used to force. */
int DrawWrappedSmallText(const MenuLayout *layout, int x, int y, const char *text, int max_chars,
                         int max_lines, uint32_t color) {
  const char *cursor = text;
  if (max_chars > 127) max_chars = 127;
  if (max_chars <= 0 || !cursor) return 0;
  size_t remaining = strlen(cursor);
  int line = 0;
  for (; line < max_lines && remaining; line++) {
    ArInterfaceTextLine slice;
    char buffer[kArInterfaceTextMaximumBytes + 1];
    if (!ArInterfaceText_WrapLine(cursor, remaining, (size_t)max_chars, sizeof(buffer) - 1, &slice))
      break;
    memcpy(buffer, cursor, slice.bytes);
    buffer[slice.bytes] = 0;
    DrawSmallText(layout, x, y + line * kSmallLineHeight, buffer, color);
    cursor += slice.consumed;
    remaining -= slice.consumed;
  }
  return line;
}

/* Compact preview never grows the footer. An ellipsis points to Details. */
void DrawSmallTextPreview(const MenuLayout *layout, int x, int y, const char *text, int columns,
                          int lines, uint32_t color) {
  if (!text || columns < 4) return;
  if (columns > 127) columns = 127;
  size_t remaining = strlen(text);
  for (int line = 0; line < lines && remaining; ++line) {
    ArInterfaceTextLine slice;
    char buffer[kArInterfaceTextMaximumBytes + 1];
    if (!ArInterfaceText_WrapLine(text, remaining, columns, sizeof(buffer) - 4, &slice) ||
        !slice.consumed)
      break;
    const bool truncated = line == lines - 1 && slice.consumed < remaining;
    if (truncated &&
        !ArInterfaceText_WrapLine(text, remaining, columns - 3, sizeof(buffer) - 4, &slice))
      break;
    memcpy(buffer, text, slice.bytes);
    strcpy(buffer + slice.bytes, truncated ? "..." : "");
    DrawSmallText(layout, x, y + line * kSmallLineHeight, buffer, color);
    text += slice.consumed;
    remaining -= slice.consumed;
  }
}

void AddMenuHint(MenuHints *hints, const char *key, const char *label) {
  if (!key || !*key || hints->count >= kMenuHintMax) return;
  hints->items[hints->count].key = key;
  hints->items[hints->count++].label = label;
}

static void DrawHintText(const MenuLayout *layout, int x, int y, const char *text, int width,
                         uint32_t color) {
  const int columns = width / kDebugGlyphWidth;
  if (SmallTextWidth(text) <= width || columns < 4)
    DrawSmallTextN(layout, x, y, text, columns, color);
  else
    DrawSmallTextPreview(layout, x, y, text, columns, 1, color);
}

void DrawMenuHints(const MenuLayout *layout, int x, int y, int width, const MenuHints *hints,
                   uint32_t key_color, uint32_t label_color) {
  if (!hints->count || width <= 0) return;
  enum { kKeyGap = 5, kHintGap = 11 };
  int sizes[kMenuHintMax], total = 0;
  for (int i = 0; i < hints->count; ++i) {
    sizes[i] =
        SmallTextWidth(hints->items[i].key) + kKeyGap + SmallTextWidth(hints->items[i].label);
    total += sizes[i] + (i ? kHintGap : 0);
  }
  /* Preserve every command. Prefer one line; otherwise balance two fixed
   * lines. Unusually long remapped keys are clipped inside their own cell,
   * never allowed to overlap another hint or the frame. */
  int split = hints->count;
  if (total > width) {
    int left = 0, best = total;
    for (int i = 1; i < hints->count; ++i) {
      left += sizes[i - 1] + (i > 1 ? kHintGap : 0);
      int right = total - left - kHintGap;
      int widest = left > right ? left : right;
      if (widest < best) {
        split = i;
        best = widest;
      }
    }
  }
  for (int line = 0; line < (split == hints->count ? 1 : 2); ++line) {
    const int begin = line ? split : 0, end = line ? hints->count : split;
    int content = 0;
    for (int i = begin; i < end; ++i)
      content += sizes[i];
    const int available = width - (end - begin - 1) * kHintGap;
    int cursor = x;
    for (int i = begin; i < end; ++i) {
      const int cell = content > available ? sizes[i] * available / content : sizes[i];
      int key_width = SmallTextWidth(hints->items[i].key);
      int label_minimum = SmallTextWidth(hints->items[i].label);
      if (label_minimum > 4 * kDebugGlyphWidth) label_minimum = 4 * kDebugGlyphWidth;
      if (key_width > cell - kKeyGap - label_minimum) key_width = cell - kKeyGap - label_minimum;
      if (key_width < kDebugGlyphWidth) key_width = kDebugGlyphWidth;
      if (key_width > cell) key_width = cell;
      DrawHintText(layout, cursor, y + line * kSmallLineHeight, hints->items[i].key, key_width,
                   key_color);
      DrawHintText(layout, cursor + key_width + kKeyGap, y + line * kSmallLineHeight,
                   hints->items[i].label, cell - key_width - kKeyGap, label_color);
      cursor += cell + kHintGap;
    }
  }
}

int SnappedFitScale(int output_width, int output_height) {
  int fit_x = output_width * kPercentScale / kMinimumLayoutWidth;
  int fit_y = output_height * kPercentScale / kMinimumLayoutHeight;
  int fit = fit_x < fit_y ? fit_x : fit_y;
  fit = (fit / kScaleStepPercent) * kScaleStepPercent;
  if (fit < kMinimumScalePercent) fit = kMinimumScalePercent;
  if (fit > kMaximumScalePercent) fit = kMaximumScalePercent;
  return fit;
}

MenuLayout BuildLayoutAtScale(int output_width, int output_height, int scale) {
  int logical_width = output_width * kPercentScale / scale;
  int logical_height = output_height * kPercentScale / scale;
  int used_width = ScalePosition(logical_width, scale);
  int used_height = ScalePosition(logical_height, scale);
  return (MenuLayout){
      output_width,
      output_height,
      scale,
      logical_width,
      logical_height,
      (output_width - used_width) / 2,
      (output_height - used_height) / 2,
  };
}

static const char *DecisionUi(const char *key) {
  return ArUiCatalog_Text(SettingsOverlay_InterfaceLocale(), key, key);
}

void DrawDecision(const MenuLayout *layout, const OverlayDecision *decision) {
  const int width = (layout->logical_width < 496 ? layout->logical_width - 32 : 464) / 8 * 8;
  const int height = (layout->logical_height < 288 ? layout->logical_height - 32 : 256) / 8 * 8;
  const int x = (layout->logical_width - width) / 2;
  const int y = (layout->logical_height - height) / 2;
  FillLogicalRect(layout, 0, 0, layout->logical_width, layout->logical_height, ARGB(180, 0, 0, 0));
  DrawDialogPanel(layout, x, y, width, height);
  DrawWrappedSmallText(layout, x + 16, y + 12, DecisionUi(decision->title),
                       (width - 32) / kDebugGlyphWidth, 2, kGameGold);
  DrawWrappedSmallText(
      layout, x + 16, y + 36, decision->body_text ? decision->body : DecisionUi(decision->body),
      (width - 32) / kDebugGlyphWidth, (height - 100) / kSmallLineHeight, kSteelBlue);
  const int choices_y = y + height - 48;
  for (unsigned n = 0; n < (decision->notice?1u:2u); ++n) {
    const bool selected = decision->accept_selected == (n == 0);
    if (selected) FillLogicalRect(layout, x + 8, choices_y + n * 16 - 2, width - 16, 14, kPanel);
    DrawSmallText(layout, x + 16, choices_y + n * 16, selected ? ">" : " ", kSelectYellow);
    DrawSmallText(layout, x + 30, choices_y + n * 16,
                  DecisionUi(n ? "overlay.decision.cancel" : decision->accept),
                  selected ? kSelectYellow : kSteelBlue);
  }
}
