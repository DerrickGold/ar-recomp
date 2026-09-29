package appicons

import (
	"bytes"
	"debug/pe"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/tc-hib/winres"
)

// Cross-build actual PE files on the host; no Windows VM/target execution.
func TestWindowsResourcesRoundTrip(t *testing.T) {
	if testing.Short() {
		t.Skip("cross-compiles Windows fixtures")
	}
	for _, arch := range []string{"amd64", "arm64"} {
		t.Run(arch, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "game.exe")
			cmd := exec.Command("go", "build", "-trimpath", "-ldflags=-H windowsgui", "-o", path, "./testdata/main.go")
			cmd.Env = append(os.Environ(), "CGO_ENABLED=0", "GOOS=windows", "GOARCH="+arch)
			if output, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("cross build: %v: %s", err, output)
			}
			original, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			// Seed an unrelated resource to prove icon embedding preserves it.
			rs := &winres.ResourceSet{}
			manifest := []byte(`<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"/>`)
			if err := rs.Set(winres.RT_MANIFEST, winres.ID(1), 0, manifest); err != nil {
				t.Fatal(err)
			}
			var seeded bytes.Buffer
			if err := rs.WriteToEXE(&seeded, bytes.NewReader(original)); err != nil {
				t.Fatal(err)
			}
			for _, icon := range []Icon{Builder, Game} {
				var result bytes.Buffer
				if err := icon.WriteEXE(&result, bytes.NewReader(seeded.Bytes())); err != nil {
					t.Fatal(err)
				}
				got, err := winres.LoadFromEXE(bytes.NewReader(result.Bytes()))
				if err != nil {
					t.Fatal(err)
				}
				if !bytes.Equal(got.Get(winres.RT_MANIFEST, winres.ID(1), 0), manifest) {
					t.Fatal("lost existing manifest")
				}
				id := winres.ID(1)
				if icon == Builder {
					id = 3
				}
				extracted, err := got.GetIcon(id)
				if err != nil {
					t.Fatal(err)
				}
				var ico bytes.Buffer
				if err := extracted.SaveICO(&ico); err != nil {
					t.Fatal(err)
				}
				if !bytes.Equal(ico.Bytes(), icon.ICO()) {
					t.Fatal("embedded icon changed")
				}
				before, err := pe.NewFile(bytes.NewReader(original))
				if err != nil {
					t.Fatal(err)
				}
				after, err := pe.NewFile(bytes.NewReader(result.Bytes()))
				if err != nil {
					t.Fatal(err)
				}
				if before.Machine != after.Machine {
					t.Fatal("architecture changed")
				}
				beforeCode, _ := before.Section(".text").Data()
				afterCode, _ := after.Section(".text").Data()
				if !bytes.Equal(beforeCode, afterCode) {
					t.Fatal("program code changed")
				}
			}
			if err := Game.ApplyEXE(path); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestInvalidExecutableIsUntouched(t *testing.T) {
	path := filepath.Join(t.TempDir(), "game.exe")
	data := []byte("invalid executable")
	if err := os.WriteFile(path, data, 0755); err != nil {
		t.Fatal(err)
	}
	if err := Game.ApplyEXE(path); err == nil {
		t.Fatal("invalid executable accepted")
	}
	after, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(after, data) {
		t.Fatal("damaged input", err)
	}
}

// Optional smoke test for a maintainer-supplied C/RC compiler output. This also
// checks rewriting an executable that already has a compiler-emitted .rsrc.
func TestResourceCompilerIcon(t *testing.T) {
	path := os.Getenv("AR_ICON_TEST_PE")
	if path == "" {
		t.Skip("set AR_ICON_TEST_PE to the C/RC fixture executable")
	}
	original, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	resources, err := winres.LoadFromEXE(bytes.NewReader(original))
	if err != nil {
		t.Fatal(err)
	}
	icon, err := resources.GetIcon(winres.ID(1))
	if err != nil {
		t.Fatal(err)
	}
	var ico bytes.Buffer
	if err := icon.SaveICO(&ico); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(ico.Bytes(), Game.ICO()) {
		t.Fatal("RC compiler embedded a different icon")
	}
	var rewritten bytes.Buffer
	if err := Game.WriteEXE(&rewritten, bytes.NewReader(original)); err != nil {
		t.Fatal(err)
	}
	if _, err := pe.NewFile(bytes.NewReader(rewritten.Bytes())); err != nil {
		t.Fatal(err)
	}
}
