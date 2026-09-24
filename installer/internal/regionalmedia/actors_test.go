package regionalmedia

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func TestActorDrawingSources(t *testing.T) {
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		t.Skip("optional five-ROM actor drawing contract")
	}
	all := make(map[string][]actorResource)
	for _, name := range []string{"ar.sfc", "ar-jp.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc"} {
		rom, err := os.ReadFile(filepath.Join(root, name))
		if err != nil {
			t.Fatal(err)
		}
		resources, err := actorResources(rom)
		if err != nil {
			t.Fatal(name, err)
		}
		all[name] = resources
		for _, r := range resources {
			if r.kind != actorPictures {
				continue
			}
			count := int(binary.LittleEndian.Uint16(r.data))
			for v := 0; v < count; v++ {
				start, end := binary.LittleEndian.Uint32(r.data[4+4*v:]), binary.LittleEndian.Uint32(r.data[8+4*v:])
				parts := int(binary.LittleEndian.Uint16(r.data[start:]))
				if int(end-start) != 4+12*parts {
					t.Fatal("noncanonical picture")
				}
			}
		}
		if name == "ar-jp.sfc" {
			var kinds [4]int
			for _, r := range resources {
				kinds[r.kind]++
			}
			t.Logf("resource kinds: characters=%d palettes=%d picture tables=%d", kinds[1], kinds[2], kinds[3])
			data, err := japaneseActorArt(rom)
			if err != nil {
				t.Fatal(err)
			}
			t.Logf("actor art entries=%d bytes=%d sha256=%x", len(resources), len(data), sha256.Sum256(data))
			if probe := os.Getenv("AR_ACTOR_ART_PROBE"); probe != "" {
				path := filepath.Join(t.TempDir(), "actors.bin")
				if err := os.WriteFile(path, data, 0600); err != nil {
					t.Fatal(err)
				}
				if output, err := exec.Command(probe, path).CombinedOutput(); err != nil {
					t.Fatalf("C consumer: %v: %s", err, output)
				}
			}
		}
	}
	us := all["ar.sfc"]
	resource := func(name string, scene uint16, kind, slot byte) []byte {
		for _, r := range all[name] {
			if r.scene == scene && r.kind == kind && r.slot == slot {
				return r.data
			}
		}
		t.Fatalf("missing %s resource %04x/%d/%d", name, scene, kind, slot)
		return nil
	}
	// Three native-only visual ordinals need explicit runtime handling. The
	// Bloodpool placeholder and Pharaoh beam keep native parts; their actual
	// tiles must remain compatible with the selected Japanese atlas.
	for _, special := range []struct {
		scene uint16
		slot  byte
		tile  int
		blank bool
	}{{0x0202, 0, 0xb9, true}, {0x0303, 0, 0xa8, false}, {0x0407, 1, 0xa8, false}} {
		a := resource("ar.sfc", special.scene, actorCharacters, special.slot)[special.tile*32 : (special.tile+1)*32]
		b := resource("ar-jp.sfc", special.scene, actorCharacters, special.slot)[special.tile*32 : (special.tile+1)*32]
		if !bytes.Equal(a, b) || (special.blank && !bytes.Equal(a, make([]byte, 32))) {
			t.Fatal("native-only picture's fallback tile changed", special)
		}
	}
	for name, resources := range all {
		if len(resources) != len(us) {
			t.Fatalf("%s resource count %d vs US%d", name, len(resources), len(us))
		}
		for i, r := range resources {
			if r.key() != us[i].key() {
				t.Fatalf("%s asset declaration mismatch %x/%x", name, r.key(), us[i].key())
			}
			if name != "ar-jp.sfc" && !bytes.Equal(r.data, us[i].data) {
				// PAL's four new Northwall impact poses are gameplay geometry,
				// supplied independently by the existing boss-rule adapter.
				if r.key() != 0x04060301 {
					t.Fatalf("unexpected Western difference: %s %x", name, r.key())
				}
				picture := func(data []byte, v int) []byte {
					return data[binary.LittleEndian.Uint32(data[4+4*v:]):binary.LittleEndian.Uint32(data[8+4*v:])]
				}
				if binary.LittleEndian.Uint16(r.data) != 25 || binary.LittleEndian.Uint16(us[i].data) != 21 {
					t.Fatal("Northwall ordinal mismatch")
				}
				for v := 0; v < 21; v++ {
					other := v
					if v >= 9 {
						other += 4
					}
					if !bytes.Equal(picture(us[i].data, v), picture(r.data, other)) {
						t.Fatal("Northwall picture relocation mismatch")
					}
				}
			}
		}
	}
}

func TestActorPicturesAreDrawOnly(t *testing.T) {
	// An opaque animation prefix and two identical references to a picture.
	data := []byte{8, 0, 0xde, 0xad, 0xbe, 0xef, 0, 0, 12, 0, 12, 0,
		0xfc, 99, 8, 77, 1, 1, 4, 20, 2, 30, 0x34, 0x42}
	got, err := actorPicturesFromAnimation(data)
	if err != nil {
		t.Fatal(err)
	}
	want := []byte{1, 0, 0, 0, 1, 0, 8, 0, 0xb1, 0xff, 0xfa, 0xff, 0xd1, 0xff, 0x34, 0x42}
	if !bytes.Equal(got[16:32], want) || !bytes.Equal(got[32:], want) {
		t.Fatalf("wrong normal/flipped drawing offsets: %x", got)
	}
	// The opaque program cannot leak into output. Each original header byte
	// participates only in the corresponding facing's derived drawing anchor.
	data[2] ^= 0xff
	data[6] ^= 0xff
	again, err := actorPicturesFromAnimation(data)
	if err != nil || !bytes.Equal(got, again) {
		t.Fatal("non-drawing metadata leaked", err)
	}
	for end := 0; end < len(data); end++ {
		if _, err := actorPicturesFromAnimation(data[:end]); err == nil {
			t.Fatal("truncated picture accepted", end)
		}
	}
	for _, at := range []int{0, 1, 8, 9, 10, 11, 16, 17} {
		bad := append([]byte(nil), data...)
		bad[at] = 0xff
		if _, err := actorPicturesFromAnimation(bad); err == nil {
			t.Fatal("invalid picture accepted", at)
		}
	}
}

func FuzzActorPictures(f *testing.F) {
	f.Add([]byte{2, 0, 4, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0})
	f.Fuzz(func(t *testing.T, data []byte) {
		out, err := actorPicturesFromAnimation(data)
		if err != nil {
			return
		}
		if len(out) > 4+4*(actorMaxPictures+1)+actorMaxPictures*(4+12*actorMaxParts) {
			t.Fatal("unbounded picture export")
		}
	})
}
