package textpreview

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"image"
	"image/draw"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

const testManifest = `[pack]
format = actraiser-language-pack
version = 2
id = preview.example
locale = en
name = Preview
autonym = Preview
author = Tests
license = MIT
direction = auto
source_profile = us
target = us-runtime
fallback = native-us
coverage = partial
[fonts]
primary = builtin:actraiser-sans
[scripts]
source = text/example.artext
`

func TestScenarioBounds(t *testing.T) {
	s := Defaults()
	s.Values["master_name"] = "a\x00b"
	if _, err := encodeScenario(s); err == nil {
		t.Fatal("embedded NUL accepted")
	}
	s = Defaults()
	s.Inks["native:invented"] = "#ffffff"
	if _, err := encodeScenario(s); err == nil {
		t.Fatal("unknown ink accepted")
	}
	s = Defaults()
	s.Page = 64
	if _, err := encodeScenario(s); err == nil {
		t.Fatal("out-of-range page accepted")
	}
	s = Defaults()
	s.Values["master_name"] = "Amélie"
	s.Inks["native:hud.body"] = "#FFCC88"
	if _, err := encodeScenario(s); err != nil {
		t.Fatal(err)
	}
}
func TestGamePlayback(t *testing.T) {
	binary := os.Getenv("AR_AUTHOR_TEXT_PREVIEW")
	if binary == "" {
		t.Skip("set AR_AUTHOR_TEXT_PREVIEW to the native test worker or game")
	}
	root, err := filepath.Abs("../../..")
	if err != nil {
		t.Fatal(err)
	}
	builtin := filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf")
	files := fstest.MapFS{"pack.ini": {Data: []byte(testManifest)}, "text/example.artext": {Data: []byte(`@define-style prose band=#B0B0F0 body=#FFFFFF shadow=#101828
:: dialogue.event.relay.aitos
@layout flow
@style prose
A <span scale="120%"><i>mixed</i></span> line.
@wait 8
@line
After the wait.
@page
Second page.
@end
`)}}
	pack, err := lk.LoadAuthorPack(files)
	if err != nil {
		t.Fatal(err)
	}
	before := pack.RuntimeRevision()
	movie, err := Run(context.Background(), binary, builtin, "dialogue.event.relay.aitos", pack, pack, Defaults())
	if err != nil {
		t.Fatal(err)
	}
	if movie.Pages != 2 || movie.Page != 0 || !movie.NativeBounds || movie.Width != 552 || movie.Height != 168 || movie.Source != "text/example.artext" || len(movie.Sheets) == 0 || len(movie.Fonts) == 0 {
		t.Fatal("invalid movie metadata")
	}
	if movie.PackID != "preview.example" || movie.Fallback || movie.Appearance.Style.Treatment != "prose" || len(movie.Treatments) != 1 || movie.Treatments[0].SourceLine != 1 {
		t.Fatal("template treatment trace lost", movie.Treatments)
	}
	wait := -1
	for i, f := range movie.Frames {
		if f.Kind == "wait" {
			wait = i
			break
		}
	}
	if wait < 0 || movie.Frames[wait+1].Tick-movie.Frames[wait].Tick < 8 {
		t.Fatal("authored wait was lost", movie.Frames)
	}
	if movie.Frames[len(movie.Frames)-1].Kind != "page" {
		t.Fatal("page control lost")
	}
	sizes := map[uint32]bool{}
	for _, font := range movie.Fonts {
		sizes[font.Pixels] = true
		if font.Missing {
			t.Fatal("unexpected missing glyph")
		}
	}
	if len(sizes) != 2 {
		t.Fatal("mixed font sizes lost", sizes)
	}
	s := Defaults()
	s.Page = 1
	second, err := Run(context.Background(), binary, builtin, "dialogue.event.relay.aitos", pack, pack, s)
	if err != nil || second.Page != 1 || second.Frames[len(second.Frames)-1].Kind != "end" {
		t.Fatal("page navigation failed", err)
	}
	retainedSizes := map[uint32]bool{}
	for _, font := range second.Fonts {
		retainedSizes[font.Pixels] = true
	}
	if len(retainedSizes) != 2 {
		t.Fatal("previous page styles were not retained", retainedSizes)
	}
	s.Delay = 0
	cleared, err := Run(context.Background(), binary, builtin, "dialogue.event.relay.aitos", pack, pack, s)
	if err != nil {
		t.Fatal(err)
	}
	for _, font := range cleared.Fonts {
		if font.Pixels != cleared.Fonts[0].Pixels {
			t.Fatal("instant-text page did not clear earlier rows")
		}
	}
	if pack.RuntimeRevision() != before {
		t.Fatal("playback mutated source pack")
	}
	raw, _ := json.Marshal(movie)
	if strings.Contains(string(raw), "actraiser-text-preview-") {
		t.Fatal("temporary path escaped diagnostics")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := Run(ctx, binary, builtin, "dialogue.event.relay.aitos", pack, pack, Defaults()); err == nil {
		t.Fatal("cancelled playback completed")
	}
}

func TestGameFixedSurfacePlayback(t *testing.T) {
	binary := os.Getenv("AR_AUTHOR_TEXT_PREVIEW")
	if binary == "" {
		t.Skip("native preview worker unavailable")
	}
	root, _ := filepath.Abs("../../..")
	builtin := filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf")
	keyboard := "Choose a name\n@line\n<span scale=\"120%\">{master_name}</span>\n@line\n--------\n@line\n" +
		"A B C D E F G H I J K L M\n@line\nN O P Q R S T U V W X Y Z\n@line\n" +
		"a b c d e f g h i j k l m\n@line\nn o p q r s t u v w x y z\n@line\n" +
		"0 1 2 3 4 5 6 7 8 9 . {icon.name_entry.backspace} {icon.name_entry.finish}\n@end\n"
	for _, tc := range []struct {
		id, body      string
		width, height int
		placeholders  bool
	}{
		{"world_map.location_label", "{location_name}\n@end\n", 228, 24, false},
		{"action.hud.lives_value", "<i>{hud_value}</i>\n@end\n", 48, 24, false},
		{"action.hud.act_label", "ACT\n@end\n", 144, 24, true},
		{"credits.page_00", "<span color=\"#FFCC00\">C</span>REDITS\n@line\nTranslation team\n@end\n", 768, 624, false},
		{"name_entry.prompt_and_alphabet", keyboard, 0, 0, false},
	} {
		t.Run(tc.id, func(t *testing.T) {
			files := fstest.MapFS{"pack.ini": {Data: []byte(testManifest)}, "text/example.artext": {Data: []byte(":: " + tc.id + "\n" + tc.body)}}
			pack, err := lk.LoadAuthorPack(files)
			if err != nil {
				t.Fatal(err)
			}
			scenario := Defaults()
			if strings.Contains(tc.body, "{location_name}") {
				scenario.Values["location_name"] = "Town 12"
			}
			if strings.Contains(tc.body, "{hud_value}") {
				scenario.Values["hud_value"] = "07"
			}
			if strings.Contains(tc.body, "{master_name}") {
				scenario.Values["master_name"] = "Amélie"
				scenario.Values["icon.name_entry.backspace"] = "icon.name_entry.backspace"
				scenario.Values["icon.name_entry.finish"] = "icon.name_entry.finish"
			}
			movie, err := Run(context.Background(), binary, builtin, tc.id, pack, pack, scenario)
			if err != nil {
				t.Fatal(err)
			}
			if !movie.NativeBounds || len(movie.Frames) != 1 || len(movie.Fonts) == 0 ||
				tc.width != 0 && (movie.Width != tc.width || movie.Height != tc.height) || movie.ArtworkPlaceholders != tc.placeholders {
				t.Fatalf("incorrect fixed surface: %dx%d, native=%v, frames=%d, fonts=%d, placeholders=%v", movie.Width, movie.Height, movie.NativeBounds, len(movie.Frames), len(movie.Fonts), movie.ArtworkPlaceholders)
			}
		})
	}
}

func TestTitleCopyrightHardBreaks(t *testing.T) {
	binary := os.Getenv("AR_AUTHOR_TEXT_PREVIEW")
	if binary == "" {
		t.Skip("native preview worker unavailable")
	}
	root, _ := filepath.Abs("../../..")
	builtin := filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf")
	var previous string
	for _, layout := range []string{"single_line_label", "centered_block", ""} {
		t.Run(layout, func(t *testing.T) {
			body := ":: title.copyright\n"
			if layout != "" {
				body += "@layout " + layout + "\n"
			}
			body += "<i>HHHHHH</i>\n@line\n<span scale=\"80%\">HHH</span>\n@line\nH\n@end\n"
			pack, err := lk.LoadAuthorPack(fstest.MapFS{
				"pack.ini":            {Data: []byte(testManifest)},
				"text/example.artext": {Data: []byte(body)},
			})
			if err != nil {
				t.Fatal(err)
			}
			movie, err := Run(context.Background(), binary, builtin, "title.copyright", pack, pack, Defaults())
			if err != nil {
				t.Fatal(err)
			}
			if !movie.NativeBounds || movie.Width != 768 || movie.Height != 120 || movie.Layout != "centered_block" || len(movie.Frames) != 1 {
				t.Fatalf("wrong title footer bounds/layout: %dx%d %s", movie.Width, movie.Height, movie.Layout)
			}
			frame := movie.Frames[0]
			data, err := base64.StdEncoding.DecodeString(strings.TrimPrefix(movie.Sheets[frame.Sheet], "data:image/png;base64,"))
			if err != nil {
				t.Fatal(err)
			}
			bitmap, err := png.Decode(bytes.NewReader(data))
			if err != nil {
				t.Fatal(err)
			}
			// Verify the actual rendered pixels, not just preserved script ops:
			// each H row has continuous ink, with a blank gap between lines.
			lines, previousInk := 0, false
			for y := int(frame.Index) * movie.Height; y < (int(frame.Index)+1)*movie.Height; y++ {
				left, right := movie.Width, -1
				for x := 0; x < movie.Width; x++ {
					r, g, b, _ := bitmap.At(x, y).RGBA()
					if r>>8 != 18 || g>>8 != 24 || b>>8 != 40 {
						if x < left {
							left = x
						}
						right = x
					}
				}
				ink := right >= left
				if ink && !previousInk {
					lines++
				}
				if ink && (left+right < movie.Width-8 || left+right > movie.Width+8) {
					t.Fatalf("line is not centered: %d..%d", left, right)
				}
				previousInk = ink
			}
			if lines != 3 {
				t.Fatalf("@line rendered %d ink rows, want 3", lines)
			}
			if previous != "" && previous != movie.Sheets[0] {
				t.Fatal("legacy and corrected declarations produced different rendering")
			}
			previous = movie.Sheets[0]
		})
	}
}

func TestAuthoredBreaksAcrossLayouts(t *testing.T) {
	binary := os.Getenv("AR_AUTHOR_TEXT_PREVIEW")
	if binary == "" {
		t.Skip("native preview worker unavailable")
	}
	root, _ := filepath.Abs("../../..")
	builtin := filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf")
	scenario := Defaults()
	scenario.Delay, scenario.Size, scenario.Pixelation, scenario.PixelSize = 0, 100, 0, 0
	for _, id := range []string{"action.stage_name.fillmore", "title.copyright", "dialogue.event.relay.aitos"} {
		t.Run(id, func(t *testing.T) {
			var previous []byte
			for _, separator := range []string{" ", "\n@line\n", "\n@paragraph\n"} {
				body := ":: " + id + "\nHHHHH" + separator + "<i>H</i>\n@end\n"
				pack, err := lk.LoadAuthorPack(fstest.MapFS{
					"pack.ini":            {Data: []byte(testManifest)},
					"text/example.artext": {Data: []byte(body)},
				})
				if err != nil {
					t.Fatal(err)
				}
				movie, err := Run(context.Background(), binary, builtin, id, pack, pack, scenario)
				if err != nil {
					t.Fatalf("separator %q: %v", separator, err)
				}
				pixels := lastFramePixels(t, movie)
				if bytes.Equal(previous, pixels) {
					t.Fatalf("separator %q was flattened in the rendered preview", separator)
				}
				previous = pixels
			}
		})
	}
	// Forty rows cannot fit the title prompt's one-tile region. The preview
	// must explain the overflow, never turn these breaks into spaces to fit.
	pack, err := lk.LoadAuthorPack(fstest.MapFS{
		"pack.ini": {Data: []byte(testManifest)},
		"text/example.artext": {Data: []byte(":: title.start_prompt\n" +
			strings.Repeat("H\n@line\n", 39) + "H\n@end\n")},
	})
	if err != nil {
		t.Fatal(err)
	}
	scenario.Values["icon.ui.selection_pointer"] = "icon.ui.selection_pointer"
	if _, err := Run(context.Background(), binary, builtin, "title.start_prompt", pack, pack, scenario); err == nil || !strings.Contains(err.Error(), "could not be fitted") {
		t.Fatalf("expected a fitting diagnostic, got %v", err)
	}
}

func lastFramePixels(t *testing.T, movie Movie) []byte {
	t.Helper()
	frame := movie.Frames[len(movie.Frames)-1]
	data, err := base64.StdEncoding.DecodeString(strings.TrimPrefix(movie.Sheets[frame.Sheet], "data:image/png;base64,"))
	if err != nil {
		t.Fatal(err)
	}
	sheet, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	bitmap := image.NewRGBA(image.Rect(0, 0, movie.Width, movie.Height))
	draw.Draw(bitmap, bitmap.Bounds(), sheet, image.Pt(0, int(frame.Index)*movie.Height), draw.Src)
	return bitmap.Pix
}

// Windows text-mode stdin interprets 0x1a as EOF and folds CRLF. Exercise both
// in valid binary fields through the real worker, including on Windows runners.
func TestGamePlaybackBinaryScenario(t *testing.T) {
	binary := os.Getenv("AR_AUTHOR_TEXT_PREVIEW")
	if binary == "" {
		t.Skip("set AR_AUTHOR_TEXT_PREVIEW to the native worker or game")
	}
	root, err := filepath.Abs("../../..")
	if err != nil {
		t.Fatal(err)
	}
	pack, err := lk.LoadAuthorPack(fstest.MapFS{
		"pack.ini":            {Data: []byte(testManifest)},
		"text/example.artext": {Data: []byte(":: dialogue.event.relay.aitos\n@layout flow\nA binary scenario.\n@end\n")},
	})
	if err != nil {
		t.Fatal(err)
	}
	scenario := Defaults()
	scenario.Values["master_name"] = strings.Repeat("A", 26)
	scenario.Inks["native:hud.body"] = "#0A0D1A"
	encoded, err := encodeScenario(scenario)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(encoded, []byte{0x1a, 0x0d, 0x0a}) {
		t.Fatal("fixture does not exercise Windows stream translation")
	}
	movie, err := Run(context.Background(), binary, filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf"),
		"dialogue.event.relay.aitos", pack, pack, scenario)
	if err != nil {
		t.Fatal(err)
	}
	if len(movie.Frames) == 0 || movie.Frames[len(movie.Frames)-1].Kind != "end" {
		t.Fatal("binary request did not finish playback")
	}
}

// Use a Unicode TEMP directory as well as a Unicode font path: both the
// worker's file reads and its report/image writes must honor UTF-8 argv.
func TestGamePlaybackUnicodePaths(t *testing.T) {
	binary := os.Getenv("AR_AUTHOR_TEXT_PREVIEW")
	if binary == "" {
		t.Skip("set AR_AUTHOR_TEXT_PREVIEW to the native worker or game")
	}
	builtin := os.Getenv("AR_AUTHOR_PREVIEW_FONT")
	if builtin == "" {
		root, err := filepath.Abs("../../..")
		if err != nil {
			t.Fatal(err)
		}
		builtin = filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf")
	}
	root := filepath.Join(t.TempDir(), "Playback 日本 🎮")
	if err := os.Mkdir(root, 0700); err != nil {
		t.Fatal(err)
	}
	font, err := os.ReadFile(builtin)
	if err != nil {
		t.Fatal(err)
	}
	builtin = filepath.Join(root, "フォント.ttf")
	if err := os.WriteFile(builtin, font, 0600); err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"TMPDIR", "TMP", "TEMP"} {
		t.Setenv(key, root)
	}
	pack, err := lk.LoadAuthorPack(fstest.MapFS{
		"pack.ini":            {Data: []byte(testManifest)},
		"text/example.artext": {Data: []byte(":: dialogue.event.relay.aitos\n@layout flow\nUnicode paths.\n@end\n")},
	})
	if err != nil {
		t.Fatal(err)
	}
	movie, err := Run(context.Background(), binary, builtin, "dialogue.event.relay.aitos", pack, pack, Defaults())
	if err != nil {
		t.Fatal(err)
	}
	if len(movie.Frames) == 0 || len(movie.Sheets) == 0 || movie.Frames[len(movie.Frames)-1].Kind != "end" {
		t.Fatal("Unicode path playback did not produce a complete movie")
	}
	t.Run("report write error", func(t *testing.T) {
		manifest, err := snapshot(filepath.Join(root, "draft"), pack)
		if err != nil {
			t.Fatal(err)
		}
		output := filepath.Join(root, "blocked-output")
		// A directory at the report filename fails the open on every platform,
		// including elevated test runners where read-only permissions do not.
		if err := os.MkdirAll(filepath.Join(output, "report.json"), 0700); err != nil {
			t.Fatal(err)
		}
		input, err := encodeScenario(Defaults())
		if err != nil {
			t.Fatal(err)
		}
		command := exec.CommandContext(t.Context(), binary, "--text-preview-v1", manifest, manifest, builtin,
			"dialogue.event.relay.aitos", output)
		command.Dir = root
		command.Stdin = bytes.NewReader(input)
		diagnostic, err := command.CombinedOutput()
		if err == nil || !strings.Contains(string(diagnostic), "cannot open playback report:") {
			t.Fatalf("report failure did not identify file access: %s (%v)", diagnostic, err)
		}
	})
}
