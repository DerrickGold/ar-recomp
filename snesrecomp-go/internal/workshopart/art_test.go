package workshopart

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

func TestPixelPlanesAndCompositionFlips(t *testing.T) {
	data := make([]byte, 8192)
	// Four plane bits produce palette index 15 at tile 0, pixel (2,3).
	for _, i := range []int{6, 7, 22, 23} {
		data[256+i] = 1 << 5
	}
	data[512+30], data[512+31] = 0x1f, 0x7c // magenta BGR555.
	data[0] = 1
	copy(data[1:], []byte{0, 0, 0, 0, 0})
	row := composition{"test", 0, 256, 512, 8, 8}
	for _, test := range []struct {
		flags byte
		x, y  int
	}{{0, 2, 3}, {0x40, 5, 3}, {0x80, 2, 4}, {0xc0, 5, 4}} {
		data[5] = test.flags
		dst := image.NewNRGBA(image.Rect(0, 0, 8, 8))
		if err := drawComposition(dst, data, row, 0, 0); err != nil {
			t.Fatal(err)
		}
		if dst.NRGBAAt(test.x, test.y) != (color.NRGBA{255, 0, 255, 255}) {
			t.Fatal("incorrect tile flip/planes/palette", test, dst.NRGBAAt(test.x, test.y))
		}
		if dst.NRGBAAt(0, 0).A != 0 {
			t.Fatal("palette zero is not transparent")
		}
	}
	for _, test := range []struct {
		b byte
		n int
	}{{0x7f, 127}, {0x80, 128}, {0x81, -127}, {0xff, -1}} {
		if partOffset(test.b) != test.n {
			t.Fatal(test)
		}
	}
	data[2] = 0xff
	if err := drawComposition(image.NewNRGBA(image.Rect(0, 0, 8, 8)), data, row, 0, 0); err == nil {
		t.Fatal("accepted unreviewed bounds")
	}
}

func TestWrongROMAndCacheValidation(t *testing.T) {
	for _, data := range [][]byte{nil, make([]byte, 1<<20), make([]byte, (1<<20)+512)} {
		if _, err := Extract(data); err == nil {
			t.Fatal("accepted unsupported ROM")
		}
	}
	a := syntheticAssets(t)
	root, err := OpenCacheRoot(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer root.Close()
	if err := WriteCache(root, a); err != nil {
		t.Fatal(err)
	}
	loaded, err := ReadCache(root)
	if err != nil || !bytes.Equal(loaded.PNG, a.PNG) {
		t.Fatal(err)
	}
	a.Catalog.Frames[0].Width++
	if err := WriteCache(root, a); err == nil {
		t.Fatal("accepted modified geometry")
	}
	if _, err := ReadCache(root); err != nil {
		t.Fatal("failed update damaged old cache", err)
	}
	if err := os.WriteFile(filepath.Join(root.Name(), cacheFile), []byte("truncated"), 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := ReadCache(root); err == nil {
		t.Fatal("accepted broken cache")
	}
	dir, outside := t.TempDir(), t.TempDir()
	if err := os.Symlink(outside, filepath.Join(dir, "game-assets")); err != nil {
		t.Skip(err)
	}
	if _, err := OpenCacheRoot(dir); err == nil {
		t.Fatal("followed cache symlink")
	}
}

func syntheticAssets(t *testing.T) *Assets {
	t.Helper()
	a := &Assets{Catalog: Catalog{Version: Version, ROM: SourceSHA256, Frames: expectedFrames()}}
	for _, facing := range []string{"right", "left"} {
		for _, clip := range masterClips {
			animation := Animation{ID: "master." + facing + "." + clip.id}
			for i := 0; i < clip.count; i++ {
				animation.Poses = append(animation.Poses, Pose{masterID(facing, 4), 1})
			}
			a.Catalog.Animations = append(a.Catalog.Animations, animation)
		}
	}
	for _, actor := range encounterActors {
		for _, facing := range []string{"normal", "flipped"} {
			for _, clip := range actor.clips {
				animation := Animation{ID: actor.id + "." + facing + "." + clip.id}
				for _, program := range clip.programs {
					for _, v := range program.visuals {
						animation.Poses = append(animation.Poses, Pose{actorID(actor.id, facing, v), 1})
					}
				}
				a.Catalog.Animations = append(a.Catalog.Animations, animation)
			}
		}
	}
	var encoded bytes.Buffer
	if err := png.Encode(&encoded, image.NewNRGBA(image.Rect(0, 0, AtlasWidth, AtlasHeight))); err != nil {
		t.Fatal(err)
	}
	a.PNG = encoded.Bytes()
	a.Catalog.ImageSHA256 = fmt.Sprintf("%x", sha256.Sum256(a.PNG))
	return a
}

func TestLargeObjectTileWrap(t *testing.T) {
	data := make([]byte, 16000)
	data[0] = 1
	copy(data[1:], []byte{1, 0, 0, 15, 0})
	const graphics = 256
	const palette = 10000
	for _, p := range []struct{ tile, index int }{{15, 1}, {0, 2}, {31, 3}, {16, 4}} {
		for y := 0; y < 8; y++ {
			for plane := 0; plane < 4; plane++ {
				if p.index&(1<<plane) != 0 {
					data[graphics+p.tile*32+(plane/2)*16+y*2+plane%2] = 255
				}
			}
		}
	}
	for i, c := range []uint16{0, 0x001f, 0x03e0, 0x7c00, 0x7fff} {
		data[palette+i*2] = byte(c)
		data[palette+i*2+1] = byte(c >> 8)
	}
	row := composition{"large", 0, graphics, palette, 16, 16}
	for _, flags := range []byte{0, 0x40, 0x80, 0xc0} {
		data[5] = flags
		dst := image.NewNRGBA(image.Rect(0, 0, 16, 16))
		if err := drawComposition(dst, data, row, 0, 0); err != nil {
			t.Fatal(err)
		}
		for _, p := range []struct {
			x, y int
			c    color.NRGBA
		}{{2, 3, color.NRGBA{255, 0, 0, 255}}, {10, 3, color.NRGBA{0, 255, 0, 255}}, {2, 11, color.NRGBA{0, 0, 255, 255}}, {10, 11, color.NRGBA{255, 255, 255, 255}}} {
			x, y := p.x, p.y
			if flags&0x40 != 0 {
				x = 15 - x
			}
			if flags&0x80 != 0 {
				y = 15 - y
			}
			if dst.NRGBAAt(x, y) != p.c {
				t.Fatal("16x16 nibble wrapping/flip mismatch", flags, x, y, dst.NRGBAAt(x, y))
			}
		}
	}
}

func BenchmarkRetailExtract(b *testing.B) {
	path := os.Getenv("AR_WORKSHOP_TEST_ROM")
	if path == "" {
		b.Skip("optional local US ROM")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		b.Fatal(err)
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := Extract(data); err != nil {
			b.Fatal(err)
		}
	}
}

func TestRetailArtwork(t *testing.T) {
	path := os.Getenv("AR_WORKSHOP_TEST_ROM")
	if path == "" {
		t.Skip("optional local retail-ROM acceptance")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	d, err := localizationkit.NewDecoder(data)
	if err != nil || d.ReleaseID() != "us" {
		t.Fatal("graphics identity disagrees with shared source profile", err)
	}
	a, err := Extract(data)
	if err != nil {
		t.Fatal(err)
	}
	if err := validate(a); err != nil {
		t.Fatal(err)
	}
	if prefix := os.Getenv("AR_WORKSHOP_TEST_SIM_SNAPSHOT"); prefix != "" {
		vram, err := os.ReadFile(prefix + ".vram.bin")
		if err != nil {
			t.Fatal(err)
		}
		cgram, err := os.ReadFile(prefix + ".cgram.bin")
		if err != nil {
			t.Fatal(err)
		}
		if len(vram) != 65536 || len(cgram) != 512 || !bytes.Equal(data[0x68000:0x6c000], vram[0x4000:0x8000]) || !bytes.Equal(data[0xe3c93:0xe3d93], cgram[0x100:0x200]) {
			t.Fatal("ROM sprite upload/palette differs from native snapshot")
		}
	}
	if out := os.Getenv("AR_WORKSHOP_TEST_OUTPUT"); out != "" {
		if err := os.WriteFile(out, a.PNG, 0644); err != nil {
			t.Fatal(err)
		}
	}
}
