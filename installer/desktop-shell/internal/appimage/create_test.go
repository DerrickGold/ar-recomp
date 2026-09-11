package appimage

import (
	"bytes"
	"debug/elf"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func fixtureRuntime(t *testing.T, path string, machine elf.Machine) []byte {
	t.Helper()
	data := make([]byte, 64)
	copy(data, "\x7fELF\x02\x01\x01\x00AI\x02")
	binary.LittleEndian.PutUint16(data[16:], 3)
	binary.LittleEndian.PutUint16(data[18:], uint16(machine))
	binary.LittleEndian.PutUint32(data[20:], 1)
	binary.LittleEndian.PutUint16(data[52:], 64)
	if err := os.WriteFile(path, data, 0644); err != nil {
		t.Fatal(err)
	}
	return data // A non-executable ELF fixture: tests never run target code.
}

func TestNativeSquashFSRoundTrip(t *testing.T) {
	for _, tool := range []string{"mksquashfs", "unsquashfs"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skip(tool + " not installed")
		}
	}
	dir := t.TempDir()
	app := filepath.Join(dir, "App Dir")
	if err := os.Mkdir(app, 0755); err != nil {
		t.Fatal(err)
	}
	payload := []byte("offline payload\n")
	if err := os.WriteFile(filepath.Join(app, "data"), payload, 0644); err != nil {
		t.Fatal(err)
	}
	runtime := filepath.Join(dir, "Linux runtime")
	original := fixtureRuntime(t, runtime, elf.EM_AARCH64)
	out := filepath.Join(dir, "Builder.AppImage")
	if err := Create(app, runtime, out, "arm64"); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(data[:64], original) || string(data[64:68]) != "hsqs" {
		t.Fatal("wrong runtime/filesystem boundary")
	}
	contents, err := exec.Command("unsquashfs", "-o", "64", "-cat", out, "data").Output()
	if err != nil || !bytes.Equal(contents, payload) {
		t.Fatalf("native extraction: %q, %v", contents, err)
	}
	unchanged, _ := os.ReadFile(runtime)
	if !bytes.Equal(unchanged, original) {
		t.Fatal("runtime modified")
	}
	if err := Create(app, runtime, out, "arm64"); err == nil {
		t.Fatal("overwrote existing image")
	}
	if err := Create(app, runtime, filepath.Join(dir, "wrong.AppImage"), "amd64"); err == nil {
		t.Fatal("mixed architecture accepted")
	}
	data[8] = 0
	if err := os.WriteFile(runtime, data[:64], 0644); err != nil {
		t.Fatal(err)
	}
	if err := Create(app, runtime, filepath.Join(dir, "bad.AppImage"), "arm64"); err == nil {
		t.Fatal("invalid runtime accepted")
	}
}
