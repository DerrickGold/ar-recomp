package localization

import (
	"bytes"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/png"
)

// NativeGraphicsFiles returns a detached, local-only extraction bundle. No
// files are written, no fonts are installed, and no artwork is redistributable
// merely because it was extracted. Indexed PNGs retain the native 2bpp indices;
// scene palettes are separate, rather than baking one arbitrary tint into a
// purported universal font. The manifest records unmapped glyphs explicitly.
func (d *Decoder) NativeGraphicsFiles() (map[string][]byte, error) {
	if d == nil {
		return nil, fmt.Errorf("nil localization decoder")
	}
	entries, err := d.assetScript()
	if err != nil {
		return nil, err
	}
	font, err := d.dialogFontAsset(entries)
	if err != nil {
		return nil, err
	}
	ending, err := d.endingAssets(entries)
	if err != nil {
		return nil, err
	}
	dialog := font.decoded
	files := map[string][]byte{"dialogue.2bpp": dialog, "credits.2bpp": ending.font,
		"credits.tilemaps": ending.pages, "credits.palette.bgr555": bytes.Clone(ending.palette)}
	for _, font := range []struct {
		name string
		data []byte
	}{{"dialogue", dialog}, {"credits", ending.font}} {
		atlas, err := native2bppAtlas(font.data)
		if err != nil {
			return nil, err
		}
		files[font.name+".png"] = atlas
	}
	for page := 0; page < endingPageCount; page++ {
		picture, err := native2bppPage(ending.pages[page*endingPageBytes:(page+1)*endingPageBytes], ending.font, ending.palette)
		if err != nil {
			return nil, err
		}
		files[fmt.Sprintf("credits/page-%02d.png", page)] = picture
	}
	glyphs := make([]IRObject, 256)
	for code := 0; code < 256; code++ {
		glyph := IRObject{"tile": code, "classification": "unmapped_native_tile"}
		if value, ok := d.profile.Glyphs[byte(code)]; ok {
			glyph["unicode"] = value
			glyph["classification"] = "mapped_text_glyph"
		}
		if icon, ok := d.profile.Icons[byte(code)]; ok {
			glyph["icon"] = icon.Value
			glyph["part_index"], glyph["part_count"] = icon.PartIndex, icon.PartCount
			glyph["classification"] = "native_icon"
		}
		// A few native atlas slots are punctuation in prose and icon parts
		// only at a specific fixed-composer source. Preserve both meanings.
		contexts := []IRObject{}
		for _, context := range []struct {
			source *int
			codes  [2]byte
			icon   string
		}{
			{d.profile.PopulationSource, d.profile.PopulationCodes, "status.population"},
			{d.speedSource, d.profile.SpeedCodes, "ui.speed_direction"},
		} {
			if context.source == nil {
				continue
			}
			for part, value := range context.codes {
				if int(value) == code {
					contexts = append(contexts, IRObject{"consumer": "fixed_composer", "source_pc24": pcString(offsetPC(*context.source)),
						"icon": context.icon, "part_index": part, "part_count": 2})
				}
			}
		}
		if len(contexts) > 0 {
			glyph["consumer_overrides"] = contexts
		}
		if d.profile.Encoding == "direct-glyph" && (code == 0xde || code == 0xdf) {
			glyph["classification"] = "combining_native_diacritic"
			glyph["combining_unicode"] = []string{"\u3099", "\u309a"}[code-0xde]
		}
		glyphs[code] = glyph
	}
	palettes := []IRObject{}
	for _, entry := range entries {
		for _, command := range entry.commands {
			if command.code&0x80 != 0 || command.code&0x40 == 0 {
				continue
			}
			// $B330 uploads source colours [first,end), starting at the
			// third operand's CGRAM index. Keep the exact slice and owner.
			first, end, dest := int(command.operands[0]), int(command.operands[1]), int(command.operands[2])
			if end <= first || dest+end-first > 256 {
				return nil, fmt.Errorf("invalid scene palette bounds")
			}
			if dest >= 32 {
				continue
			} // Not a 2bpp BG3 palette.
			source := assetSource(command.operands) + first*2
			data, err := d.span(source, (end-first)*2)
			if err != nil {
				return nil, err
			}
			name := fmt.Sprintf("palettes/%02x-%02x-%02x.bgr555", entry.mode, entry.submode, dest)
			if _, exists := files[name]; exists {
				return nil, fmt.Errorf("duplicate scene palette destination")
			}
			files[name] = bytes.Clone(data)
			palettes = append(palettes, IRObject{"file": name, "mode": entry.mode, "submode": entry.submode,
				"cgram_index": dest, "color_count": end - first, "source_file_offset": fileOffset(source), "sha256": rawSHA256(data)})
		}
	}
	manifest := IRObject{"format": "actraiser-native-graphics-reference", "version": 1,
		"release_id": d.profile.ID, "rom_sha256": d.profile.SHA256,
		"redistribution": "rom_derived_do_not_distribute", "tile_width": 8, "tile_height": 8,
		"atlas_columns": 16, "pixel_format": "snes_2bpp", "png_palette": "index_preview_not_retail_colors",
		"dialogue_font": font.evidence(), "dialogue_glyphs": glyphs, "scene_palettes": palettes,
		"credits": ending.evidence(), "credits_page_count": endingPageCount,
		"credits_alphabet": "distinct_native_tile_compositions_not_dialogue_codepoints"}
	files["manifest.json"], err = json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return nil, err
	}
	files["manifest.json"] = append(files["manifest.json"], '\n')
	return files, nil
}

func native2bppIndex(data []byte, tile, x, y int) byte {
	base, bit := tile*16+y*2, uint(7-x)
	return data[base]>>bit&1 | (data[base+1]>>bit&1)<<1
}

func native2bppAtlas(data []byte) ([]byte, error) {
	if len(data) != 4096 {
		return nil, fmt.Errorf("expected 256 2bpp font tiles")
	}
	palette := color.Palette{color.NRGBA{0, 0, 0, 0}, color.NRGBA{85, 85, 85, 255}, color.NRGBA{170, 170, 170, 255}, color.NRGBA{255, 255, 255, 255}}
	picture := image.NewPaletted(image.Rect(0, 0, 128, 128), palette)
	for tile := 0; tile < 256; tile++ {
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				picture.SetColorIndex(tile%16*8+x, tile/16*8+y, native2bppIndex(data, tile, x, y))
			}
		}
	}
	var out bytes.Buffer
	if err := png.Encode(&out, picture); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

func native2bppPage(page, font, palette []byte) ([]byte, error) {
	if len(page) != endingPageBytes || len(font) != 4096 || len(palette) != 32 {
		return nil, fmt.Errorf("invalid credit page extent")
	}
	picture := image.NewNRGBA(image.Rect(0, 0, 256, 224))
	for cell := 0; cell < 32*28; cell++ {
		descriptor := word(page[cell*2:])
		tile, pal := descriptor&0x3ff, (descriptor>>10&7)*4
		if tile >= 256 || pal+3 >= len(palette)/2 {
			return nil, fmt.Errorf("credit page leaves font or palette")
		}
		for y := 0; y < 8; y++ {
			for x := 0; x < 8; x++ {
				sx, sy := x, y
				if descriptor&0x4000 != 0 {
					sx = 7 - x
				}
				if descriptor&0x8000 != 0 {
					sy = 7 - y
				}
				index := native2bppIndex(font, tile, sx, sy)
				if index == 0 {
					continue
				}
				v := word(palette[(pal+int(index))*2:])
				// Same bit replication as game-side ExpandColor5, not a
				// multiply/divide approximation that differs at midtones.
				expand := func(v int) byte { v &= 31; return byte(v<<3 | v>>2) }
				c := color.NRGBA{expand(v), expand(v >> 5), expand(v >> 10), 255}
				picture.SetNRGBA(cell%32*8+x, cell/32*8+y, c)
			}
		}
	}
	var out bytes.Buffer
	if err := png.Encode(&out, picture); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}
