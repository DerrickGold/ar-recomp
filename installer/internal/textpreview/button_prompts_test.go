package textpreview

import (
	"bytes"
	"context"
	"encoding/base64"
	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"
)

func TestButtonPromptPresentation(t *testing.T) {
	s := Defaults()
	s.Values["icon.button.b"] = "button.glyph.playstation.cross"
	text, err := encodeScenario(s)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(text, []byte("text:Cross")) || bytes.Contains(text, []byte("button.glyph.")) {
		t.Fatal("Text mode did not convert glyph")
	}
	s.ButtonGlyphs = true
	glyph, err := encodeScenario(s)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(glyph, []byte("button.glyph.playstation.cross")) {
		t.Fatal("Glyph mode lost descriptor")
	}
	s.Values["icon.button.b"] = "text:F1"
	glyph, err = encodeScenario(s)
	if err != nil || !bytes.Contains(glyph, []byte("text:F1")) {
		t.Fatal("keyboard fallback lost", err)
	}
}

func TestButtonPromptPlayback(t *testing.T) {
	worker := os.Getenv("AR_AUTHOR_TEXT_PREVIEW")
	if worker == "" {
		t.Skip("native preview worker unavailable")
	}
	root, err := filepath.Abs("../../..")
	if err != nil {
		t.Fatal(err)
	}
	pack, err := lk.LoadAuthorPack(fstest.MapFS{"pack.ini": {Data: []byte(testManifest)}, "text/example.artext": {Data: []byte(":: sim.help.category.2\nPress {icon.button.b} or {icon.button.y}.\n@end\n")}})
	if err != nil {
		t.Fatal(err)
	}
	font := filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf")
	for _, family := range []string{"xbox", "playstation", "nintendo"} {
		for _, mode := range []bool{false, true} {
			scenario := Defaults()
			scenario.ButtonGlyphs = mode
			first, second := "a", "x"
			if family == "playstation" {
				first, second = "cross", "square"
			}
			if family == "nintendo" {
				first, second = "b", "y"
			}
			scenario.Values["icon.button.b"] = "button.glyph." + family + "." + first
			scenario.Values["icon.button.y"] = "button.glyph." + family + "." + second
			movie, err := Run(context.Background(), worker, font, "sim.help.category.2", pack, pack, scenario)
			if err != nil {
				t.Fatalf("%s glyph=%v: %v", family, mode, err)
			}
			if mode && family == "xbox" {
				data, err := base64.StdEncoding.DecodeString(strings.TrimPrefix(movie.Sheets[len(movie.Sheets)-1], "data:image/png;base64,"))
				if err != nil {
					t.Fatal(err)
				}
				bitmap, err := png.Decode(bytes.NewReader(data))
				if err != nil {
					t.Fatal(err)
				}
				coloured := false
				for y := bitmap.Bounds().Min.Y; y < bitmap.Bounds().Max.Y && !coloured; y++ {
					for x := bitmap.Bounds().Min.X; x < bitmap.Bounds().Max.X; x++ {
						r, g, b, a := bitmap.At(x, y).RGBA()
						if a > 0 && g > r*3/2 && g > b*3/2 {
							coloured = true
							break
						}
					}
				}
				if !coloured {
					t.Fatal("Xbox A artwork absent from rendered playback")
				}
			}
			if len(movie.Frames) == 0 || len(movie.Sheets) == 0 {
				t.Fatal("empty button preview")
			}
		}
	}
}
