package regionalmedia

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestInstallReadIsNonMutatingAndRejectsInvalidData(t *testing.T) {
	root := t.TempDir()
	rows, err := ListInstalled(root)
	if err != nil || len(rows) != 5 {
		t.Fatal(rows, err)
	}
	for _, row := range rows {
		if row.Status != "missing" || len(row.Resources) != 0 {
			t.Fatal(row)
		}
	}
	if _, _, err = Install(root, []byte("not a donor"), true); err == nil {
		t.Fatal("invalid package installed")
	}
	entries, err := os.ReadDir(root)
	if err != nil || len(entries) != 0 {
		t.Fatal("read/rejected upload created files", entries, err)
	}
}
func TestInstallConfinesDirectoriesAndPreservesExisting(t *testing.T) {
	root, outside := t.TempDir(), t.TempDir()
	if err := os.Symlink(outside, filepath.Join(root, "game-assets")); err != nil {
		t.Skip(err)
	}
	if _, err := openInstallRoot(root, true); err == nil {
		t.Fatal("symlink directory accepted")
	}
	entries, _ := os.ReadDir(outside)
	if len(entries) != 0 {
		t.Fatal("escaped root")
	}
	root = t.TempDir()
	dir, err := openInstallRoot(root, true)
	if err != nil {
		t.Fatal(err)
	}
	defer dir.Close()
	// Exercise publication independently of copyrighted package validation.
	if err = publishDonor(dir, "jp.armedia", []byte("previous"), false); err != nil {
		t.Fatal(err)
	}
	if err = publishDonor(dir, "jp.armedia", []byte("next"), false); !errors.Is(err, ErrReplaceRequired) {
		t.Fatal(err)
	}
	data, err := readInstalled(dir, "jp.armedia")
	if err != nil || string(data) != "previous" {
		t.Fatal("prior donor lost", err)
	}
	if err = publishDonor(dir, "jp.armedia", []byte("next"), true); err != nil {
		t.Fatal(err)
	}
	data, err = readInstalled(dir, "jp.armedia")
	if err != nil || string(data) != "next" {
		t.Fatal(err)
	}
	entries, _ = os.ReadDir(dir.Name())
	if len(entries) != 1 {
		t.Fatal("staging leak", entries)
	}
	if err = os.Symlink(filepath.Join(dir.Name(), "jp.armedia"), filepath.Join(dir.Name(), "us.armedia")); err != nil {
		t.Fatal(err)
	}
	if _, err = readInstalled(dir, "us.armedia"); err == nil {
		t.Fatal("symlink donor read")
	}
	rows, err := ListInstalled(root)
	if err != nil || rows[0].Status != "invalid" || rows[1].Status != "invalid" {
		t.Fatal(rows, err)
	}
}
func TestROMInstallationRoundtrip(t *testing.T) {
	romRoot := os.Getenv("AR_MEDIA_ROM_DIR")
	if romRoot == "" {
		t.Skip("optional real donor installation")
	}
	root := t.TempDir()
	for _, name := range []string{"ar.sfc", "ar-jp.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc"} {
		rom, err := os.ReadFile(filepath.Join(romRoot, name))
		if err != nil {
			t.Fatal(err)
		}
		extraction, err := Extract(rom)
		if err != nil {
			t.Fatal(err)
		}
		data, err := Pack(extraction)
		if err != nil {
			t.Fatal(err)
		}
		release, changed, err := Install(root, data, false)
		if err != nil || !changed || release != extraction.Release.ID {
			t.Fatal(release, changed, err)
		}
		_, changed, err = Install(root, data, false)
		if err != nil || changed {
			t.Fatal("identical install", changed, err)
		}
		path := filepath.Join(root, "game-assets", "regions", release+".armedia")
		if err = os.WriteFile(path, []byte("old incompatible package"), 0600); err != nil {
			t.Fatal(err)
		}
		_, changed, err = Install(root, data, false)
		if !errors.Is(err, ErrReplaceRequired) || changed {
			t.Fatal("unconfirmed replacement", err)
		}
		if prior, _ := os.ReadFile(path); string(prior) != "old incompatible package" {
			t.Fatal("old bytes lost")
		}
		_, changed, err = Install(root, data, true)
		if err != nil || !changed {
			t.Fatal(err)
		}
		got, _ := os.ReadFile(path)
		if !bytes.Equal(got, data) {
			t.Fatal("installed data differs")
		}
		if info, _ := os.Stat(path); runtime.GOOS != "windows" && info.Mode().Perm()&0077 != 0 {
			t.Fatal("private media permissions", info.Mode())
		}
	}
	rows, err := ListInstalled(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, row := range rows {
		if row.Status != "installed" || len(row.Resources) == 0 {
			t.Fatal(row)
		}
	}
	// A valid donor under the wrong filename must not be advertised as usable.
	us, _ := os.ReadFile(filepath.Join(root, "game-assets", "regions", "us.armedia"))
	if err = os.WriteFile(filepath.Join(root, "game-assets", "regions", "jp.armedia"), us, 0600); err != nil {
		t.Fatal(err)
	}
	rows, err = ListInstalled(root)
	if err != nil || rows[1].Status != "invalid" {
		t.Fatal(rows, err)
	}
}
