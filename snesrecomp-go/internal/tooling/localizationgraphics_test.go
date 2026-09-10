package tooling

import (
	"archive/zip"
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

func TestLocalizationGraphicsCommandRejects(t *testing.T) {
	for _, args := range [][]string{nil, {"--rom", "missing"}, {"--out", "x.zip"}, {"--rom", "missing", "--out", "x.zip"}} {
		dir := t.TempDir()
		if err := RunLocalizationGraphicsCommand(args, dir, &bytes.Buffer{}); err == nil {
			t.Fatal("invalid request accepted")
		}
		if _, err := os.Stat(filepath.Join(dir, "x.zip")); !os.IsNotExist(err) {
			t.Fatal("failed extraction left output")
		}
	}
}

func TestLocalizationGraphicsCommandROM(t *testing.T) {
	root := os.Getenv("AR_LOCALIZATION_GUI_ROM_ROOT")
	if root == "" {
		t.Skip("regional ROM fixtures not configured")
	}
	dir := t.TempDir()
	for _, name := range []string{"ar.sfc", "ar-eu.sfc", "ar-fra.sfc", "ar-ger.sfc", "ar-jp.sfc"} {
		out := filepath.Join(dir, name+".zip")
		args := []string{"--rom", filepath.Join(root, name), "--out", out}
		if err := RunLocalizationGraphicsCommand(args, dir, &bytes.Buffer{}); err != nil {
			t.Fatal(err)
		}
		before, err := os.ReadFile(out)
		if err != nil {
			t.Fatal(err)
		}
		archive, err := zip.OpenReader(out)
		if err != nil {
			t.Fatal(err)
		}
		if len(archive.File) != 63 {
			t.Fatal("incomplete graphics archive", len(archive.File))
		}
		_ = archive.Close()
		if err := RunLocalizationGraphicsCommand(args, dir, &bytes.Buffer{}); err == nil {
			t.Fatal("existing output overwritten")
		}
		after, err := os.ReadFile(out)
		if err != nil || !bytes.Equal(before, after) {
			t.Fatal("existing output changed")
		}
	}
}
