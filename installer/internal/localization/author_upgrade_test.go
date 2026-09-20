package localization

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"testing/fstest"
)

func TestUpgradeV2PreservesTextAndProject(t *testing.T) {
	const source = `# translator's file note
:: city.fillmore.name
; keep this spelling
A <B> \\ C
@end
:: city.bloodpool.name
@alias city.fillmore.name
:: dialogue.event.relay.aitos
@alias city.fillmore.name
:: status.report.master_report
{master_name}
@line
@line
@line
LV | {master_level:02} | HP | {master_hp:02}
@end
:: action.hud.act_label
ACT
@end
:: credits.page_01
- Équipe -
@end
`
	files := fstest.MapFS{"pack.ini": {Data: []byte(authorPackManifest)}, "text/sky.artext": {Data: []byte(source)}}
	pack, err := LoadAuthorPack(files)
	if err != nil {
		t.Fatal(err)
	}
	p := &AuthorProject{pack: pack, info: projectInfo{Version: 1, Origin: "translation", Notes: "Private notes", Baseline: map[string]string{"city.fillmore.name": "baseline"}}, notices: map[string][]byte{"notices/license.txt": []byte("Credit")}}
	before := p.pack.Files()
	next, report, err := p.UpgradeV2("example.excellent-v2")
	if err != nil {
		t.Fatal(err)
	}
	if next.Pack().Manifest().Version() != 2 || report.Messages != 12 || report.AliasesPreserved != 1 || report.AliasesMaterialized != 1 {
		t.Fatal(report)
	}
	if !reflect.DeepEqual(before, p.pack.Files()) || next.Notes() != p.Notes() || !reflect.DeepEqual(p.Notices(), next.Notices()) || !reflect.DeepEqual(p.info.Baseline, next.info.Baseline) {
		t.Fatal("upgrade mutated source or lost private metadata")
	}
	for id := range pack.workspace.messageScript {
		oldOps, _ := resolvedAuthorOperations(pack.workspace, id)
		newOps, err := resolvedAuthorOperations(next.pack.workspace, id)
		if err != nil || presentationDigest(oldOps) != presentationDigest(newOps) {
			t.Fatalf("wording/control changed: %s: %v", id, err)
		}
	}
	text := next.pack.workspace.scripts[0].Text()
	for _, want := range []string{"translator's file note", "keep this spelling", `A \<B> \\\\ C`, "@layout master_status", "@style location", "@style hud-frame", "@font hud", ":: world_map.location_label", "@style world", "@numerals upright", "{location_name}", "<i>{master_hp:02}</i>", `<span style="credits-accent">- É</span>`} {
		if !strings.Contains(text, want) {
			t.Errorf("missing %q in:\n%s", want, text)
		}
	}
	if _, _, err := p.UpgradeV2(pack.manifest.metadata.ID); err == nil {
		t.Fatal("same-ID upgrade accepted")
	}
	if _, _, err := next.UpgradeV2("another.v2"); err == nil {
		t.Fatal("v2 converted twice")
	}
	if p.pack.manifest.Version() != 1 {
		t.Fatal("old format changed")
	}
}

func TestUpgradeNativeV1ROM(t *testing.T) {
	root := os.Getenv("AR_LOCALIZATION_GUI_ROM_ROOT")
	if root == "" {
		t.Skip("regional ROM fixtures not configured")
	}
	for _, file := range []string{"ar.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc", "ar-jp.sfc"} {
		t.Run(file, func(t *testing.T) {
			rom, err := os.ReadFile(filepath.Join(root, file))
			if err != nil {
				t.Fatal(err)
			}
			d, err := NewDecoder(rom)
			if err != nil {
				t.Fatal(err)
			}
			pack, err := d.buildNativeAuthorPackV1(d.NativeSourceMetadata())
			if err != nil {
				t.Fatal(err)
			}
			upgraded, _, err := upgradeAuthorPackV2(pack)
			if err != nil {
				t.Fatal(err)
			}
			for id := range pack.workspace.messageScript {
				before, _ := resolvedAuthorOperations(pack.workspace, id)
				after, err := resolvedAuthorOperations(upgraded.workspace, id)
				if err != nil || presentationDigest(before) != presentationDigest(after) {
					t.Fatalf("wording/control changed for %s: %v", id, err)
				}
			}
			entries, err := d.assetScript()
			if err != nil {
				t.Fatal(err)
			}
			pages, err := d.nativeCredits(entries)
			if err != nil {
				t.Fatal(err)
			}
			for _, page := range pages {
				if !page.ArtworkOnly && page.Accent != v1CreditsInitialAccent(d.ReleaseID(), page.ID) {
					t.Fatalf("wrong historical accent: %s", page.ID)
				}
			}
		})
	}
}
