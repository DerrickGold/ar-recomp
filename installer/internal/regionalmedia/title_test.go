package regionalmedia

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/gameassets"
)

func TestTitleSourceSelectors(t *testing.T) {
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		t.Skip("optional five-ROM title source contracts")
	}
	read := func(name string) []byte {
		t.Helper()
		b, err := os.ReadFile(filepath.Join(root, name))
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	us := read("ar.sfc")
	for _, name := range []string{"ar.sfc", "ar-jp.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc"} {
		rom := read(name)
		entries, err := gameassets.Script(rom)
		if err != nil {
			t.Fatal(err)
		}
		var palette, characters, tiles []byte
		var order []byte
		for _, e := range entries {
			if e.Mode != 0 || e.Submode != 0 {
				continue
			}
			for _, c := range e.Commands {
				p := c.Operands
				switch {
				case c.Code == 0x40:
					if !bytes.Equal(p[:3], []byte{0, 128, 0}) {
						t.Fatal(name, "title palette shape")
					}
					s := int(p[3]) | int(p[4])<<8 | int(p[5])<<16
					palette = rom[s : s+256]
					order = append(order, 1)
				case c.Code == 0x80 && p[2] == 0xff:
					if !bytes.Equal(p, []byte{0x80, 0x20, 0xff, 0, 0x83, 5}) {
						t.Fatal(name, "title raw characters")
					}
					characters = rom[0x58300:0x5c300]
					order = append(order, 2)
				case c.Code == 0x10:
					if p[0] != 0x80 {
						t.Fatal(name, "title map encoding")
					}
					s := int(p[1]) | int(p[2])<<8 | int(p[3])<<16
					if !bytes.Equal(rom[s:s+2], []byte{8, 8}) {
						t.Fatal(name, "title map dimensions")
					}
					tiles = rom[s+2 : s+2+16384]
					order = append(order, 3)
				}
			}
		}
		if !bytes.Equal(order, []byte{1, 2, 3}) {
			t.Fatal(name, "title upload order", order)
		}
		// The HD title's US placement starts at x=11, but JP has nine ink
		// pixels at x=8..10. The host's exact three-pixel coverage gutter
		// must contain the union without moving/scaling the authored image.
		outsideUS := 0
		for y := 0; y < 140; y++ {
			for x := 0; x < 256; x++ {
				cx, cy := x+128, y+129
				tile := int(tiles[(cy/8)*128+cx/8])
				if characters[tile*64+(cy%8)*8+cx%8] == 0 {
					continue
				}
				if x < 8 || x >= 248 || y < 27 || y >= 122 {
					t.Fatalf("%s native title pixel (%d,%d) outside HD coverage", name, x, y)
				}
				if x < 11 {
					outsideUS++
				}
			}
		}
		wantOutside := 0
		if name == "ar-jp.sfc" {
			wantOutside = 9
		}
		if outsideUS != wantOutside {
			t.Fatalf("%s left-edge coverage: %d pixels, want %d", name, outsideUS, wantOutside)
		}
		if !bytes.Equal(palette[0x20*2:0x2a*2], us[0xe3a93+0x20*2:0xe3a93+0x2a*2]) ||
			!bytes.Contains(rom, us[0x12a76:0x12a9c]) {
			t.Fatal(name, "native title palette cycle incompatible")
		}
		if name != "ar-jp.sfc" {
			if !bytes.Equal(characters, us[0x58300:0x5c300]) || !bytes.Equal(tiles, us[0x28e7f:0x2ce7f]) ||
				!bytes.Equal(palette, us[0xe3a93:0xe3b93]) {
				t.Fatal(name, "unexpected Western title artwork")
			}
		} else {
			extracted, err := Extract(rom)
			if err != nil {
				t.Fatal(err)
			}
			var packed []byte
			for _, r := range extracted.Resources {
				if r.ID == TitleBackground {
					packed = r.Bytes
				}
			}
			if len(packed) != 33024 || !bytes.Equal(packed[:16384], characters) ||
				!bytes.Equal(packed[16384:32768], tiles) || !bytes.Equal(packed[32768:], palette) {
				t.Fatal("title resource disagreement")
			}
		}
	}
}
