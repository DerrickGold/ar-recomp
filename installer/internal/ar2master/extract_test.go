package ar2master

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

func fixture() []byte {
	data := make([]byte, HeaderBytes+32+MotionBytes+2)
	copy(data, "AR2MASTR")
	binary.LittleEndian.PutUint32(data[8:], 1)
	binary.LittleEndian.PutUint16(data[len(data)-2:], 1)
	for i := 0; i < AnimationCount; i++ {
		data = append(data, 1, 8, 0, 0)
	}
	frame := make([]byte, 15+7+2+32)
	frame[14] = 1
	frame[22] = 32
	data = append(data, frame...)
	seal(data)
	return data
}
func seal(data []byte) {
	binary.LittleEndian.PutUint32(data[12:], uint32(len(data)-HeaderBytes))
	digest := sha256.Sum256(data[HeaderBytes:])
	copy(data[16:], digest[:])
}

func TestPackBoundaries(t *testing.T) {
	data := fixture()
	if err := Validate(data); err != nil {
		t.Fatal(err)
	}
	for n := 0; n < len(data); n++ {
		if Validate(data[:n]) == nil {
			t.Fatalf("accepted truncation at %d", n)
		}
	}
	for _, offset := range []int{0, 8, 12, 16, len(data) - 1} {
		bad := bytes.Clone(data)
		bad[offset] ^= 1
		if Validate(bad) == nil {
			t.Fatalf("accepted corruption at %d", offset)
		}
	}
	for _, change := range []func([]byte){
		func(b []byte) { b[HeaderBytes+32+MotionBytes] = 97 },
		func(b []byte) { b[HeaderBytes+32+MotionBytes+2] = 13 },
		func(b []byte) { b[HeaderBytes+32+MotionBytes+5] = 1 },
		func(b []byte) { b[len(b)-34] = 31 },
		func(b []byte) { b[len(b)-35] = 1 },
	} {
		bad := bytes.Clone(data)
		change(bad)
		seal(bad)
		if Validate(bad) == nil {
			t.Fatal("accepted invalid record with valid digest")
		}
	}
	if _, err := Extract(make([]byte, ROMBytes)); err == nil {
		t.Fatal("accepted wrong ROM")
	}
	if _, err := Extract(make([]byte, ROMBytes+512)); err == nil {
		t.Fatal("accepted wrong headered ROM")
	}
}

func TestInstallAndStatus(t *testing.T) {
	root := t.TempDir()
	if Status(root) != "missing" {
		t.Fatal("new installation not missing")
	}
	if _, err := Install(root, []byte("invalid")); err == nil {
		t.Fatal("accepted invalid input")
	}
	entries, _ := os.ReadDir(root)
	if len(entries) != 0 {
		t.Fatal("invalid pack created files")
	}
	data := fixture()
	if changed, err := Install(root, data); err != nil || !changed {
		t.Fatal(changed, err)
	}
	if changed, err := Install(root, data); err != nil || changed {
		t.Fatal(changed, err)
	}
	if Status(root) != "installed" {
		t.Fatal("not ready")
	}
	path := filepath.Join(root, "game-assets", "imports", Filename)
	if err := os.WriteFile(path, []byte("interrupted old import"), 0600); err != nil {
		t.Fatal(err)
	}
	if Status(root) != "invalid" {
		t.Fatal("damaged pack is ready")
	}
	if _, err := Install(root, data); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	outside := filepath.Join(t.TempDir(), "untouched")
	if err := os.WriteFile(outside, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, path); err != nil {
		t.Skip(err)
	}
	if _, err := Install(root, data); err == nil {
		t.Fatal("accepted symlink destination")
	}
	if got, _ := os.ReadFile(outside); string(got) != "keep" {
		t.Fatal("changed symlink target")
	}
}

func TestLocalROM(t *testing.T) {
	path := os.Getenv("AR2_MASTER_ROM")
	if path == "" {
		t.Skip("optional local ActRaiser 2 ROM integration")
	}
	rom, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	pack, err := Extract(rom)
	if err != nil {
		t.Fatal(err)
	}
	headered, err := Extract(append(make([]byte, 512), rom...))
	if err != nil || !bytes.Equal(pack, headered) {
		t.Fatal("copier-header extraction differs", err)
	}
	rom[0x1234] ^= 1
	if _, err := Extract(rom); err == nil {
		t.Fatal("modified ROM accepted")
	}
	if len(pack) >= ROMBytes/8 {
		t.Fatal("pack includes more than character resources")
	}
	if _, err := Install(t.TempDir(), pack); err != nil {
		t.Fatal(err)
	}
}
