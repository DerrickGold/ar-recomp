#include "snesrecomp/runner/scene_renderer.h"
#include "snes/ppu.h"
#include <stdlib.h>
#include <string.h>

struct SrSceneRenderer {
    Ppu *ppu;
    uint32_t pixels[7][SR_SCENE_WIDTH * SR_SCENE_HEIGHT];
    SrPpuVirtualTilemapBinding bindings[2];
    uint32_t pages[2][256 * 256];
    uint32_t view[SR_SCENE_WIDTH * SR_SCENE_HEIGHT];
};

static PpuVirtualTilemapLookupResult Lookup(const void *context, int32_t x,
                                           int32_t y, uint16_t *entry) {
    const SrPpuVirtualTilemapBinding *b = context;
    return b->lookup(b->user_data, x, y, entry);
}
static size_t Span(const void *context, int32_t x, int32_t y, int32_t step,
                   size_t capacity, const uint16_t **entries, ptrdiff_t *stride) {
    const SrPpuVirtualTilemapBinding *b = context;
    if (!b->lookup_span) return 0;
    int64_t words = 0;
    uint32_t count = b->lookup_span(b->user_data, x, y, step,
                                   (uint32_t)capacity, entries, &words);
    if (count > capacity || words < PTRDIFF_MIN || words > PTRDIFF_MAX) return 0;
    *stride = (ptrdiff_t)words;
    return count;
}
static bool Band(const void *context, int32_t x, int32_t y, uint16_t entry,
                  uint8_t *band) {
    const SrPpuVirtualTilemapBinding *b = context;
    return b->band_lookup && b->band_lookup(b->user_data, x, y, entry, band);
}
SrSceneRenderer *sr_scene_renderer_create(void) {
    SrSceneRenderer *r = calloc(1, sizeof(*r));
    if (r) {
        r->ppu = ppu_init();
        if (!r->ppu) { free(r); return NULL; }
    }
    return r;
}
void sr_scene_renderer_destroy(SrSceneRenderer *r) {
    if (r) { ppu_free(r->ppu); free(r); }
}
static bool Valid(const SrSceneFrame *f) {
    if (!f || f->struct_size != sizeof(*f) || !f->vram || !f->cgram ||
        !f->mosaic || f->extra_x > 128 || f->top + f->bottom > 128 ||
        (f->main_screen | f->sub_screen | f->native_page_mask) & ~3u || (f->cgwsel & 1)) return false;
    if (f->background_view.width && (f->background_view.width > 512 ||
        f->background_view.width > f->background_view.world_width ||
        !f->background_view.world_height || f->background_view.layer > 1)) return false;
    for (unsigned bg = 0; bg < 2; ++bg) {
        const SrSceneBackground *b = &f->backgrounds[bg];
        if (!b->rows || !b->hscroll || !b->vscroll || !b->tiles.lookup ||
            b->tiles.flags != SR_PPU_VIRTUAL_TILEMAP_INCLUDE_AUTHENTIC ||
            b->tiles.reserved || b->edits.reserved ||
            (b->edits.apron != 0 && b->edits.apron != 64) ||
            b->tiles.camera_x < -65535 || b->tiles.camera_x > 65535 ||
            b->tiles.camera_y < -65535 || b->tiles.camera_y > 65535 ||
            b->tiles.hscroll_anchor > 1023 || b->tiles.vscroll_anchor > 1023 ||
            b->fill_mode > 2 || b->capture_flags & ~31u ||
            b->clip_top > 128 || b->clip_bottom > 128) return false;
        for (unsigned y = 0; y < SR_SCENE_LINES; ++y)
            if (b->rows[y].fill < SR_PPU_BACKGROUND_FILL_TRANSPARENT ||
                b->rows[y].fill > SR_PPU_BACKGROUND_FILL_RAW_WRAP ||
                b->rows[y].motion > SR_PPU_BACKGROUND_MOTION_NORMAL_SCROLL)
                return false;
    }
    return true;
}
bool sr_scene_renderer_render(SrSceneRenderer *r, const SrSceneFrame *f,
                               SrSceneSurfaces *out) {
    if (!r || !out || !Valid(f)) return false;
    Ppu *p = r->ppu;
    const int width = 256 + f->extra_x * 2, height = 224 + f->top + f->bottom;
    const int pitch = width + 128;
    /* Clearing retained storage also erases old priority/far pixels
     * when the new frame has no content. Storage is retained between frames. */
    memset(r->pixels, 0, sizeof(r->pixels));
    memcpy(p->vram, f->vram, sizeof(p->vram));
    memcpy(p->cgram, f->cgram, sizeof(p->cgram));
    p->inidisp = 15;
    p->bgmode = 1;
    p->bgTileAdr = 0;
    p->screenEnabled[0] = f->main_screen;
    p->screenEnabled[1] = f->sub_screen;
    p->screenWindowed[0] = p->screenWindowed[1] = 0;
    p->cgwsel = f->cgwsel;
    p->cgadsub = f->cgadsub;
    p->fixedColor = f->fixed_color;
    PpuSetExtraSpace(p, f->extra_x);
    PpuSetExtraVerticalSpace(p, f->top, f->bottom);
    PpuSetWidescreenPadCapturedToBudget(p, 3);
    PpuClearOverlayBindings(p);
    PpuClearOverlayCaptures(p);
    PpuClearVirtualTilemaps(p);
    PpuClearWidescreenLayerBands(p);
    PpuBeginDrawingSized(p, (uint8_t *)r->pixels[0], pitch * 4, height,
        f->reference_renderer ? kPpuRenderFlags_ReferencePixelRenderer : 0);
    for (unsigned bg = 0; bg < 2; ++bg) {
        const SrSceneBackground *b = &f->backgrounds[bg];
        r->bindings[bg] = b->tiles;
        const PpuVirtualTilemapBinding binding = {
            .lookup = Lookup, .lookup_span = b->tiles.lookup_span ? Span : NULL,
            .band_lookup = b->tiles.band_lookup ? Band : NULL,
            .context = &r->bindings[bg], .camera_x = b->tiles.camera_x,
            .camera_y = b->tiles.camera_y, .hscroll_anchor = b->tiles.hscroll_anchor,
            .vscroll_anchor = b->tiles.vscroll_anchor,
            .flags = kPpuVirtualTilemapFlag_IncludeAuthentic,
        };
        PpuSetVirtualTilemap(p, bg, &binding);
        p->captureTiles[bg] = b->edits;
        PpuBindOverlaySurfaceSized(p, (PpuOverlaySource)bg,
            (uint8_t *)r->pixels[1 + bg * 3], pitch * 4, height);
        for (int band = 1; band < 3; ++band)
            PpuBindOverlayPrioSurface(p, (PpuOverlaySource)bg, band,
                (uint8_t *)r->pixels[1 + bg * 3 + band]);
        PpuSetOverlayCapture(p, (PpuOverlaySource)bg, -(int)f->extra_x,
            -(int)f->top, width, height, b->capture_flags);
        if (b->fill_configured)
            PpuSetOverlayTransparentFill(p, (PpuOverlaySource)bg,
                (PpuOverlayTransparentFill)b->fill_mode, b->fill_cgram);
        PpuSetWidescreenLayerExtent(p, bg, UINT16_MAX, UINT16_MAX, b->top, b->bottom);
        if (b->clip_vertical) PpuSetVerticalMarginLayerClip(p, bg, b->clip_top, b->clip_bottom);
        for (int y = 0; y < 224;) {
            const SrSceneRowPolicy row = b->rows[y];
            int end = y + 1;
            while (end < 224 && row.fill == b->rows[end].fill &&
                   row.motion == b->rows[end].motion && row.left == b->rows[end].left &&
                   row.right == b->rows[end].right) ++end;
            PpuSetWidescreenLayerBand(p, bg, y, end,
                (PpuWidescreenBandFill)row.fill, (PpuWidescreenMotion)row.motion);
            PpuSetWidescreenLayerExtentBand(p, bg, y, end, row.left, row.right);
            y = end;
        }
    }
    const SrPpuBackgroundViewRequest view = {
        .struct_size = sizeof(view), .layer = f->background_view.layer,
        .world_width = f->background_view.world_width, .world_height = f->background_view.world_height,
        .width = f->background_view.width, .height = height,
        .screen_x0 = f->background_view.screen_x0, .screen_y0 = -(int)f->top,
        .pixels = r->view, .pitch_bytes = f->background_view.width * 4u, .pixel_byte_size = sizeof(r->view),
    };
    bool view_valid = view.width != 0;
    p->backgroundPacket = f->background_packet;
    if (p->backgroundPacket)
        SrPpuBgPacket_Begin(p->backgroundPacket, (unsigned)pitch, (unsigned)height);
    ppu_runLine(p, 0);
    for (int y = -(int)f->top; y < 224 + f->bottom; ++y) {
        int row = y < 0 ? 0 : y > 223 ? 223 : y;
        p->mosaic = f->mosaic[row];
        for (unsigned bg = 0; bg < 2; ++bg) {
            p->hScroll[bg] = f->backgrounds[bg].hscroll[row];
            p->vScroll[bg] = f->backgrounds[bg].vscroll[row];
        }
        if (y < 0 || y >= 224) ppu_runMarginLine(p, y + 1);
        else ppu_runLine(p, y + 1);
        if (view_valid) view_valid = PpuRenderBackgroundViewLine(p, &view, y);
    }
    if (!view_valid && p->backgroundPacket) {
        p->backgroundPacket->words[2] &= ~4u;
        p->backgroundPacket->owned_sources &= ~4u;
    }
    *out = (SrSceneSurfaces){.backdrop = r->pixels[0], .width = width,
        .height = height, .pitch_pixels = pitch,
        .background_view = view_valid ? r->view : NULL, .view_width = (int)view.width};
    p->backgroundPacket = NULL;
    for (unsigned bg = 0; bg < 2; ++bg) {
        for (unsigned band = 0; band < 3; ++band) {
            out->bands[bg][band] = r->pixels[1 + bg * 3 + band];
            if (PpuOverlaySurfaceHasContent(p, (PpuOverlaySource)bg, band))
                out->content[bg] |= 1u << band;
        }
        out->transparent_fill[bg] = PpuOverlayTransparentFillColor(p, (PpuOverlaySource)bg);
        if ((f->native_page_mask & (1u << bg)) && !f->backgrounds[bg].edits.lookup &&
            !f->backgrounds[bg].tiles.band_lookup) {
            /* Stage the canonical page through the same provider, then ask the
             * production native-page sampler to apply CHR, palette and capture
             * color policy. Scene VRAM is reloaded before the next frame. */
            for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x) {
                uint16_t entry = 0;
                Lookup(&r->bindings[bg], x, y, &entry);
                p->vram[0x6000 + y * 32 + x] = entry;
            }
            p->bgXsc[bg] = 0x60;
            p->virtualTilemap[bg] = (PpuVirtualTilemapBinding){0};
            p->captureTiles[bg] = (SrPpuCaptureTileBinding){0};
            const SrPpuBackgroundViewRequest view = {
                .struct_size = sizeof(view), .layer = bg, .width = 256, .height = 256,
                .pixels = r->pages[bg], .pitch_bytes = 256 * 4,
                .pixel_byte_size = sizeof(r->pages[bg]), .flags = SR_PPU_BACKGROUND_VIEW_NATIVE_PAGE,
            };
            if (PpuRenderNativeBackgroundView(p, &view)) out->native_pages[bg] = r->pages[bg];
        }
    }
    return true;
}
