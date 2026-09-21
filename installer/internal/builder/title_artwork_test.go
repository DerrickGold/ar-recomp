package builder

import (
	"bytes"
	"context"
	"fmt"
	"image"
	"image/png"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func TestTitleArtworkSharesMedallion(t *testing.T) {
	en, err := png.Decode(bytes.NewReader(titleLogoPNG))
	if err != nil {
		t.Fatal(err)
	}
	ja, err := png.Decode(bytes.NewReader(titleLogoJapanesePNG))
	if err != nil {
		t.Fatal(err)
	}
	// These unobscured regions belong to the shared artwork, not the localized
	// lettering. Check both the bronze relief and the negative-space chips so
	// a future localization cannot silently regenerate a different medallion.
	// Coordinates are on the unpadded master; both canvases add (62, 3).
	for _, region := range []struct {
		name   string
		bounds image.Rectangle
	}{
		{"crown", image.Rect(985, 60, 1145, 100)},
		{"upper-right chip", image.Rect(1220, 220, 1340, 260)},
		{"lower-left chip", image.Rect(875, 675, 910, 695)},
		{"lower rim", image.Rect(960, 703, 1200, 732)},
	} {
		t.Run(region.name, func(t *testing.T) {
			r := region.bounds.Add(image.Pt(62, 3))
			for y := r.Min.Y; y < r.Max.Y; y++ {
				for x := r.Min.X; x < r.Max.X; x++ {
					er, eg, eb, ea := en.At(x, y).RGBA()
					jr, jg, jb, ja := ja.At(x, y).RGBA()
					if er != jr || eg != jg || eb != jb || ea != ja {
						t.Fatalf("shared medallion differs at (%d,%d)", x, y)
					}
				}
			}
		})
	}
}

func TestTitleArtworkDimensionsAndPreview(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
	for _, tc := range []struct {
		variant, endpoint string
	}{{"en", "title-logo.png"}, {"ja", "title-logo-ja.png"}} {
		t.Run(tc.variant, func(t *testing.T) {
			artwork, ok := bundledTitleArtwork(tc.variant)
			if !ok {
				t.Fatal("missing artwork")
			}
			picture, err := png.Decode(bytes.NewReader(artwork))
			if err != nil {
				t.Fatal(err)
			}
			if picture.Bounds().Dx() != 2212 || picture.Bounds().Dy() != 760 {
				t.Fatalf("title canvas = %v; must match the existing title/swirl geometry", picture.Bounds())
			}
			// The host stretches the WHOLE texture into the capture rectangle.
			// 237x95 SNES pixels at 7:6 PAR => 276.5x95 display pixels.
			// An 8x display-space canvas makes X/Y scale exactly equal, for
			// both settled and Mode-7 sampling. Equal source dimensions alone
			// did not catch the old 2087x754 canvas widening the logo by 5.2%.
			logo, swirl := titleManifestValues("hd/test.png")
			for _, values := range [][]manifestValue{logo, swirl} {
				for _, value := range values {
					if value.Key != "rect" && value.Key != "canvas_rect" {
						continue
					}
					var x0, y0, x1, y1 int
					if n, err := fmt.Sscanf(value.Value, "%d,%d,%d,%d", &x0, &y0, &x1, &y1); n != 4 || err != nil {
						t.Fatalf("invalid title bounds: %q", value.Value)
					}
					if picture.Bounds().Dx()*(y1-y0)*6 != picture.Bounds().Dy()*(x1-x0)*7 {
						t.Fatalf("%s %s would distort %v at 7:6 PAR", value.Key, value.Value, picture.Bounds())
					}
				}
			}
			preview := httptest.NewRecorder()
			app.ServeHTTP(preview, httptest.NewRequest(http.MethodGet, "/tok/"+tc.endpoint, nil))
			if preview.Code != http.StatusOK || !bytes.Equal(preview.Body.Bytes(), artwork) {
				t.Fatal("preview differs from installable artwork")
			}
			private := httptest.NewRecorder()
			app.ServeHTTP(private, httptest.NewRequest(http.MethodGet, "/"+tc.endpoint, nil))
			if private.Code != http.StatusNotFound {
				t.Fatal("preview is not session-token gated")
			}
		})
	}
}

func TestTitleArtworkSwitchDisableRestore(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	custom := filepath.Join(root, "game-assets", "hd", "custom-title.png")
	writeAssetTestFile(t, custom, []byte("player's title"))
	// Switching either direction, disabling, and restoring must keep both
	// hooks in agreement and remember the selected style even while disabled.
	for _, step := range []struct {
		variant string
		enabled bool
	}{{"en", true}, {"ja", true}, {"en", true}, {"ja", true}, {"ja", false}, {"ja", true}} {
		fields := map[string]string{"title-change": "1", "title-variant": step.variant}
		if step.enabled {
			fields["title"] = "on"
		}
		response := postAssetForm(t, app, fields, nil)
		if response.Code != http.StatusOK {
			t.Fatalf("save %+v: %d %s", step, response.Code, response.Body.String())
		}
		config, err := loadAssetConfiguration(root)
		if err != nil {
			t.Fatal(err)
		}
		if config.Title.Enabled != step.enabled || config.Title.Variant != step.variant {
			t.Fatalf("step %+v reloaded as %+v", step, config.Title)
		}
		artwork, _ := bundledTitleArtwork(step.variant)
		relative := bundledTitleRelativePath(artwork)
		manifest, path, err := readAssetManifest(root)
		if err != nil {
			t.Fatal(err)
		}
		for _, section := range []string{"replace:title-logo", "replace:title-swirl"} {
			if value, _ := manifestSectionValue(manifest, section, "image"); value != relative {
				t.Fatalf("%s image = %q, want %q", section, value, relative)
			}
		}
		for _, bound := range []struct{ section, key, value string }{
			{"replace:title-logo", "rect", "11,27,248,122"},
			{"replace:title-swirl", "canvas_rect", "139,156,376,251"},
		} {
			if value, _ := manifestSectionValue(manifest, bound.section, bound.key); value != bound.value {
				t.Fatalf("title placement changed: %s = %q", bound.key, value)
			}
		}
		installed, err := os.ReadFile(resolveManifestFile(path, relative))
		if step.enabled {
			if err != nil || !bytes.Equal(installed, artwork) {
				t.Fatalf("incorrect installed title: %v", err)
			}
		} else if !os.IsNotExist(err) {
			t.Fatalf("disabled image still present: %v", err)
		}
	}
	if data, err := os.ReadFile(custom); err != nil || string(data) != "player's title" {
		t.Fatal("custom artwork was changed")
	}
}

func TestTitleArtworkInvalidOrUntouchedSelection(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	response := postAssetForm(t, app, map[string]string{
		"title-change": "1", "title": "on", "title-variant": "ja",
	}, nil)
	if response.Code != http.StatusOK {
		t.Fatal(response.Body.String())
	}
	before, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil {
		t.Fatal(err)
	}
	for _, variant := range []string{"jp", "../../custom", "unknown"} {
		response = postAssetForm(t, app, map[string]string{
			"title-change": "1", "title": "on", "title-variant": variant,
		}, nil)
		if response.Code != http.StatusBadRequest {
			t.Fatalf("accepted unsupported variant %q: %d", variant, response.Code)
		}
	}
	// An unrelated save must not reset the Japanese selection to English.
	response = postAssetForm(t, app, map[string]string{"title-variant": "en"}, nil)
	if response.Code != http.StatusOK {
		t.Fatal(response.Body.String())
	}
	after, err := os.ReadFile(liveAssetManifestPath(root))
	if err != nil || !bytes.Equal(before, after) {
		t.Fatal("invalid or untouched selection changed the live manifest")
	}
}

func TestTitleArtworkBrowserBehavior(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node is unavailable")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if out, err := exec.CommandContext(ctx, node, "--test", "testdata/title_artwork.test.mjs").CombinedOutput(); err != nil {
		t.Fatalf("title artwork browser checks: %v\n%s", err, out)
	}
}
