package localization

import (
	"bytes"
	"fmt"
	"image/color"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func compareNativePagePixels(t *testing.T, font, page, palette []byte) {
	t.Helper()
	encoded, err := native2bppPage(page, font, palette)
	if err != nil {
		t.Fatal(err)
	}
	probe := os.Getenv("AR_NATIVE_GRAPHICS_PROBE")
	if probe == "" {
		return
	}
	dir := t.TempDir()
	paths := []string{filepath.Join(dir, "font"), filepath.Join(dir, "page"), filepath.Join(dir, "palette")}
	for i, data := range [][]byte{font, page, palette} {
		if err := os.WriteFile(paths[i], data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	want, err := exec.Command(probe, append([]string{"--render-page"}, paths...)...).Output()
	if err != nil {
		t.Fatal(err)
	}
	picture, err := png.Decode(bytes.NewReader(encoded))
	if err != nil {
		t.Fatal(err)
	}
	got := make([]byte, 0, 256*224*4)
	for y := 0; y < 224; y++ {
		for x := 0; x < 256; x++ {
			c := color.NRGBAModel.Convert(picture.At(x, y)).(color.NRGBA)
			got = append(got, c.R, c.G, c.B, c.A)
		}
	}
	if !bytes.Equal(want, got) {
		t.Fatal("native C and Go page pixels differ")
	}
}

func TestNativeGraphicsSynthetic(t *testing.T) {
	font := make([]byte, 4096)
	for i := range font {
		font[i] = byte(i*29 ^ i>>3)
	}
	palette := make([]byte, 32)
	for i := range palette {
		palette[i] = byte(i * 13)
	}
	for variant := 0; variant < 4; variant++ {
		page := make([]byte, endingPageBytes)
		for i := 0; i < 1024; i++ {
			word := i%256 | (i/256)*0x400 | variant*0x4000
			page[i*2], page[i*2+1] = byte(word), byte(word>>8)
		}
		compareNativePagePixels(t, font, page, palette)
	}
	for _, size := range []int{0, 15, 4095, 4097} {
		if out, err := native2bppAtlas(make([]byte, size)); err == nil || out != nil {
			t.Fatal("invalid atlas size")
		}
	}
	if files, err := (*Decoder)(nil).NativeGraphicsFiles(); err == nil || files != nil {
		t.Fatal("nil exporter accepted")
	}
	page := make([]byte, endingPageBytes)
	page[1] = 1 // Tile 256 is outside the resident font.
	if out, err := native2bppPage(page, font, palette); err == nil || out != nil {
		t.Fatal("out-of-range page accepted")
	}
	page[1] = 0x10 // Palette 4 is outside the uploaded credits palette.
	if out, err := native2bppPage(page, font, palette); err == nil || out != nil {
		t.Fatal("out-of-range palette accepted")
	}
}

func TestEndingAssetsRejectIncompleteProducer(t *testing.T) {
	d := catalogTestDecoder(t, make([]byte, 128))
	for _, entries := range [][]assetEntry{nil, {{mode: 8, submode: 1}}, {{mode: 8, submode: 1}, {mode: 8, submode: 1}}} {
		if got, err := d.endingAssets(entries); err == nil || got != nil {
			t.Fatal("incomplete producer accepted")
		}
	}
	for _, command := range []assetCommand{{code: 1}, {code: 0x80}, {code: 0x40}, {code: 8, operands: []byte{0}}, {code: 4}} {
		if got, err := d.endingAssets([]assetEntry{{mode: 8, submode: 1, commands: []assetCommand{command}}}); err == nil || got != nil {
			t.Fatal("changed producer accepted", fmt.Sprintf("%x", command.code))
		}
	}
}
