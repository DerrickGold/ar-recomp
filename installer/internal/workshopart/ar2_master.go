package workshopart

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"image"
	"image/png"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
	"github.com/DerrickGold/ar-recomp/installer/internal/ar2rom"
)

const ar2Cell = 128

// These animation IDs are shared with the native Master control transcription.
// Scenery samples their artwork/timing; its travel paths are decorative only.
var ar2Clips = [...]struct {
	id        string
	animation int
}{
	{"idle", 0x01}, {"walk", 0x1a}, {"sword", 0x04},
	{"jump", 0x0f}, {"air-sword", 0x1b}, {"fall", 0x10},
	{"glide-start", 0x13}, {"glide", 0x15},
}

// ExtractAR2Master uses the installed character pack, so scenery survives ROM
// removal and never depends on importing or running the optional second game.
func ExtractAR2Master(pack []byte) (*Assets, error) {
	graphics, err := ar2master.ParseGraphics(pack)
	if err != nil {
		return nil, err
	}
	var frames []int
	seen := map[int]bool{}
	for _, clip := range ar2Clips {
		for _, step := range graphics.Animations[clip.animation] {
			if !seen[step.Frame] {
				seen[step.Frame] = true
				frames = append(frames, step.Frame)
			}
		}
	}
	atlas := image.NewNRGBA(image.Rect(0, 0, 8*ar2Cell, (len(frames)*2+7)/8*ar2Cell))
	result := &Assets{Catalog: Catalog{Version: 1, ROM: fmt.Sprintf("%x", ar2rom.Digest)}}
	id := func(facing string, index int) string { return fmt.Sprintf("ar2-master.%s.%02x", facing, index) }
	for _, facing := range []string{"right", "left"} {
		for _, index := range frames {
			n := len(result.Catalog.Frames)
			frame := Frame{id(facing, index), n % 8 * ar2Cell, n / 8 * ar2Cell, ar2Cell, ar2Cell}
			if err := drawAR2Master(atlas, graphics.Frames[index], graphics.Palette, facing == "left", frame.X, frame.Y); err != nil {
				return nil, err
			}
			result.Catalog.Frames = append(result.Catalog.Frames, frame)
		}
		for _, clip := range ar2Clips {
			animation := Animation{ID: "ar2-master." + facing + "." + clip.id}
			for _, step := range graphics.Animations[clip.animation] {
				animation.Poses = append(animation.Poses, Pose{id(facing, step.Frame), step.Ticks})
			}
			result.Catalog.Animations = append(result.Catalog.Animations, animation)
		}
	}
	var encoded bytes.Buffer
	if err := png.Encode(&encoded, atlas); err != nil {
		return nil, err
	}
	result.PNG = encoded.Bytes()
	result.Catalog.ImageSHA256 = fmt.Sprintf("%x", sha256.Sum256(result.PNG))
	return result, nil
}

func drawAR2Master(dst *image.NRGBA, frame ar2master.Frame, palette []byte, left bool, ox, oy int) error {
	xt, extent, flip := 1, int(frame.Left), uint16(0)
	if left {
		xt, extent, flip = 2, int(frame.Right), 0x4000
	}
	// Every pose shares a foot anchor (64,96), including wings and sword arcs.
	// Earlier OAM parts win overlap, just as in the game's sprite compositor.
	for i := len(frame.Parts) - 7; i >= 0; i -= 7 {
		part := frame.Parts[i : i+7]
		size := 8 + int(part[0]&1)*8
		x, y := 64-extent+int(part[xt]), 96-int(frame.Top)+int(part[3])
		if x < 0 || y < 0 || x+size > ar2Cell || y+size > ar2Cell {
			return fmt.Errorf("AR2 scenery sprite exceeds its canvas")
		}
		if err := drawObjectPart(dst, frame.Tiles, palette, ox+x, oy+y, size, binary.LittleEndian.Uint16(part[5:])^flip); err != nil {
			return err
		}
	}
	return nil
}
