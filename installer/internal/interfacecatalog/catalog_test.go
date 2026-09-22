package interfacecatalog

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"
)

func TestCatalogAndLocale(t *testing.T) {
	entries, err := Entries()
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) == 0 {
		t.Fatal("empty catalog")
	}
	for i, entry := range entries {
		if i > 0 && entries[i-1].Key >= entry.Key {
			t.Fatal("catalog not sorted")
		}
		for _, text := range entry.Text {
			if text == "" {
				t.Fatal("missing translation")
			}
		}
	}
	for input, want := range map[string]string{"": "en", "en-GB": "en", "FR-ca": "fr", "de_DE": "de", "ja-JP": "ja", "jargon": "en", "-fr": "en"} {
		if got := NormalizeLocale(input); got != want {
			t.Errorf("%q: got %s, want %s", input, got, want)
		}
	}
	copy := Locales()
	copy[0] = "wrong"
	if Locales()[0] != "en" {
		t.Fatal("mutable locale registry")
	}
}

func TestRejectInvalidCatalog(t *testing.T) {
	valid := Entry{Key: "test.message", Text: []string{"Hello {name}", "Salut {name}", "Hallo {name}", "こんにちは{name}"}}
	for _, mutation := range []func([]Entry) []Entry{
		func(e []Entry) []Entry { return append(e, e[0]) },
		func(e []Entry) []Entry { e[0].Key = "Invalid key"; return e },
		func(e []Entry) []Entry { e[0].Text = e[0].Text[:3]; return e },
		func(e []Entry) []Entry { e[0].Text[1] = "Salut {wrong}"; return e },
		func(e []Entry) []Entry { e[0].Text[1] = "Salut {name"; return e },
		func(e []Entry) []Entry { e[0].Text[1] = "Salut {bad-name}"; return e },
		func(e []Entry) []Entry { e[0].Text[1] = "Salut }"; return e },
		func(e []Entry) []Entry { e[0].Text[1] = "\x00"; return e },
		func(e []Entry) []Entry { e[0].Text[1] = ""; return e },
		func(e []Entry) []Entry { e[0].Text[1] = strings.Repeat("a", 4096); return e },
	} {
		copy := valid
		copy.Text = slices.Clone(valid.Text)
		data, err := json.Marshal(mutation([]Entry{copy}))
		if err != nil {
			t.Fatal(err)
		}
		if _, err := Decode(data); err == nil {
			t.Fatalf("accepted invalid catalog %s", data)
		}
	}
	for _, data := range [][]byte{[]byte("[] true"), []byte(`[{"key":"test", "text":[], "extra":true}]`), {0xff}} {
		if _, err := Decode(data); err == nil {
			t.Fatalf("accepted %q", data)
		}
	}
}

func TestGeneratedCAndExplicitOverlayKeys(t *testing.T) {
	root := filepath.Join("..", "..", "..")
	if _, err := os.Stat(filepath.Join(root, "src", "settings_overlay.c")); os.IsNotExist(err) {
		t.Skip("standalone Go source tree")
	}
	cmd := exec.Command("go", "run", "./cmd/generate", "-check", "-output", filepath.Join(root, "src/localization/ui_catalog_data.inc"))
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("generated catalog: %v\n%s", err, out)
	}
	entries, err := Entries()
	if err != nil {
		t.Fatal(err)
	}
	keys := make(map[string]bool)
	for _, entry := range entries {
		keys[entry.Key] = true
	}
	for _, file := range []string{"settings_overlay.c", "settings_overlay_localization.c", "settings_overlay_regions.c", "settings_overlay_layers_localization.c", "manual/manual_caption.c"} {
		data, err := os.ReadFile(filepath.Join(root, "src", file))
		if err != nil {
			t.Fatal(err)
		}
		for _, match := range regexp.MustCompile(`"((?:overlay|common|setting)\.[a-z0-9_.]+)"`).FindAllSubmatch(data, -1) {
			if !keys[string(match[1])] {
				t.Errorf("%s references missing catalog key %s", file, match[1])
			}
		}
	}
}

// Optional release/backend gate: the built game's real SDL3_ttf stack must
// cover all built-in interface text, not just a hand-picked Japanese example.
func TestShippedInterfaceFontCoverage(t *testing.T) {
	probe := os.Getenv("AR_AUTHOR_FONT_PROBE")
	if probe == "" {
		t.Skip("set AR_AUTHOR_FONT_PROBE to the built game")
	}
	entries, err := Entries()
	if err != nil {
		t.Fatal(err)
	}
	seen := make(map[rune]bool)
	for _, entry := range entries {
		for _, text := range entry.Text {
			for _, scalar := range text {
				if scalar != '\n' {
					seen[scalar] = true
				}
			}
		}
	}
	scalars := make([]rune, 0, len(seen))
	for scalar := range seen {
		scalars = append(scalars, scalar)
	}
	slices.Sort(scalars)
	var input bytes.Buffer
	for _, scalar := range scalars {
		fmt.Fprintf(&input, "%X\n", scalar)
	}
	root, err := filepath.Abs(filepath.Join("..", "..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(probe, "--font-coverage-v1", filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf"), filepath.Join(root, "game-assets/fonts/noto/NotoSansJP-Bold.otf"))
	cmd.Stdin = &input
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	output, err := cmd.Output()
	if err != nil {
		t.Fatalf("font probe: %v\n%s", err, stderr.String())
	}
	lines := strings.Split(strings.TrimSpace(string(output)), "\n")
	if len(lines) != len(scalars) {
		t.Fatalf("probe returned %d scalars; expected %d", len(lines), len(scalars))
	}
	for i, line := range lines {
		if line != fmt.Sprintf("%04X\t1", scalars[i]) {
			t.Errorf("interface glyph U+%04X: %s", scalars[i], line)
		}
	}
}
