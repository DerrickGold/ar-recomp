package workshopart

import (
	"encoding/binary"
	"fmt"
	"image"

	"github.com/DerrickGold/ar-recomp/installer/internal/quintet"
)

// Exact US $05:8000 entry 01/01, not a second general asset-script VM.
// Keep this bounded selection in agreement with ActionRoomScene_LookupTile.
func unpack(data []byte, at, size int) ([]byte, error) {
	b, _, err := quintet.Decompress(data, at)
	if err != nil {
		return nil, err
	}
	if len(b) != size {
		return nil, fmt.Errorf("unexpected scenery asset size at $%06X", at)
	}
	return b, nil
}

func extractFillmore(dst *image.NRGBA, data []byte, assets *Assets) error {
	characters, err := unpack(data, 0x74000, 8192)
	if err != nil {
		return err
	}
	second, err := unpack(data, 0x78000, 8192)
	if err != nil {
		return err
	}
	characters = append(characters, second...)
	palette := append(append([]byte{}, data[0xaff80:0xb0000]...), data[0x2ff80:0x30000]...)
	for _, layer := range []struct {
		mapAt, metaAt, pageY, x int
		attributes              uint16
	}{
		{0xd0704, 0xdd687, 0, 0, 0x0100},
		{0xaf131, 0xd41ca, 2, 256, 0x1000},
	} {
		width, height := int(data[layer.mapAt]), int(data[layer.mapAt+1])
		if width < 1 || height <= layer.pageY || width*height > 64 {
			return fmt.Errorf("invalid Fillmore map dimensions")
		}
		cells, err := unpack(data, layer.mapAt+2, width*height*256)
		if err != nil {
			return err
		}
		meta, err := unpack(data, layer.metaAt, 2048)
		if err != nil {
			return err
		}
		page := cells[layer.pageY*width*256 : (layer.pageY*width+1)*256]
		if err := drawBackground(dst, page, meta, characters, palette, layer.attributes, layer.x, backgroundRow); err != nil {
			return err
		}
	}
	if err := extractEncounterActors(dst, data, assets); err != nil {
		return err
	}
	frames := expectedFrames()
	assets.Catalog.Frames = append(assets.Catalog.Frames, frames[len(frames)-2:]...)
	return nil
}

// One page contains 16x16 metatile IDs; each definition holds four big-endian
// 8px tile words. Native $02:B6D3 mask + per-layer attributes select CHR banks.
func drawBackground(dst *image.NRGBA, page, meta, characters, palette []byte, attributes uint16, ox, oy int) error {
	if len(page) != 256 || len(meta) != 2048 || len(characters) != 16384 || len(palette) != 256 {
		return fmt.Errorf("invalid background inputs")
	}
	for ty := 0; ty < 32; ty++ {
		for tx := 0; tx < 32; tx++ {
			at := int(page[(ty/2)*16+tx/2])*8 + ((ty&1)*2+(tx&1))*2
			entry := binary.BigEndian.Uint16(meta[at:])&0xecff | attributes
			tile, pal := int(entry&0x3ff), int((entry>>10)&7)*32
			if tile >= 512 {
				return fmt.Errorf("background leaves reviewed character banks")
			}
			for y := 0; y < 8; y++ {
				for x := 0; x < 8; x++ {
					sx, sy := x, y
					if entry&0x4000 != 0 {
						sx = 7 - x
					}
					if entry&0x8000 != 0 {
						sy = 7 - y
					}
					index := pixel4bpp(characters, tile, sx, sy)
					if index == 0 {
						continue
					}
					c := binary.LittleEndian.Uint16(palette[pal+int(index)*2:])
					dst.SetNRGBA(ox+tx*8+x, oy+ty*8+y, snesColor(c))
				}
			}
		}
	}
	return nil
}
