// Maintainer-only format/size export. Run from installer/internal/appicons:
// go run ./cmd/export. Player packaging consumes the checked-in results.
package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"

	"github.com/tc-hib/winres"
)

func main() {
	for _, name := range []string{"builder", "game"} {
		if err := export(name); err != nil {
			panic(err)
		}
	}
}

func export(name string) error {
	f, err := os.Open(filepath.Join("originals", name+"-16bit.png"))
	if err != nil {
		return err
	}
	source, err := png.Decode(f)
	f.Close()
	if err != nil {
		return err
	}
	if source.Bounds().Dx() != source.Bounds().Dy() {
		return fmt.Errorf("%s must be square", name)
	}
	dir := filepath.Join("assets", name)
	if err = os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	images := map[int]image.Image{}
	pngs := map[int][]byte{}
	for _, size := range []int{16, 32, 48, 64, 128, 256, 512, 1024} {
		// Nearest-neighbor export keeps the requested pixel-art edges crisp.
		img := image.NewNRGBA(image.Rect(0, 0, size, size))
		bounds := source.Bounds()
		for y := 0; y < size; y++ {
			for x := 0; x < size; x++ {
				img.Set(x, y, source.At(bounds.Min.X+(2*x+1)*bounds.Dx()/(2*size), bounds.Min.Y+(2*y+1)*bounds.Dy()/(2*size)))
			}
		}
		var b bytes.Buffer
		if err = png.Encode(&b, img); err != nil {
			return err
		}
		images[size], pngs[size] = img, b.Bytes()
		if err = os.WriteFile(filepath.Join(dir, fmt.Sprintf("%d.png", size)), b.Bytes(), 0644); err != nil {
			return err
		}
	}
	var icoImages []image.Image
	for _, size := range []int{16, 32, 48, 64, 128, 256} {
		icoImages = append(icoImages, images[size])
	}
	icon, err := winres.NewIconFromImages(icoImages)
	if err != nil {
		return err
	}
	var ico bytes.Buffer
	if err = icon.SaveICO(&ico); err != nil {
		return err
	}
	if err = os.WriteFile(filepath.Join(dir, "icon.ico"), ico.Bytes(), 0644); err != nil {
		return err
	}
	var chunks bytes.Buffer
	for _, entry := range []struct {
		tag  string
		size int
	}{
		{"icp4", 16}, {"icp5", 32}, {"icp6", 64}, {"ic07", 128}, {"ic08", 256}, {"ic09", 512}, {"ic10", 1024},
		{"ic11", 32}, {"ic12", 64}, {"ic13", 256}, {"ic14", 512},
	} {
		data := pngs[entry.size]
		chunks.WriteString(entry.tag)
		binary.Write(&chunks, binary.BigEndian, uint32(len(data)+8))
		chunks.Write(data)
	}
	var icns bytes.Buffer
	icns.WriteString("icns")
	binary.Write(&icns, binary.BigEndian, uint32(chunks.Len()+8))
	icns.Write(chunks.Bytes())
	return os.WriteFile(filepath.Join(dir, "icon.icns"), icns.Bytes(), 0644)
}
