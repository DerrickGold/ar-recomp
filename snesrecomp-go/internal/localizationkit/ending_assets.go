package localizationkit

import (
	"bytes"
	"fmt"
)

const endingPageBytes = 0x800
const endingPageCount = 20

type endingAssets struct {
	sceneStart, sceneEnd                  int
	pageSource, fontSource, paletteSource int
	pageCompressed, fontCompressed        int
	pages, font, palette                  []byte
}

func assetSource(operands []byte) int {
	return int(operands[3]) | int(operands[4])<<8 | int(operands[5])<<16
}

// The scene's bit-0 command uses the same bounded native decompressor as
// ordinary actor composition data, but prepares twenty complete BG3 maps at
// $7E:4000. The presenter subsequently copies one map into $7F:B000. Neither
// source nor alphabet may be borrowed from the ordinary dialogue scene.
func (d *Decoder) endingAssets(entries []assetEntry) (*endingAssets, error) {
	var scene *assetEntry
	for i := range entries {
		if entries[i].mode == 8 && entries[i].submode == 1 {
			if scene != nil {
				return nil, fmt.Errorf("duplicate ending scene")
			}
			scene = &entries[i]
		}
	}
	if scene == nil {
		return nil, fmt.Errorf("ending scene missing")
	}
	a := &endingAssets{sceneStart: scene.start, sceneEnd: scene.end}
	video := false
	for _, command := range scene.commands {
		switch command.code {
		case 1:
			if a.pages != nil || len(command.operands) != 6 || !bytes.Equal(command.operands[:3], []byte{0, 0, 0}) {
				return nil, fmt.Errorf("ending page producer changed")
			}
			a.pageSource = assetSource(command.operands)
			var err error
			a.pages, a.pageCompressed, err = decompressNative(d.rom, a.pageSource)
			if err != nil {
				return nil, err
			}
		case 0x80:
			if a.font != nil || len(command.operands) != 6 || !bytes.Equal(command.operands[:3], []byte{0, 8, 0x50}) {
				return nil, fmt.Errorf("ending font producer changed")
			}
			a.fontSource = assetSource(command.operands)
			var err error
			a.font, a.fontCompressed, err = decompressNative(d.rom, a.fontSource)
			if err != nil {
				return nil, err
			}
		case 0x40:
			if a.palette != nil || len(command.operands) != 6 || !bytes.Equal(command.operands[:3], []byte{0, 16, 0}) {
				return nil, fmt.Errorf("ending palette producer changed")
			}
			a.paletteSource = assetSource(command.operands)
			var err error
			a.palette, err = d.span(a.paletteSource, 32)
			if err != nil {
				return nil, err
			}
		case 8:
			if video || !bytes.Equal(command.operands, []byte{0x2f}) {
				return nil, fmt.Errorf("ending video profile changed")
			}
			video = true
		case 2: // Music descriptors are native and independent of lettering.
		default:
			return nil, fmt.Errorf("unknown ending asset command %02x", command.code)
		}
	}
	if !video || len(a.pages) != endingPageBytes*endingPageCount || len(a.font) != 4096 || len(a.palette) != 32 {
		return nil, fmt.Errorf("ending asset extents changed")
	}
	for offset := 0; offset < len(a.pages); offset += 2 {
		if word(a.pages[offset:])&0x3ff >= len(a.font)/16 {
			return nil, fmt.Errorf("ending page tile outside resident alphabet")
		}
	}
	return a, nil
}

func (a *endingAssets) evidence() IRObject {
	return IRObject{
		"scene_file_offset": fileOffset(a.sceneStart), "scene_byte_count": a.sceneEnd - a.sceneStart,
		"page_source_file_offset": fileOffset(a.pageSource), "page_decoded_byte_count": len(a.pages),
		"page_decoded_sha256": rawSHA256(a.pages), "page_compressed_cursor_bytes": a.pageCompressed,
		"font_source_file_offset": fileOffset(a.fontSource), "font_decoded_byte_count": len(a.font),
		"font_decoded_sha256": rawSHA256(a.font), "font_compressed_cursor_bytes": a.fontCompressed,
		"font_format": "snes_2bpp_8x8", "font_vram_word_address": "$5000",
		"palette_source_file_offset": fileOffset(a.paletteSource), "palette_byte_count": len(a.palette),
		"palette_sha256": rawSHA256(a.palette), "palette_cgram_index": 0,
		"page_destination_wram": "$7E:4000", "page_format": "snes_bg3_32x32_le16",
		"redistribution": "rom_derived_do_not_distribute",
	}
}
