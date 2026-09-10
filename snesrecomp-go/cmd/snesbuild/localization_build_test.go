package main

import (
	"bytes"
	"context"
	"flag"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

func TestGUIExtractsNativeSourceBeforeBuildDependencies(t *testing.T) {
	rom := os.Getenv("AR_LOCALIZATION_BUILD_ROM")
	if rom == "" {
		t.Skip("optional real-ROM early extraction acceptance")
	}
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "snesbuild.ini"), []byte("name = ActRaiserRecomp\nsource = main.c\nlocalization = actraiser-us\n"), 0644); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	_, err := buildFromGUI(context.Background(), guiFlags{toolchainDir: "missing-tools"}, root, root, rom, &output)
	if err == nil {
		t.Fatal("missing build dependencies should fail after extraction")
	}
	pack, openErr := localizationkit.OpenNativeUSSource(filepath.Join(root, "game-assets", "languages", "native-us"))
	if openErr != nil || pack == nil {
		t.Fatal("later build failure prevented extraction", openErr, output.String())
	}
	if !strings.HasPrefix(strings.TrimSpace(output.String()), "=== Preparing native US language source ===") {
		t.Fatal("extraction was not first", output.String())
	}
	if pack.Workspace().Stats().MessageCount != 495 {
		t.Fatal("incomplete baseline")
	}
}

func TestBuildCarriesSelectedROMIntoSourceExtraction(t *testing.T) {
	flags := flag.NewFlagSet("build", flag.ContinueOnError)
	values := addBuildFlags(flags)
	root := t.TempDir()
	if err := flags.Parse([]string{"--root", root, "--rom", "roms/local copy.sfc", "--zig", "unused-test-zig"}); err != nil {
		t.Fatal(err)
	}
	plain, err := values.options().Paths.Resolve()
	if err != nil {
		t.Fatal(err)
	}
	hermetic, err := values.hermeticOptions()
	if err != nil {
		t.Fatal(err)
	}
	resolved, err := hermetic.Paths.Resolve()
	if err != nil {
		t.Fatal(err)
	}
	want := filepath.Join(root, "roms", "local copy.sfc")
	if plain.ROM != want || resolved.ROM != want {
		t.Fatal("source extraction lost the selected ROM", plain.ROM, resolved.ROM)
	}
}

func TestTtfPinSelectorsAreExclusive(t *testing.T) {
	for _, pair := range [][2]string{{"--sdl", "--sdl-ttf"}, {"--steam-deck-sdl", "--steam-deck-sdl-ttf"}, {"--sdl-ttf", "--steam-deck-sdl-ttf"}} {
		if err := runToolchain([]string{"pin", pair[0], pair[1]}); err == nil {
			t.Fatal("ambiguous pins accepted", pair)
		}
	}
}
