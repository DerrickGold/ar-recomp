package rom

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadStripsCopierHeader(t *testing.T) {
	path := filepath.Join(t.TempDir(), "headered.sfc")
	data := append(make([]byte, 512), make([]byte, 32*1024)...)
	data[512] = 0x42
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatal(err)
	}
	got, err := Load(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 32*1024 || got[0] != 0x42 {
		t.Fatalf("header was not stripped: len=%d first=%02x", len(got), got[0])
	}
}

func TestLoROMOffsetMirrorsHighBanks(t *testing.T) {
	low, err := LoROMOffset(0x03, 0x9234)
	if err != nil {
		t.Fatal(err)
	}
	high, err := LoROMOffset(0x83, 0x9234)
	if err != nil {
		t.Fatal(err)
	}
	if low != high {
		t.Fatalf("LoROM mirrors differ: low=%x high=%x", low, high)
	}
}
