package romset

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2game"
	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
	"github.com/DerrickGold/ar-recomp/installer/internal/regionalmedia"
)

func TestRejectIncompleteOrUnknownSelection(t *testing.T) {
	for _, inputs := range [][]Input{nil, make([]Input, 7), {{Name: "ar2.sfc", Data: []byte("not a ROM")}}} {
		if _, err := Prepare(inputs); err == nil {
			t.Fatal("invalid selection accepted")
		}
	}
}

func TestLocalMixedROMBuild(t *testing.T) {
	directory := os.Getenv("AR_ROM_SET_DIR")
	if directory == "" {
		t.Skip("set AR_ROM_SET_DIR to a local ROM fixture directory")
	}
	var inputs []Input
	for _, name := range []string{"ar2.sfc", "ar-jp.sfc", "ar-fra.sfc", "ar.sfc", "ar-eu.sfc", "ar-ger.sfc"} {
		data, err := os.ReadFile(filepath.Join(directory, name))
		if err != nil {
			t.Fatal(err)
		}
		// Copier headers and misleading filenames cannot change identity.
		inputs = append(inputs, Input{Name: "selected.smc", Data: append(make([]byte, 512), data...)})
	}
	plan, err := Prepare(inputs)
	if err != nil {
		t.Fatal(err)
	}
	baseline, _ := os.ReadFile(filepath.Join(directory, "ar.sfc"))
	if !bytes.Equal(plan.Baseline, baseline) || len(plan.Releases) != 6 {
		t.Fatal("wrong baseline or missing release")
	}
	root := t.TempDir()
	if err := plan.Install(root); err != nil {
		t.Fatal(err)
	}
	if err := plan.Install(root); err != nil {
		t.Fatal("repeated import:", err)
	}
	if ar2game.Status(root) != "installed" || ar2master.Status(root) != "installed" {
		t.Fatal("AR2 imports missing")
	}
	installed, err := regionalmedia.ListInstalled(root)
	if err != nil || len(installed) != 5 {
		t.Fatal("regional assets:", installed, err)
	}
	projects, _ := filepath.Glob(filepath.Join(root, "game-assets/languages/projects/*.arproject"))
	if len(projects) != 4 {
		t.Fatalf("got %d regional text sources", len(projects))
	}
	if _, err := Prepare([]Input{inputs[0], {Name: "bad.sfc", Data: []byte("bad")}}); err == nil {
		t.Fatal("partial invalid selection accepted")
	}
	duplicate, err := Prepare([]Input{inputs[0], inputs[0], inputs[3]})
	if err != nil || len(duplicate.Releases) != 2 {
		t.Fatal("duplicate ROM handling:", err)
	}
}

func TestLocalAR1OnlyBuild(t *testing.T) {
	directory := os.Getenv("AR_ROM_SET_DIR")
	if directory == "" {
		t.Skip("set AR_ROM_SET_DIR to a local ROM fixture directory")
	}
	rom, err := os.ReadFile(filepath.Join(directory, "ar.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	plan, err := Prepare([]Input{{Name: "ar.sfc", Data: rom}})
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(plan.Baseline, rom) || len(plan.Releases) != 1 || plan.master != nil || plan.world != nil {
		t.Fatal("AR1-only selection acquired optional content")
	}
	root := t.TempDir()
	if err := plan.Install(root); err != nil {
		t.Fatal(err)
	}
	if ar2game.Status(root) != "missing" || ar2master.Status(root) != "missing" {
		t.Fatal("AR1-only install created AR2 imports")
	}
	media, err := regionalmedia.ListInstalled(root)
	if err != nil || len(media) == 0 {
		t.Fatal("AR1 regional media missing", media, err)
	}
	for _, donor := range media {
		want := "missing"
		if donor.Release == "us" {
			want = "installed"
		}
		if donor.Status != want {
			t.Fatalf("unexpected regional media: %+v", donor)
		}
	}
}
