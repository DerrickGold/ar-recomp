package localization

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestCreditsPagesROM(t *testing.T) {
	root := os.Getenv("AR_LOCALIZATION_GUI_ROM_ROOT")
	if root == "" {
		t.Skip("regional ROM fixtures not configured")
	}
	for _, name := range []string{"ar.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc", "ar-jp.sfc"} {
		t.Run(name, func(t *testing.T) {
			rom, err := os.ReadFile(filepath.Join(root, name))
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
			pages, err := decodeCreditPages(d.ReleaseID(), assets.pages)
			if err != nil {
				t.Fatal(err)
			}
			if len(pages) != 20 || !pages[15].ArtworkOnly || pages[19].ID != "credits.game_over" {
				t.Fatal("incomplete page classification")
			}
			if d.ReleaseID() == "fr" && (len(pages[9].Lines) != 5 || !strings.Contains(strings.Join(pages[1].Lines, " "), "Scénario")) {
				t.Fatal("French wrapping/accent lost")
			}
			if d.ReleaseID() == "jp" && (len(pages[2].Lines) != 3 || pages[16].ID != "credits.the_end" || pages[17].ID != "credits.special_mode") {
				t.Fatal("JP contributor/terminal identity lost")
			}
			if !strings.Contains(strings.Join(pages[4].Lines, " "), "Yūzō") {
				t.Fatal("macrons inverted")
			}
			for _, page := range pages {
				if page.ArtworkOnly {
					continue
				}
				changed := bytes.Clone(assets.pages)
				// Both upper and lower halves are part of each verified composition.
				for _, row := range []int{page.Rows[0], page.Rows[0] + 1} {
					changed[page.Page*endingPageBytes+row*64] = 0xff
					if _, err := decodeCreditPages(d.ReleaseID(), changed); err == nil {
						t.Fatal("unknown credits composition accepted")
					}
				}
			}
			pack, err := d.BuildNativeAuthorPack(d.NativeSourceMetadata())
			if err != nil {
				t.Fatal(err)
			}
			pack, err = pack.EditMessage("credits.page_01", "- Équipe -\n@line\n別の名前\n@end\n", TranslationDone)
			if err != nil {
				t.Fatal(err)
			}
			for _, bad := range []string{"one\n@page\ntwo\n@end\n", strings.Repeat("line\n@line\n", 6) + "last\n@end\n"} {
				if _, err := pack.EditMessage("credits.page_01", bad, TranslationWIP); err == nil {
					t.Fatal("unpresentable credits accepted")
				}
			}
			if probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE"); probe != "" {
				dir := t.TempDir()
				writeAuthorTestFiles(t, dir, pack.Files())
				assertAuthorPackRuntime(t, probe, dir, pack)
			}
			if d.ReleaseID() == "us" {
				catalog, err := d.BuildNativeCatalog()
				if err != nil {
					t.Fatal(err)
				}
				old, err := nativeAuthorPack(pack.manifest, catalog.SemanticRoutes.Routes, catalog.source.NativeDialogueLayout, nativeHUDLabels("us")...)
				if err != nil {
					t.Fatal(err)
				}
				dir := t.TempDir()
				writeAuthorTestFiles(t, dir, old.Files())
				upgraded, err := EnsureNativeUSSource(dir, rom)
				if err != nil {
					t.Fatal(err)
				}
				if _, err = upgraded.MessageOperations("credits.the_end"); err != nil {
					t.Fatal("old baseline missed credits", err)
				}
				again, err := EnsureNativeUSSource(dir, rom)
				if err != nil || upgraded.RuntimeRevision() != again.RuntimeRevision() {
					t.Fatal("baseline supplement is not idempotent", err)
				}
				if probe := os.Getenv("AR_CREDITS_RUNTIME_PROBE"); probe != "" {
					path := filepath.Join(t.TempDir(), "pages.bin")
					if err := os.WriteFile(path, assets.pages, 0600); err != nil {
						t.Fatal(err)
					}
					if out, err := exec.Command(probe, "--check-pages", path).CombinedOutput(); err != nil {
						t.Fatalf("runtime credits: %v\n%s", err, out)
					}
				}
			}
		})
	}
}

func TestCreditsDecoderSynthetic(t *testing.T) {
	if _, err := decodeCreditPages("us", nil); err == nil {
		t.Fatal("extent accepted")
	}
	pages := make([]byte, 20*endingPageBytes)
	for i := 0; i < len(pages); i += 2 {
		pages[i] = 0x10
	}
	for page := 0; page < 20; page++ {
		for x, v := range []byte{0x20, 0x21} {
			pages[page*endingPageBytes+13*64+x*2] = v
			pages[page*endingPageBytes+14*64+x*2] = v + 0x10
		}
	}
	decoded, err := decodeCreditPages("us", pages)
	if err != nil {
		t.Fatal(err)
	}
	if decoded[0].Lines[0] != "A" || decoded[0].Rows[0] != 13 {
		t.Fatal("synthetic alphabet geometry")
	}
	for _, off := range []int{13 * 64, 14 * 64, 13*64 + 1, 14*64 + 1} {
		bad := bytes.Clone(pages)
		bad[off] = 0xff
		if _, err := decodeCreditPages("us", bad); err == nil {
			t.Fatal("bad tile accepted")
		}
	}
}
