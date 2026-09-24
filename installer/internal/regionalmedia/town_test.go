package regionalmedia

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/gameassets"
)

func TestTownSourceSelectors(t *testing.T) {
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		t.Skip("optional five-ROM town source contracts")
	}
	read := func(name string) []byte {
		t.Helper()
		b, e := os.ReadFile(filepath.Join(root, name))
		if e != nil {
			t.Fatal(e)
		}
		return b
	}
	us := read("ar.sfc")
	var baselinePalettes [][]byte
	for _, name := range []string{"ar.sfc", "ar-jp.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc"} {
		rom := read(name)
		entries, err := gameassets.Script(rom)
		if err != nil {
			t.Fatal(err)
		}
		towns := 0
		for _, entry := range entries {
			if entry.Mode != 0 || entry.Submode < 1 || entry.Submode > 6 {
				continue
			}
			towns++
			var sources []int
			paletteIndex := 0
			for _, c := range entry.Commands {
				p := c.Operands
				if c.Code == 0x80 {
					source := int(p[3]) | int(p[4])<<8 | int(p[5])<<16
					if source == 0x60000 || source == 0x64000 || source == 0x68000 {
						if p[0] != 0x80 || p[1] != 0x20 || p[2] != map[int]byte{0x60000: 0, 0x64000: 0, 0x68000: 0x20}[source] {
							t.Fatalf("%s changed raw shape: %x", name, p)
						}
						sources = append(sources, source)
					}
				}
				if c.Code == 0x40 && entry.Submode == 1 {
					source := int(p[3]) | int(p[4])<<8 | int(p[5])<<16
					palette := rom[source+int(p[0])*2 : source+int(p[1])*2]
					if name == "ar.sfc" {
						baselinePalettes = append(baselinePalettes, palette)
					} else if !bytes.Equal(palette, baselinePalettes[paletteIndex]) {
						t.Fatal(name, "palette incompatible")
					}
					paletteIndex++
				}
			}
			if len(sources) != 3 || sources[0] != 0x60000 || sources[1] != 0x64000 || sources[2] != 0x68000 {
				t.Fatal(name, "town bank order", sources)
			}
		}
		if towns != 6 {
			t.Fatal(name, "town count", towns)
		}
		comp := 0xd32b
		if name == "ar-jp.sfc" {
			comp = 0xd2b5
		}
		if !bytes.Equal(rom[comp:comp+42], us[0xd32b:0xd32b+42]) {
			t.Fatal(name, "follower composition contract")
		}
		for _, tile := range []int{0x1a2, 0x1a3, 0x1b2, 0x1b3, 0x1a8, 0x1a9, 0x1b8, 0x1b9} {
			if name != "ar-jp.sfc" && !bytes.Equal(rom[0x68000+tile*32:0x68000+(tile+1)*32], us[0x68000+tile*32:0x68000+(tile+1)*32]) {
				t.Fatal(name, "unexpected Western follower variant")
			}
		}
		for _, tile := range []int{0x1ce, 0x1cf, 0x1de, 0x1df} {
			if !bytes.Equal(rom[0x64000+tile*32:0x64000+(tile+1)*32], us[0x64000+tile*32:0x64000+(tile+1)*32]) {
				t.Fatal(name, "late lair bank must stay native")
			}
		}
		if !bytes.Equal(rom[0x62260:0x62280], rom[0x66260:0x66280]) {
			t.Fatal(name, "pyramid bank disagreement")
		}
	}
	// These characters occur only in structure definitions, not in the
	// immutable ground atlas used by enhanced navigation. No global atlas
	// mutation/cache invalidation is needed for these sparse replacements.
	for i := 0; i < 256*4; i++ {
		tile := binary.BigEndian.Uint16(us[0xc881a+i*2:]) & 511
		if tile == 0x113 || tile == 0x1ce || tile == 0x1cf || tile == 0x1de || tile == 0x1df {
			t.Fatal("town artwork unexpectedly shared with ground")
		}
	}
}
