package regionalmedia

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/gameassets"
	"github.com/DerrickGold/ar-recomp/installer/internal/quintet"
)

func TestGeneratedActorBindingsCurrent(t *testing.T) {
	path := filepath.Join(t.TempDir(), "native.inc")
	if output, err := exec.Command("go", "run", "./cmd/genactors", path).CombinedOutput(); err != nil {
		t.Fatalf("%v: %s", err, output)
	}
	want, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile("../../../src/regional/regional_actor_art_native.inc")
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, want) {
		t.Fatal("run go generate ./internal/regionalmedia; native actor bindings are stale")
	}
}

func TestNativeActorBindings(t *testing.T) {
	bindings := NativeActorBindings()
	if len(bindings) != 115 {
		t.Fatal("native actor declaration count")
	}
	var previous uint32
	loads := make(map[uint16]uint8)
	starts := 0
	sources := make(map[uint32]bool)
	for _, b := range bindings {
		key := uint32(b.Scene)<<16 | uint32(b.Kind)<<8 | uint32(b.Slot)
		if key <= previous || b.Kind < 1 || b.Kind > 3 || b.Slot > 1 || b.Source >= 1<<20 || b.Size == 0 {
			t.Fatal("invalid declaration", b)
		}
		previous = key
		if b.Kind == actorCharacters {
			loads[b.Scene] |= 1 << b.Slot
		}
		if b.Kind == actorPalette {
			loads[b.Scene] |= 4
		}
		if b.Kind == actorPictures && (b.Pictures == 0 || b.Pictures > actorMaxPictures || int(b.Table)+2*int(b.Pictures) > int(b.Size)) {
			t.Fatal("picture table bounds")
		}
		if b.Kind == actorPictures {
			if sources[b.Source] {
				t.Fatal("ambiguous native animation source owner", b)
			}
			sources[b.Source] = true
			if b.Slot == 0 {
				starts++
			}
		}
	}
	// A presentation choice can be captured once on the first native sprite
	// upload: every reviewed character load is paired with its palette in the
	// same script. Scenes without these declarations inherit the prior choice.
	if len(loads) != 40 {
		t.Fatal("sprite upload boundary census")
	}
	if starts != 13 {
		t.Fatal("expected the twelve act entrances and Death Heim")
	}
	for scene, mask := range loads {
		if mask&4 == 0 || mask&3 == 0 {
			t.Fatalf("unpaired sprite upload %04x/%x", scene, mask)
		}
	}
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		t.Skip("optional native ROM declaration agreement")
	}
	rom, err := os.ReadFile(filepath.Join(root, "ar.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	entries, err := gameassets.Script(rom)
	if err != nil {
		t.Fatal(err)
	}
	probe := os.Getenv("AR_ACTOR_ART_PROBE")
	drawProbe := os.Getenv("AR_ACTOR_DRAW_PROBE")
	var donorPath string
	if drawProbe != "" {
		jp, err := os.ReadFile(filepath.Join(root, "ar-jp.sfc"))
		if err != nil {
			t.Fatal(err)
		}
		packet, err := japaneseActorArt(jp)
		if err != nil {
			t.Fatal(err)
		}
		donorPath = filepath.Join(t.TempDir(), "donor.bin")
		if err := os.WriteFile(donorPath, packet, 0600); err != nil {
			t.Fatal(err)
		}
	}
	for _, b := range bindings {
		matches := 0
		for _, e := range entries {
			if uint16(e.Submode)<<8|uint16(e.Mode) != b.Scene {
				continue
			}
			for _, c := range e.Commands {
				p := c.Operands
				match := b.Kind == actorPictures && c.Code == 1 && p[0] == b.Slot ||
					b.Kind == actorCharacters && c.Code == 0x80 && p[0] == 0 && p[1] == 0x10 && p[2] == 0x30+b.Slot*0x10 ||
					b.Kind == actorPalette && c.Code == 0x40 && p[0] == 0 && p[1] == 0x40 && p[2] == 0x80
				if !match {
					continue
				}
				matches++
				if int(b.Source) != int(p[3])|int(p[4])<<8|int(p[5])<<16 {
					t.Fatal("native source mismatch", b)
				}
			}
		}
		if matches != 1 {
			t.Fatal("non-unique native declaration", b)
		}
		if b.Kind == actorPalette {
			if b.Size != 128 {
				t.Fatal("native palette size")
			}
			continue
		}
		data, _, err := quintet.Decompress(rom, int(b.Source))
		if err != nil {
			t.Fatal(err)
		}
		if len(data) != int(b.Size) {
			t.Fatal("native image size", b)
		}
		if b.Kind != actorPictures {
			continue
		}
		if binary.LittleEndian.Uint16(data) != b.Table || int(binary.LittleEndian.Uint16(data[b.Table:])) != int(b.Table)+2*int(b.Pictures) {
			t.Fatal("native table identity", b)
		}
		if probe != "" || drawProbe != "" {
			path := filepath.Join(t.TempDir(), "native.bin")
			if err := os.WriteFile(path, data, 0600); err != nil {
				t.Fatal(err)
			}
			args := []string{"--native", fmt.Sprintf("%x", b.Source), fmt.Sprintf("%x", 0x4000+int(b.Slot)*0x1000), path}
			if probe != "" {
				if output, err := exec.Command(probe, args...).CombinedOutput(); err != nil {
					t.Fatalf("native C binding %04x/%d: %v: %s", b.Scene, b.Slot, err, output)
				}
			}
			if drawProbe != "" {
				if output, err := exec.Command(drawProbe, append(args, donorPath)...).CombinedOutput(); err != nil {
					t.Fatalf("native C draw adapter %04x/%d: %v: %s", b.Scene, b.Slot, err, output)
				}
			}
		}
	}
}
