package ar2game

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

func TestRejectMalformedWorldPacksWithoutWriting(t *testing.T) {
	root := t.TempDir()
	for _, data := range [][]byte{nil, []byte("AR2WORLD"), make([]byte, MaximumBytes), make([]byte, MaximumBytes+1)} {
		if _, err := Install(root, data); err == nil {
			t.Fatal("invalid pack installed")
		}
	}
	if Status(root) != "missing" {
		t.Fatal("invalid import changed status")
	}
	entries, _ := os.ReadDir(root)
	if len(entries) != 0 {
		t.Fatal("invalid import created files")
	}
}

func TestLocalWorldPackRoundTrip(t *testing.T) {
	directory := os.Getenv("AR_ROM_SET_DIR")
	if directory == "" {
		t.Skip("set AR_ROM_SET_DIR for local ROM validation")
	}
	rom, err := os.ReadFile(filepath.Join(directory, "ar2.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	pack, err := Extract(append(make([]byte, 512), rom...))
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	if changed, err := Install(root, pack); !changed || err != nil {
		t.Fatal(changed, err)
	}
	if changed, err := Install(root, pack); changed || err != nil {
		t.Fatal(changed, err)
	}
	got, err := ReadROM(root)
	if err != nil || !bytes.Equal(got, rom) {
		t.Fatal("ROM data changed:", err)
	}
	for _, offset := range []int{0, 8, 12, 16, HeaderBytes, len(pack) - 1} {
		bad := append([]byte(nil), pack...)
		bad[offset] ^= 1
		if _, err := Install(root, bad); err == nil {
			t.Fatal("tampered pack accepted")
		}
		got, err = ReadROM(root)
		if err != nil || !bytes.Equal(got, rom) {
			t.Fatal("failed replacement damaged prior content")
		}
	}
}
