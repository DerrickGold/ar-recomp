package project

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

func TestNativeLocalizationBuildOptIn(t *testing.T) {
	paths, err := DefaultPaths(t.TempDir()).Resolve()
	if err != nil {
		t.Fatal(err)
	}
	manifest := filepath.Join(paths.Root, ManifestFileName)
	var output bytes.Buffer
	if err := prepareBuildLocalization(paths, manifest, &output); err != nil {
		t.Fatal("legacy CMake projects must remain supported", err)
	}
	if err := os.WriteFile(manifest, []byte("name = OtherGame\nsource = main.c\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := prepareBuildLocalization(paths, manifest, &output); err != nil || output.Len() != 0 {
		t.Fatal("unrelated game required an ActRaiser ROM", err)
	}
	if err := os.WriteFile(manifest, []byte("name = ActRaiserRecomp\nsource = main.c\nlocalization = actraiser-us\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := prepareBuildLocalization(paths, manifest, &output); err == nil {
		t.Fatal("missing US source/ROM accepted")
	}
	if !strings.Contains(output.String(), "Preparing native US language source") {
		t.Fatal("build step omitted")
	}
	if err := os.WriteFile(paths.ROM, []byte("not a ROM"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := prepareBuildLocalization(paths, manifest, &output); err == nil {
		t.Fatal("invalid ROM accepted")
	}
	if _, err := os.Stat(filepath.Join(paths.Root, "game-assets", "languages", "native-us", "pack.ini")); !os.IsNotExist(err) {
		t.Fatal("failed extraction installed a pack")
	}
	for _, file := range []string{"build.go", "hermetic.go"} {
		data, err := os.ReadFile(file)
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(data), "prepareBuildLocalization(paths,") {
			t.Fatal(file, "omits shared source step")
		}
	}
}

func TestNativeLocalizationFreshBuildExtraction(t *testing.T) {
	rom := os.Getenv("AR_LOCALIZATION_BUILD_ROM")
	if rom == "" {
		t.Skip("optional real-ROM build source acceptance")
	}
	paths, err := DefaultPaths(t.TempDir()).Resolve()
	if err != nil {
		t.Fatal(err)
	}
	paths.ROM = rom
	manifest := filepath.Join(paths.Root, ManifestFileName)
	if err := os.WriteFile(manifest, []byte("name = ActRaiserRecomp\nsource = main.c\nlocalization = actraiser-us\n"), 0644); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	if err := prepareBuildLocalization(paths, manifest, &output); err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(paths.Root, "game-assets", "languages", "native-us")
	p, err := localizationkit.OpenAuthorPack(dir)
	if err != nil {
		t.Fatal(err)
	}
	if p.Manifest().Metadata().ID != "native-us" || p.Workspace().Stats().MessageCount != 500 {
		t.Fatal("incorrect source coverage")
	}
	before := p.RuntimeRevision()
	paths.ROM = filepath.Join(paths.Root, "missing.sfc")
	if err := prepareBuildLocalization(paths, manifest, &output); err != nil {
		t.Fatal("existing source not reused", err)
	}
	p, err = localizationkit.OpenAuthorPack(dir)
	if err != nil || p.RuntimeRevision() != before {
		t.Fatal("build replaced local source", err)
	}
}
