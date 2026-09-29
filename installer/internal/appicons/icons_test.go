package appicons

import (
	"bytes"
	"encoding/binary"
	"image/png"
	"os"
	"path/filepath"
	"testing"

	"github.com/tc-hib/winres"
)

func TestPlatformExports(t *testing.T) {
	for _, icon := range []Icon{Builder, Game} {
		t.Run(string(icon), func(t *testing.T) {
			for _, size := range []int{16, 32, 48, 64, 128, 256, 512, 1024} {
				img, err := png.Decode(bytes.NewReader(icon.PNG(size)))
				if err != nil {
					t.Fatal(err)
				}
				if img.Bounds().Dx() != size || img.Bounds().Dy() != size {
					t.Fatal(img.Bounds())
				}
				_, _, _, alpha := img.At(0, 0).RGBA()
				if alpha != 0 {
					t.Fatal("icon background is not transparent")
				}
			}
			ico := icon.ICO()
			if binary.LittleEndian.Uint16(ico[4:6]) != 6 {
				t.Fatal("missing ICO sizes")
			}
			if _, err := winres.LoadICO(bytes.NewReader(ico)); err != nil {
				t.Fatal(err)
			}
			icns := icon.ICNS()
			if string(icns[:4]) != "icns" || int(binary.BigEndian.Uint32(icns[4:8])) != len(icns) {
				t.Fatal("invalid ICNS container")
			}
			sizes := map[string]int{"icp4": 16, "icp5": 32, "icp6": 64, "ic07": 128, "ic08": 256, "ic09": 512, "ic10": 1024, "ic11": 32, "ic12": 64, "ic13": 256, "ic14": 512}
			for offset := 8; offset < len(icns); {
				if offset+8 > len(icns) {
					t.Fatal("truncated ICNS")
				}
				tag := string(icns[offset : offset+4])
				length := int(binary.BigEndian.Uint32(icns[offset+4 : offset+8]))
				if length < 8 || offset+length > len(icns) {
					t.Fatal("invalid ICNS chunk")
				}
				if !bytes.Equal(icns[offset+8:offset+length], icon.PNG(sizes[tag])) {
					t.Fatalf("ICNS %s differs from PNG", tag)
				}
				delete(sizes, tag)
				offset += length
			}
			if len(sizes) != 0 {
				t.Fatal("missing ICNS sizes", sizes)
			}
		})
	}
	if bytes.Equal(Builder.PNG(256), Game.PNG(256)) {
		t.Fatal("Builder and game need distinct icons")
	}
}

func TestAppDirIcons(t *testing.T) {
	for _, icon := range []Icon{Builder, Game} {
		t.Run(string(icon), func(t *testing.T) {
			root := t.TempDir()
			entry := filepath.Join(root, icon.Name()+".desktop")
			if err := os.WriteFile(entry, []byte("[Desktop Entry]\nIcon="+icon.Name()+"\n"), 0644); err != nil {
				t.Fatal(err)
			}
			if err := icon.WriteAppDir(root); err != nil {
				t.Fatal(err)
			}
			for _, size := range []int{16, 32, 48, 64, 128, 256, 512} {
				path := filepath.Join(root, "usr/share/icons/hicolor", fmtSize(size), "apps", icon.Name()+".png")
				data, err := os.ReadFile(path)
				if err != nil || !bytes.Equal(data, icon.PNG(size)) {
					t.Fatal("missing desktop icon", path, err)
				}
			}
			if err := os.Remove(filepath.Join(root, ".DirIcon")); err != nil {
				t.Fatal(err)
			}
			if err := icon.ValidateAppDir(root); err == nil {
				t.Fatal("missing .DirIcon accepted")
			}
			if err := os.WriteFile(filepath.Join(root, ".DirIcon"), []byte("not a PNG"), 0644); err != nil {
				t.Fatal(err)
			}
			if err := icon.ValidateAppDir(root); err == nil {
				t.Fatal("corrupt .DirIcon accepted")
			}
			if err := icon.WriteAppDir(root); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(entry, []byte("[Desktop Entry]\nIcon=wrong\n"), 0644); err != nil {
				t.Fatal(err)
			}
			if err := icon.ValidateAppDir(root); err == nil {
				t.Fatal("mismatched desktop icon accepted")
			}
		})
	}
}

func fmtSize(size int) string {
	return map[int]string{16: "16x16", 32: "32x32", 48: "48x48", 64: "64x64", 128: "128x128", 256: "256x256", 512: "512x512"}[size]
}
