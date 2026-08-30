package tooling

import (
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

func TestRenderROMAndSnapshotCHR(t *testing.T) {
	root := t.TempDir()
	tile := make([]byte, 32)
	for row := 0; row < 8; row++ {
		tile[row*2] = 0x80 // color 1 in the leftmost pixel.
	}
	inputPath := filepath.Join(root, "tiles.bin")
	if err := os.WriteFile(inputPath, tile, 0o644); err != nil {
		t.Fatal(err)
	}
	romOutput := filepath.Join(root, "rom.png")
	report, err := RenderROMCHR(inputPath, 0, 32, 1, romOutput)
	if err != nil || report.Width != 10 || report.Height != 10 {
		t.Fatalf("report=%+v err=%v", report, err)
	}
	file, err := os.Open(romOutput)
	if err != nil {
		t.Fatal(err)
	}
	picture, err := png.Decode(file)
	file.Close()
	if err != nil {
		t.Fatal(err)
	}
	r, _, _, _ := picture.At(1, 1).RGBA()
	if r == 0 {
		t.Fatal("decoded tile pixel remained black")
	}
	cgram := make([]byte, 512)
	cgram[2], cgram[3] = 0x1f, 0x00
	cgramPath := filepath.Join(root, "cgram.bin")
	if err := os.WriteFile(cgramPath, cgram, 0o644); err != nil {
		t.Fatal(err)
	}
	snapshotOutput := filepath.Join(root, "snapshot.png")
	if _, err := RenderSnapshotCHR(inputPath, cgramPath, 0, 0, 1, 1, snapshotOutput); err != nil {
		t.Fatal(err)
	}
}
