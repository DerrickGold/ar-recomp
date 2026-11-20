package main

import (
	"bytes"
	"context"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/builder"
	"github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

func TestGUIExtractsNativeSourceBeforeBuildDependencies(t *testing.T) {
	rom := os.Getenv("AR_LOCALIZATION_BUILD_ROM")
	if rom == "" {
		t.Skip("optional real-ROM early extraction acceptance")
	}
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "snesbuild.ini"), []byte("name = ActRaiserRecomp\nsource = main.c\n"), 0644); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	_, err := buildFromGUI(context.Background(), guiFlags{
		toolchainDir: "missing-tools", snesbuild: filepath.Join(root, "missing-snesbuild"),
	}, root, root, rom, &output)
	if err == nil {
		t.Fatal("missing build dependencies should fail after extraction")
	}
	nativeDir := filepath.Join(root, "game-assets", "languages", "native-us")
	pack, openErr := localization.OpenNativeUSSource(nativeDir)
	if openErr != nil || pack == nil {
		t.Fatal("later build failure prevented extraction", openErr, output.String())
	}
	if !strings.Contains(output.String(), "=== Preparing native US language source ===") ||
		!strings.Contains(err.Error(), "missing-snesbuild") {
		t.Fatalf("expected extraction before compiler discovery: %v\n%s", err, output.String())
	}
	archivePath := filepath.Join(root, "game-assets", "mods", ".bundled", "native-us.o2r")
	archive, err := localization.OpenNativeUSMod(archivePath)
	if err != nil || !slices.Equal(slices.Sorted(maps.Values(archive.Workspace().Sources())), slices.Sorted(maps.Values(pack.Workspace().Sources()))) {
		t.Fatalf("generated native archive doesn't match the unpacked baseline: %v", err)
	}
	raw, err := os.ReadFile(archivePath)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := localization.ReadLanguageMod(bytes.NewReader(raw), int64(len(raw))); err == nil {
		t.Fatal("the required baseline must stay reserved for the generated native slot")
	}
	if err := os.Rename(nativeDir, nativeDir+"-backup"); err != nil {
		t.Fatal(err)
	}
	if err := builder.PrepareRuntimeLanguages(context.Background(), root, "", &output); err != nil {
		t.Fatal(err)
	}
	restored, err := localization.OpenNativeUSSource(nativeDir)
	if err != nil || restored == nil || restored.Manifest().Metadata().ID != "native-us" {
		t.Fatalf("the generated archive didn't restore the runtime baseline: %v", err)
	}
}
