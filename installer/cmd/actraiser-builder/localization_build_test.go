package main

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

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
	pack, openErr := localization.OpenNativeUSSource(filepath.Join(root, "game-assets", "languages", "native-us"))
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
