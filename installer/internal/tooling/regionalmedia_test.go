package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/regionalmedia"
)

func TestRegionalMediaCommand(t *testing.T) {
	dir := t.TempDir()
	var output bytes.Buffer
	if err := RunRegionalMediaCommand(nil, dir, &output); err == nil {
		t.Fatal("missing options accepted")
	}
	if err := os.WriteFile(filepath.Join(dir, "bad.sfc"), make([]byte, 1<<20), 0600); err != nil {
		t.Fatal(err)
	}
	if err := RunRegionalMediaCommand([]string{"--rom", "bad.sfc", "--out", "out.armedia"}, dir, &output); err == nil {
		t.Fatal("unknown donor accepted")
	}
	if _, err := os.Stat(filepath.Join(dir, "out.armedia")); !os.IsNotExist(err) {
		t.Fatal("failed extraction wrote output", err)
	}
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		return
	}
	args := []string{"--rom", filepath.Join(root, "ar-jp.sfc"), "--out", "out.armedia"}
	if err := RunRegionalMediaCommand(args, dir, &output); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "out.armedia")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	pack, err := regionalmedia.Unpack(data)
	if err != nil || pack.Release.ID != "jp" {
		t.Fatal(err)
	}
	if !bytes.Contains(output.Bytes(), []byte("do not redistribute")) {
		t.Fatal("missing local-only notice")
	}
	if err := RunRegionalMediaCommand(args, dir, &output); err == nil {
		t.Fatal("overwrote existing package")
	}
	again, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(data, again) {
		t.Fatal("modified existing package", err)
	}
}
