package localizationkit

import (
	"encoding/json"
	"os"
	"path/filepath"
	"slices"
	"testing"
)

func TestUSRuntimeRoutesROM(t *testing.T) {
	root := os.Getenv("AR_LOCALIZATION_GUI_ROM_ROOT")
	if root == "" {
		t.Skip("regional ROM fixtures not configured")
	}
	rom, err := os.ReadFile(filepath.Join(root, "ar.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	d, err := NewDecoder(rom)
	if err != nil {
		t.Fatal(err)
	}
	// Caller, source, destination and display restoration are one modal
	// lifetime proof. A changed instruction must not silently vend stale routes.
	for _, offset := range []int{0x117e6, 0x117e7, 0x117e9, 0x117ec, 0x11856, 0x11857, 0x11859, 0x1185c, 0x11862} {
		changed := *d
		changed.rom = slices.Clone(d.rom)
		changed.rom[offset] ^= 1
		if err := changed.verifyUSSoundTestComposer(); err == nil {
			t.Fatalf("sound-test lifetime mutation accepted at %x", offset)
		}
	}
	for name, generate := range map[string]func() (IRObject, error){"dialogue": d.USRuntimeDialogueRoutes, "compose": d.USRuntimeComposeRoutes} {
		t.Run(name, func(t *testing.T) {
			actual, err := generate()
			if err != nil {
				t.Fatal(err)
			}
			raw, err := os.ReadFile("../../../tools/data/localization/us-runtime-" + name + "-routes-v1.json")
			if err != nil {
				t.Fatal(err)
			}
			var expected any
			if err := json.Unmarshal(raw, &expected); err != nil {
				t.Fatal(err)
			}
			requireJSONEqual(t, "US runtime "+name+" routes", actual, expected)
		})
	}
}

func TestRuntimeDialogueRoutesRequireUS(t *testing.T) {
	for _, d := range []*Decoder{nil, {profile: decoderProfile{ID: "jp"}}} {
		if result, err := d.USRuntimeDialogueRoutes(); err == nil || result != nil {
			t.Fatal("invalid runtime profile accepted")
		}
		if result, err := d.USRuntimeComposeRoutes(); err == nil || result != nil {
			t.Fatal("invalid composer profile accepted")
		}
	}
}
