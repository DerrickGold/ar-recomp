package localization

import (
	"bytes"
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

func TestEndingAssetSceneROM(t *testing.T) {
	root := os.Getenv("AR_LOCALIZATION_GUI_ROM_ROOT")
	if root == "" {
		t.Skip("regional ROM fixtures not configured")
	}
	for _, fixture := range []struct {
		name                                string
		scene, font, pages, presenter, hold int
	}{
		{"ar.sfc", 0x28e1a, 0xd1b6f, 0x3d1a0, 0x02ab30, 354},
		{"ar-eu.sfc", 0x28e1a, 0xd14b2, 0x3d1a0, 0x02abc9, 286},
		{"ar-ger.sfc", 0x28e1a, 0xd06f7, 0x3d1a0, 0x02abd2, 286},
		{"ar-fra.sfc", 0x28e1a, 0xd0000, 0x3d1a0, 0x02abbb, 286},
		{"ar-jp.sfc", 0x28e14, 0xd1a2b, 0x54549, 0x02a876, 382},
	} {
		t.Run(fixture.name, func(t *testing.T) {
			rom, err := os.ReadFile(filepath.Join(root, fixture.name))
			if err != nil {
				t.Fatal(err)
			}
			d, err := NewDecoder(rom)
			if err != nil {
				t.Fatal(err)
			}
			entries, err := d.assetScript()
			if err != nil {
				t.Fatal(err)
			}
			assets, err := d.endingAssets(entries)
			if err != nil {
				t.Fatal(err)
			}
			if len(assets.pages) != 20*0x800 {
				t.Fatal("incomplete ending pages")
			}
			evidence, err := d.endingGraphicalSurface()
			if err != nil {
				t.Fatal(err)
			}
			if assets.sceneStart != fixture.scene || assets.fontSource != fixture.font || assets.pageSource != fixture.pages ||
				irString(evidence["copy_routine"].(IRObject), "entry_pc24") != pcString(fixture.presenter) ||
				evidence["timing"].(IRObject)["hold_frames"] != fixture.hold {
				t.Fatal("reviewed credits source/timing changed")
			}
			last := entries[len(entries)-1]
			for index, command := range last.commands {
				if command.code == 2 {
					continue
				}
				missing := last
				missing.commands = append(append([]assetCommand{}, last.commands[:index]...), last.commands[index+1:]...)
				duplicate := last
				duplicate.commands = append(append([]assetCommand{}, last.commands...), command)
				for _, changed := range []assetEntry{missing, duplicate} {
					if out, err := d.endingAssets([]assetEntry{changed}); err == nil || out != nil {
						t.Fatal("incomplete/ambiguous credits producer accepted")
					}
				}
			}
			for page := 0; page < endingPageCount; page++ {
				compareNativePagePixels(t, assets.font, assets.pages[page*endingPageBytes:(page+1)*endingPageBytes], assets.palette)
			}
			// All native font tiles, not just characters which happen to
			// appear in the credits. Include each palette and both flips.
			for variant := 0; variant < 4; variant++ {
				page := make([]byte, endingPageBytes)
				for cell := 0; cell < 1024; cell++ {
					v := cell%256 | variant*0x4000
					page[cell*2], page[cell*2+1] = byte(v), byte(v>>8)
				}
				compareNativePagePixels(t, assets.font, page, assets.palette)
			}
			font, err := d.dialogFont(entries)
			if err != nil {
				t.Fatal(err)
			}
			if font["source_file_offset"] == fileOffset(assets.fontSource) {
				t.Fatal("credits aliased dialogue alphabet")
			}
			files, err := d.NativeGraphicsFiles()
			if err != nil {
				t.Fatal(err)
			}
			for variant := 0; variant < 4; variant++ {
				page := make([]byte, endingPageBytes)
				for cell := 0; cell < 1024; cell++ {
					v := cell%256 | variant*0x4000
					page[cell*2], page[cell*2+1] = byte(v), byte(v>>8)
				}
				compareNativePagePixels(t, files["dialogue.2bpp"], page, assets.palette)
			}
			for _, path := range []string{"dialogue.png", "credits.png", "credits/page-00.png", "credits/page-19.png"} {
				picture, err := png.Decode(bytes.NewReader(files[path]))
				if err != nil {
					t.Fatalf("%s: %v", path, err)
				}
				if picture.Bounds().Dx() != 128 && picture.Bounds().Dx() != 256 {
					t.Fatal("invalid atlas geometry")
				}
			}
			if out := os.Getenv("AR_LOCALIZATION_GRAPHICS_TEST_OUTPUT"); out != "" {
				for path, data := range files {
					path = filepath.Join(out, d.ReleaseID(), path)
					if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
						t.Fatal(err)
					}
					if err := os.WriteFile(path, data, 0600); err != nil {
						t.Fatal(err)
					}
				}
			}
			t.Logf("%s: %d bounded files, scene %x, font %x, pages %x", d.ReleaseID(), len(files), assets.sceneStart, assets.fontSource, assets.pageSource)
		})
	}
}
