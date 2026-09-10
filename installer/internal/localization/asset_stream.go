package localization

import (
	"bytes"
	"fmt"
	"math/bits"
	"slices"

	"github.com/DerrickGold/ar-recomp/installer/internal/quintet"
)

// Fonts, extraction regressions and workshop art use the same native codec.
func decompressNative(rom []byte, offset int) ([]byte, int, error) {
	return quintet.Decompress(rom, offset)
}

const assetScriptBase = 0x28000
const assetScriptLimit = assetScriptBase + 0x7ff0

var assetOperandCounts = [8]int{6, 5, 3, 1, 4, 7, 6, 6}

type assetCommand struct {
	code     byte
	operands []byte
}
type assetEntry struct {
	mode, submode byte
	start, end    int
	commands      []assetCommand
}

func (d *Decoder) assetScript() ([]assetEntry, error) {
	entries := []assetEntry{}
	cursor := assetScriptBase + 3
	read := func(count int) ([]byte, error) {
		if cursor > assetScriptLimit-count {
			return nil, fmt.Errorf("asset script crosses its bounded table")
		}
		data, err := d.span(cursor, count)
		if err == nil {
			cursor += count
		}
		return data, err
	}
	for cursor < assetScriptLimit {
		start := cursor
		selectors, err := read(2)
		if err != nil {
			return nil, err
		}
		entry := assetEntry{mode: selectors[0], submode: selectors[1], start: start, commands: []assetCommand{}}
		for {
			raw, err := read(1)
			if err != nil {
				return nil, err
			}
			code := raw[0]
			if code == 0 {
				break
			}
			operands, err := read(assetOperandCounts[bits.Len8(code)-1])
			if err != nil {
				return nil, err
			}
			entry.commands = append(entry.commands, assetCommand{code, operands})
		}
		entry.end = cursor
		entries = append(entries, entry)
		// The final boss (07/08) is not the end of this table: 08/01
		// owns the credits palette, alternate font and all twenty page maps.
		if entry.mode == 8 && entry.submode == 1 {
			return entries, nil
		}
	}
	return nil, fmt.Errorf("asset script end marker missing")
}

type nativeFontAsset struct {
	source, compressed int
	decoded            []byte
	references         []IRObject
}

func (d *Decoder) dialogFontAsset(entries []assetEntry) (*nativeFontAsset, error) {
	references := []IRObject{}
	sources := []int{}
	for _, entry := range entries {
		if entry.mode == 8 { // Credits use a distinct, double-height alphabet.
			continue
		}
		for _, command := range entry.commands {
			if command.code&0x80 == 0 || !bytes.Equal(command.operands[:3], []byte{0, 8, 0x50}) {
				continue
			}
			source := int(command.operands[3]) | int(command.operands[4])<<8 | int(command.operands[5])<<16
			if !slices.Contains(sources, source) {
				sources = append(sources, source)
			}
			references = append(references, IRObject{"mode": fmt.Sprintf("$%02X", entry.mode), "submode": fmt.Sprintf("$%02X", entry.submode), "source_file_offset": fileOffset(source)})
		}
	}
	if len(sources) != 1 {
		return nil, fmt.Errorf("expected one dialog font source, found %d", len(sources))
	}
	source := sources[0]
	decoded, compressed, err := decompressNative(d.rom, source)
	if err != nil {
		return nil, err
	}
	if len(decoded) != 0x1000 {
		return nil, fmt.Errorf("dialog font decoded to %d bytes instead of 4096", len(decoded))
	}
	return &nativeFontAsset{source, compressed, decoded, references}, nil
}

func (d *Decoder) dialogFont(entries []assetEntry) (IRObject, error) {
	asset, err := d.dialogFontAsset(entries)
	if err != nil {
		return nil, err
	}
	return asset.evidence(), nil
}

func (asset *nativeFontAsset) evidence() IRObject {
	source, compressed, decoded, references := asset.source, asset.compressed, asset.decoded, asset.references
	unique := map[[16]byte]bool{}
	nonblank := 0
	for offset := 0; offset < len(decoded); offset += 16 {
		tile := [16]byte(decoded[offset : offset+16])
		unique[tile] = true
		if tile != [16]byte{} {
			nonblank++
		}
	}
	return IRObject{"status": "asset_script_source_verified", "classification": "regional_graphical_text_font", "source_file_offset": fileOffset(source), "source_pc24": cursorAddress(source),
		"script_reference_count": len(references), "script_references": references, "decoded_byte_count": len(decoded), "compressed_byte_count": compressed,
		"decoded_sha256": rawSHA256(decoded), "tile_count": len(decoded) / 16, "nonblank_tile_count": nonblank, "unique_tile_count": len(unique), "redistribution": "rom_derived_do_not_distribute"}
}
