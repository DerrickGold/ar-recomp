package workshopart

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/png"
	"os"
	"reflect"
	"testing"
)

func TestActionOriginOverlapAndMirroring(t *testing.T) {
	chr, palette := make([]byte, 8192), make([]byte, 96)
	// The first OAM part wins; the second remains visible through index zero.
	chr[3*2] = 1 << 5
	chr[32+3*2+1] = 1<<5 | 1<<4
	palette[2], palette[4], palette[5] = 31, 0xe0, 3
	def := []byte{16, 16, 40, 24, 2, 0, 8, 16, 10, 0, 0, 0, 0, 8, 16, 10, 0, 1, 0}
	for _, left := range []bool{false, true} {
		out := image.NewNRGBA(image.Rect(0, 0, 96, 96))
		if err := drawAction(out, def, chr, palette, left, 0, 0); err != nil {
			t.Fatal(err)
		}
		x, behind := 42, 43
		if left {
			x, behind = 53, 52
		}
		if out.NRGBAAt(x, 20) != (color.NRGBA{255, 0, 0, 255}) || out.NRGBAAt(behind, 20) != (color.NRGBA{0, 255, 0, 255}) {
			t.Fatal("native anchor, mirror, Y carry or OAM overlap changed")
		}
	}
	for _, bad := range [][]byte{nil, def[:6], {0, 0, 0, 0, 0}, {0, 0, 0, 0, 33}} {
		if err := drawAction(image.NewNRGBA(image.Rect(0, 0, 96, 96)), bad, chr, palette, false, 0, 0); err == nil {
			t.Fatal("accepted malformed action composition")
		}
	}
	bad := bytes.Clone(def)
	bad[5] = 2
	if err := drawAction(image.NewNRGBA(image.Rect(0, 0, 96, 96)), bad, chr, palette, false, 0, 0); err == nil {
		t.Fatal("accepted unreviewed flags")
	}
}

func TestSceneMetadataBounds(t *testing.T) {
	a := syntheticAssets(t)
	for _, tick := range []int{0, -1, 257} {
		a.Catalog.Animations[0].Poses[0].Ticks = tick
		if validate(a) == nil {
			t.Fatal("accepted invalid duration", tick)
		}
	}
	a.Catalog.Animations[0].Poses[0].Ticks = 1
	a.Catalog.Animations[0].Poses[0].Frame = "not-a-frame"
	if validate(a) == nil {
		t.Fatal("accepted missing sprite")
	}
	a = syntheticAssets(t)
	a.Catalog.Version = 1
	if validate(a) == nil {
		t.Fatal("accepted old atlas version")
	}
	if err := drawBackground(image.NewNRGBA(image.Rect(0, 0, 256, 256)), nil, nil, nil, nil, 0, 0, 0); err == nil {
		t.Fatal("accepted missing background inputs")
	}
	if _, err := unpack([]byte{0, 0}, 0, 8192); err == nil {
		t.Fatal("accepted wrong decoded size")
	}
	for _, at := range []int{-1, 0, 2} {
		if _, err := tablePointer([]byte{255, 255}, at); err == nil {
			t.Fatal("accepted invalid encounter pointer")
		}
	}
	for _, mutate := range []func(*Assets){
		func(a *Assets) { a.Catalog.Animations = a.Catalog.Animations[:len(a.Catalog.Animations)-1] },
		func(a *Assets) { a.Catalog.Animations[12].Poses[0].Frame = "master.right.04" },
		func(a *Assets) { a.Catalog.Animations[12].Poses[0].Ticks = 0 },
		func(a *Assets) { a.Catalog.Animations[12].ID = "unreviewed.actor" },
		func(a *Assets) {
			a.Catalog.Animations[12].Poses = append(a.Catalog.Animations[12].Poses, Pose{"fillmore.bird.normal.1f", 1})
		},
	} {
		a := syntheticAssets(t)
		mutate(a)
		if validate(a) == nil {
			t.Fatal("accepted corrupt encounter metadata")
		}
	}
}

func TestSceneryAtlasCellsNeverOverlap(t *testing.T) {
	frames := expectedFrames()
	if len(frames) != 98 {
		t.Fatalf("frontend atlas contract changed: %d frames", len(frames))
	}
	for i, f := range frames {
		r := image.Rect(f.X, f.Y, f.X+f.Width, f.Y+f.Height)
		if !r.In(image.Rect(0, 0, AtlasWidth, AtlasHeight)) {
			t.Fatal("frame outside atlas", f.ID)
		}
		for _, other := range frames[:i] {
			if r.Overlaps(image.Rect(other.X, other.Y, other.X+other.Width, other.Y+other.Height)) {
				t.Fatal("overlapping frames", f.ID, other.ID)
			}
		}
	}
	if got := len(syntheticAssets(t).Catalog.Animations); got != 28 {
		t.Fatalf("frontend clip contract changed: %d", got)
	}
}

func TestRetailActionAgreement(t *testing.T) {
	path := os.Getenv("AR_WORKSHOP_TEST_ROM")
	if path == "" {
		t.Skip("optional local retail-ROM acceptance")
	}
	rom, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	a, err := Extract(rom)
	if err != nil {
		t.Fatal(err)
	}
	atlas, err := png.Decode(bytes.NewReader(a.PNG))
	if err != nil {
		t.Fatal(err)
	}
	frames := map[string]Frame{}
	for _, frame := range a.Catalog.Frames {
		frames[frame.ID] = frame
	}
	// Independently rendered with game-side ActionRoomScene_Load/LookupTile,
	// full-brightness SNES BGR555 and sparse index-zero. No captured art vended.
	for id, want := range map[string]string{
		"fillmore.forest": "5f65a6e9f795caa3f3ad0f24d7468dc5cfd4f940338a674368cf7c27d886f95e",
		"fillmore.ground": "2d60ac15a77a9dad3fe5b3c1df1c1a9edef32924ea2eebfd06f66aa1f7f72040",
	} {
		frame := frames[id]
		picture := image.NewNRGBA(image.Rect(0, 0, 256, 256))
		draw.Draw(picture, picture.Bounds(), atlas, image.Pt(frame.X, frame.Y), draw.Src)
		if got := fmt.Sprintf("%x", sha256.Sum256(picture.Pix)); got != want {
			t.Fatalf("%s disagrees with game scene: %s", id, got)
		}
	}
	for _, visual := range masterVisuals {
		r, l := frames[masterID("right", visual)], frames[masterID("left", visual)]
		for y := 0; y < 96; y++ {
			for x := 0; x < 96; x++ {
				if atlas.At(r.X+x, r.Y+y) != atlas.At(l.X+95-x, l.Y+y) {
					t.Fatalf("Master %x lost shared anchor at %d,%d", visual, x, y)
				}
			}
		}
	}
	for _, actor := range encounterActors {
		for _, visual := range actor.visuals {
			r, l := frames[actorID(actor.id, "normal", visual)], frames[actorID(actor.id, "flipped", visual)]
			nonzero := 0
			for y := 0; y < actor.cell; y++ {
				for x := 0; x < actor.cell; x++ {
					right, left := atlas.At(r.X+x, r.Y+y), atlas.At(l.X+actor.cell-1-x, l.Y+y)
					if right != left {
						t.Fatalf("%s visual %x lost mirrored anchor at %d,%d", actor.id, visual, x, y)
					}
					_, _, _, alpha := right.RGBA()
					if alpha != 0 {
						nonzero++
					}
				}
			}
			if nonzero < 32 {
				t.Fatal("empty encounter sprite", r.ID)
			}
		}
	}
	for clip, want := range map[int][]int{
		1: {1, 1, 1, 1, 1, 5, 5, 5},
		2: {7, 1, 1, 1, 10},
		// The scene's airborne hit waits until tick 23; first seven rows
		// occupy 21 ticks, and the full jump-sword program is 40 ticks.
		4: {2, 3, 4, 4, 1, 5, 2, 1, 1, 1, 3, 4, 4, 3, 2},
	} {
		var ticks []int
		for _, p := range a.Catalog.Animations[clip].Poses {
			ticks = append(ticks, p.Ticks)
		}
		if !reflect.DeepEqual(ticks, want) {
			t.Fatalf("native DEC/BMI duration convention changed: %v", ticks)
		}
	}
}
