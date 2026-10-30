package workshopart

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"os"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
)

func TestAR2MasterFootAnchorAndOverlap(t *testing.T) {
	tiles, palette := make([]byte, 64), make([]byte, 32)
	tiles[6] = 1 << 5
	tiles[32+7] = 1<<5 | 1<<4
	palette[2], palette[4], palette[5] = 31, 0xe0, 3
	frame := ar2master.Frame{Left: 16, Right: 16, Top: 40, Tiles: tiles,
		Parts: []byte{0, 8, 16, 10, 0, 0, 0, 0, 8, 16, 10, 0, 1, 0}}
	for _, left := range []bool{false, true} {
		out := image.NewNRGBA(image.Rect(0, 0, ar2Cell, ar2Cell))
		if err := drawAR2Master(out, frame, palette, left, 0, 0); err != nil {
			t.Fatal(err)
		}
		x, behind := 58, 59
		if left {
			x, behind = 69, 68
		}
		if out.NRGBAAt(x, 69) != (color.NRGBA{255, 0, 0, 255}) || out.NRGBAAt(behind, 69) != (color.NRGBA{0, 255, 0, 255}) {
			t.Fatal("foot anchor, mirroring or first-part priority changed", left)
		}
	}
	frame.Top = 255
	if err := drawAR2Master(image.NewNRGBA(image.Rect(0, 0, ar2Cell, ar2Cell)), frame, palette, false, 0, 0); err == nil {
		t.Fatal("accepted artwork outside its frame")
	}
	if _, err := ExtractAR2Master([]byte("damaged pack")); err == nil {
		t.Fatal("accepted invalid character pack")
	}
}

func TestRetailAR2MasterScenery(t *testing.T) {
	path := os.Getenv("AR2_MASTER_ROM")
	if path == "" {
		t.Skip("optional local ActRaiser 2 ROM")
	}
	rom, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	pack, err := ar2master.Extract(rom)
	if err != nil {
		t.Fatal(err)
	}
	assets, err := ExtractAR2Master(pack)
	if err != nil {
		t.Fatal(err)
	}
	if len(assets.Catalog.Animations) != 16 || len(assets.PNG) > 128<<10 {
		t.Fatal("unexpected character atlas size")
	}
	atlas, err := png.Decode(bytes.NewReader(assets.PNG))
	if err != nil {
		t.Fatal(err)
	}
	frames := assets.Catalog.Frames
	for i, right := range frames[:len(frames)/2] {
		left := frames[i+len(frames)/2]
		visible := 0
		for y := 0; y < ar2Cell; y++ {
			for x := 0; x < ar2Cell; x++ {
				r, l := atlas.At(right.X+x, right.Y+y), atlas.At(left.X+ar2Cell-1-x, left.Y+y)
				if r != l {
					t.Fatalf("%s lost mirrored foot anchor at %d,%d", right.ID, x, y)
				}
				_, _, _, alpha := r.RGBA()
				if alpha != 0 {
					visible++
				}
			}
		}
		if visible < 32 {
			t.Fatal("empty character frame", right.ID)
		}
	}
	// Standing slash: native duration words 3,4,5,3 become 4,5,6,4 ticks.
	for i, want := range []int{4, 5, 6, 4} {
		if assets.Catalog.Animations[2].Poses[i].Ticks != want {
			t.Fatal("native sword timing changed")
		}
	}
	if out := os.Getenv("AR_WORKSHOP_AR2_TEST_OUTPUT"); out != "" {
		if err := os.WriteFile(out, assets.PNG, 0600); err != nil {
			t.Fatal(err)
		}
	}
}
