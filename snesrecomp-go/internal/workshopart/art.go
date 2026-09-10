// Package workshopart decodes a deliberately small, verified US graphics
// profile for decorative builder scenes. It never reads live game state.
package workshopart

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
)

const (
	Version = 3
	// Same exact headerless retail identity as the native-US language profile.
	SourceSHA256  = "b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0"
	AtlasWidth    = 960
	AtlasHeight   = 1728
	cellSize      = 96
	bossCellSize  = 160
	bossRow       = 672
	backgroundRow = 1472
)

// Frame keeps the composition's record origin; tight-cropping each pose would
// erase native per-part offsets and introduce jitter between wing frames.
type Frame struct {
	ID     string `json:"id"`
	X      int    `json:"x"`
	Y      int    `json:"y"`
	Width  int    `json:"width"`
	Height int    `json:"height"`
}

type Catalog struct {
	Version     int         `json:"version"`
	ROM         string      `json:"romSHA256"`
	ImageSHA256 string      `json:"imageSHA256"`
	Frames      []Frame     `json:"frames"`
	Animations  []Animation `json:"animations"`
}

type Assets struct {
	Catalog Catalog
	PNG     []byte
}

type composition struct {
	id                                        string
	address, graphics, palette, width, height int
}

func compositions() []composition {
	// $05:82EF map $00/$09: raw OBJ upload $02CE7F -> VRAM $4000,
	// palette $0E4093 -> CGRAM $80. $01:EC40/$EC6E are the two 3x3 grids.
	rows := []composition{
		{"palace.idle.0", 0xec40, 0x2ce7f, 0xe4093, 48, 48},
		{"palace.idle.1", 0xec6e, 0x2ce7f, 0xe4093, 48, 48},
	}
	// $05:8038 and $05:8266: raw $068000 -> VRAM $2000, 16 KiB;
	// $0E3C93 -> CGRAM $80, 256 bytes. Same atlas for towns and Sky Palace.
	for _, group := range []struct {
		id        string
		addresses []int
	}{
		{"angel.back", []int{0xa627, 0xa63c, 0xa651, 0xa666}},
		{"angel.front", []int{0xa67b, 0xa690, 0xa6a5, 0xa6ba}},
		{"angel.left", []int{0xa6cf, 0xa6d5, 0xa6ea, 0xa6f0}},
		{"angel.right", []int{0xa705, 0xa70b, 0xa720, 0xa726}},
	} {
		for i, address := range group.addresses {
			rows = append(rows, composition{fmt.Sprintf("%s.%d", group.id, i), address, 0x68000, 0xe3c93, 16, 18})
		}
	}
	return rows
}

func Extract(data []byte) (*Assets, error) {
	if len(data) != 1<<20 || fmt.Sprintf("%x", sha256.Sum256(data)) != SourceSHA256 {
		return nil, fmt.Errorf("workshop scenery requires the clean, headerless US ROM")
	}
	return extract(data)
}

func extract(data []byte) (*Assets, error) {
	atlas := image.NewNRGBA(image.Rect(0, 0, AtlasWidth, AtlasHeight))
	result := &Assets{Catalog: Catalog{Version: Version, ROM: SourceSHA256}}
	for i, row := range compositions() {
		x, y := i%10*cellSize, i/10*cellSize
		if err := drawComposition(atlas, data, row, x, y); err != nil {
			return nil, err
		}
		result.Catalog.Frames = append(result.Catalog.Frames, Frame{row.id, x, y, row.width, row.height})
	}
	if err := extractMaster(atlas, data, result); err != nil {
		return nil, err
	}
	if err := extractFillmore(atlas, data, result); err != nil {
		return nil, err
	}
	var encoded bytes.Buffer
	if err := png.Encode(&encoded, atlas); err != nil {
		return nil, err
	}
	result.PNG = encoded.Bytes()
	result.Catalog.ImageSHA256 = fmt.Sprintf("%x", sha256.Sum256(result.PNG))
	return result, nil
}

func partOffset(value byte) int {
	if value > 0x80 {
		return int(value) - 256
	}
	return int(value) // Native $80 is +128, not -128.
}

func pixel4bpp(data []byte, tile, x, y int) byte {
	var index byte
	for plane := 0; plane < 4; plane++ {
		index |= ((data[tile*32+(plane/2)*16+y*2+plane%2] >> (7 - x)) & 1) << plane
	}
	return index
}

func snesColor(c uint16) color.NRGBA {
	expand := func(n uint16) uint8 { return uint8((n&31)<<3 | (n&31)>>2) }
	return color.NRGBA{expand(c), expand(c >> 5), expand(c >> 10), 255}
}

// Rasterize a bank-local OBJ part. Large tiles wrap the low tile nibble
// independently; BG tilemaps don't use this addressing rule.
func drawObjectPart(dst *image.NRGBA, characters, palette []byte, x, y, size int, attr uint16) error {
	for py := 0; py < size; py++ {
		for px := 0; px < size; px++ {
			sx, sy := px, py
			if attr&0x4000 != 0 {
				sx = size - 1 - sx
			}
			if attr&0x8000 != 0 {
				sy = size - 1 - sy
			}
			tile := (((int(attr&255)>>4)+(sy>>3))<<4 | ((int(attr&15) + (sx >> 3)) & 15)) & 255
			if tile*32+32 > len(characters) {
				return fmt.Errorf("composition leaves reviewed character range")
			}
			index := pixel4bpp(characters, tile, sx&7, sy&7)
			if index == 0 {
				continue
			}
			at := int((attr>>9)&7)*32 + int(index)*2
			if at+2 > len(palette) {
				return fmt.Errorf("composition leaves reviewed palette range")
			}
			dst.SetNRGBA(x+px, y+py, snesColor(binary.LittleEndian.Uint16(palette[at:])))
		}
	}
	return nil
}

func drawComposition(dst *image.NRGBA, data []byte, row composition, ox, oy int) error {
	if row.address < 0 || row.address >= len(data) {
		return fmt.Errorf("composition outside ROM")
	}
	count := int(data[row.address])
	if count < 1 || count > 64 || row.address+1+count*5 > len(data) {
		return fmt.Errorf("invalid composition size")
	}
	// Both reviewed sheets have all tiles used by these compositions in bank 0.
	if row.graphics < 0 || row.graphics+0x1000 > len(data) || row.palette < 0 || row.palette+256 > len(data) {
		return fmt.Errorf("graphics outside ROM")
	}
	for i := 0; i < count; i++ {
		part := data[row.address+1+i*5 : row.address+6+i*5]
		size := 8
		if part[0]&1 != 0 {
			size = 16
		}
		x, y := partOffset(part[1]), partOffset(part[2])
		attr := binary.LittleEndian.Uint16(part[3:])
		if part[0]&^byte(1) != 0 || attr&0x100 != 0 || x < 0 || y < 0 || x+size > row.width || y+size > row.height {
			return fmt.Errorf("unreviewed composition bounds or OBJ bank at $01:%04X", row.address)
		}
		if err := drawObjectPart(dst, data[row.graphics:row.graphics+0x1000], data[row.palette:row.palette+256], ox+x, oy+y, size, attr); err != nil {
			return err
		}
	}
	return nil
}
