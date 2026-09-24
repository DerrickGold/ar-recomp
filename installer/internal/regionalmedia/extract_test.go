package regionalmedia

import (
	"bytes"
	"crypto/sha256"
	"github.com/DerrickGold/ar-recomp/installer/internal/gameassets"
	"github.com/DerrickGold/ar-recomp/installer/internal/quintet"
	"os"
	"path/filepath"
	"testing"
)

func TestROMResources(t *testing.T) {
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		t.Skip("set AR_MEDIA_ROM_DIR for optional five-ROM checks")
	}
	for _, name := range []string{"ar.sfc", "ar-jp.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc"} {
		t.Run(name, func(t *testing.T) {
			rom, err := os.ReadFile(filepath.Join(root, name))
			if err != nil {
				t.Fatal(err)
			}
			assets, err := Extract(rom)
			if err != nil {
				t.Fatal(err)
			}
			for _, r := range assets.Resources {
				t.Logf("%s id=%d size=%d sha256=%x", assets.Release.ID, r.ID, len(r.Bytes), sha256.Sum256(r.Bytes))
			}
			entries, _ := gameassets.Script(rom)
			var source int
			owners := 0
			for _, entry := range entries {
				if entry.Mode != 7 || (entry.Submode != 1 && entry.Submode != 8) {
					continue
				}
				if len(entry.Commands) == 0 || entry.Commands[0].Code != 8 {
					t.Fatal("profile must precede donor upload")
				}
				for _, c := range entry.Commands {
					if c.Code&0x80 == 0 || c.Operands[2] != 0x10 {
						continue
					}
					p := c.Operands
					address := int(p[3]) | int(p[4])<<8 | int(p[5])<<16
					if source != 0 && source != address {
						t.Fatal("Death Heim entry/final source ownership changed")
					}
					source = address
					owners++
				}
			}
			if owners != 2 || (assets.Release.ID == "us" && source != 0x7d146) {
				t.Fatal("native donor seam source changed", source, owners)
			}
		})
	}
}

func TestRejectUnknownROM(t *testing.T) {
	if assets, err := Extract(make([]byte, 1<<20)); err == nil || assets != nil {
		t.Fatal("unrecognized ROM accepted")
	}
}

// Test the native consumers as well as resource hashes. A self-consistent
// extraction/hash pair can otherwise approve the wrong mode's item graphic.
func TestPALItemSourceSelectors(t *testing.T) {
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		t.Skip("optional three-ROM item selector checks")
	}
	us, err := os.ReadFile(filepath.Join(root, "ar.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	for region, name := range []string{"ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc"} {
		rom, err := os.ReadFile(filepath.Join(root, name))
		if err != nil {
			t.Fatal(err)
		}
		assertCode := func(at int, code []byte) {
			t.Helper()
			if !bytes.Equal(rom[at:at+len(code)], code) {
				t.Fatalf("%s selector changed at %x", name, at)
			}
		}
		// ID*$80 + A000; only Action mode2 adds0800 before publishing.
		assertCode(0x11e0, []byte{0xbd, 0x38, 0, 0x29, 0xff, 0, 0xeb, 0x4a, 0x18, 0x69, 0, 0xa0,
			0x5a, 0xac, 0x36, 3, 0xc0, 2, 0, 0xd0, 4, 0x18, 0x69, 0, 8, 0x85, 0xd1})
		// Dirty HUD: zero ->5, then DEC => zero-based index4, not5.
		assertCode(0x125f, []byte{0xbd, 0x38, 0, 0x29, 0xff, 0, 0xd0, 3, 0xa9, 5, 0, 0x3a, 0xeb, 0x4a, 0x18, 0x69, 0, 0xac})
		// Shared heal/score effects still differ in sound from US; health
		// growth uses COP8D, while all four spell pickups request BRK0F.
		for _, at := range []int{0x7f6, 0x819, 0x82e, 0x839, 0x846, 0x856} {
			assertCode(at, []byte{0xa9, 0x0f, 0, 0})
		}
		assertCode(0x802, []byte{0xa9, 0x8d, 2, 0})
		// Room entry has the same Action zero->5 choice but copies128 words.
		base := []int{0x142cc, 0x142d5, 0x142be}[region]
		assertCode(base, []byte{0xad, 0x36, 3, 0xc9, 2, 0, 0xd0, 0x18, 0xa4, 0x21, 0xf0, 8,
			0xb9, 0xff, 0x1b, 0x29, 0xff, 0, 0xd0, 3, 0xa9, 5, 0, 0x3a, 0xeb, 0x4a, 0x18, 0x69, 0, 8})
		assertCode(base+0x35, []byte{0xa9, 0x80, 0, 0x85, 0x0c, 0xbf, 0, 0xa4, 6, 0x8d, 0x18, 0x21, 0xe8, 0xe8, 0xc6, 0x0c, 0xd0, 0xf3})
		extracted, err := Extract(rom)
		if err != nil {
			t.Fatal(err)
		}
		var health, hud []byte
		for _, resource := range extracted.Resources {
			if resource.ID == ActionHealthPickup {
				health = resource.Bytes
			}
			if resource.ID == ActionSpellHUD {
				hud = resource.Bytes
			}
		}
		if !bytes.Equal(health, rom[0x32880:0x32900]) || bytes.Equal(health, rom[0x32080:0x32100]) {
			t.Fatal("health-growth graphic confused with Story extra-life graphic")
		}
		if len(hud) != 768 {
			t.Fatal("HUD resource extent", len(hud))
		}
		for spell := 0; spell <= 4; spell++ {
			index := spell - 1
			if spell == 0 {
				index = 4
			}
			if !bytes.Equal(hud[index*128:index*128+256], rom[0x32c00+index*128:0x32d00+index*128]) {
				t.Fatal("HUD room window", spell)
			}
		}
		if !bytes.Equal(us[0x3d040:0x3d0a0], rom[0x3d040:0x3d0a0]) {
			t.Fatal("common OBJ palette is incompatible")
		}
	}
}

// A CHR-only donor must not require a different palette, metatile numbering or
// background map. Verify those owners across all five releases, not just the
// appearance of one screenshot.
func TestDeathHeimCHRCompatibility(t *testing.T) {
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		t.Skip("optional donor compatibility")
	}
	var baseline [3][]byte
	for region, name := range []string{"ar.sfc", "ar-jp.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc"} {
		rom, err := os.ReadFile(filepath.Join(root, name))
		if err != nil {
			t.Fatal(err)
		}
		entries, err := gameassets.Script(rom)
		if err != nil {
			t.Fatal(err)
		}
		var actual [3][]byte
		for _, entry := range entries {
			if entry.Mode != 7 || entry.Submode != 1 {
				continue
			}
			for _, c := range entry.Commands {
				p := c.Operands
				if c.Code == 0x40 && p[2] == 0 {
					if p[0] != 0 || p[1] != 0x40 {
						t.Fatal("palette shape")
					}
					source := int(p[3]) | int(p[4])<<8 | int(p[5])<<16
					actual[0] = rom[source : source+128]
				}
				if c.Code == 0x20 && p[3] == 2 {
					source := int(p[4]) | int(p[5])<<8 | int(p[6])<<16
					actual[1], _, err = quintet.Decompress(rom, source)
					if err != nil {
						t.Fatal(err)
					}
				}
				if c.Code == 0x10 && p[0] == 2 {
					source := int(p[1]) | int(p[2])<<8 | int(p[3])<<16
					decoded, _, err := quintet.Decompress(rom, source+2)
					if err != nil {
						t.Fatal(err)
					}
					actual[2] = append(append([]byte(nil), rom[source:source+2]...), decoded...)
				}
			}
		}
		for i, data := range actual {
			if len(data) == 0 {
				t.Fatal("missing BG2 owner", name, i)
			}
			if region == 0 {
				baseline[i] = append([]byte(nil), data...)
			} else if !bytes.Equal(data, baseline[i]) {
				t.Fatal("CHR-only donor is incompatible", name, i)
			}
		}
	}
}
