package localization

import (
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

// Local release gate. Never embeds retail content in the test or its output.
// Each source pack must agree with the game's production parser/validator,
// independently of the builder, and every release must pass the full census.
func TestNativeROMCoverageAndAuthorRuntime(t *testing.T) {
	root := os.Getenv("AR_LOCALIZATION_GUI_ROM_ROOT")
	if root == "" {
		t.Skip("regional ROM fixtures not configured")
	}
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	var catalogs []*NativeCatalog
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
			catalog, err := d.BuildNativeCatalog()
			if err != nil {
				t.Fatal(err)
			}
			catalogs = append(catalogs, catalog)
			pack, err := d.BuildNativeAuthorPack(d.NativeSourceMetadata())
			if err != nil {
				t.Fatal(err)
			}
			dir := t.TempDir()
			writeAuthorTestFiles(t, dir, pack.Files())
			reopened, err := OpenAuthorPack(dir)
			if err != nil || !reflect.DeepEqual(pack.Files(), reopened.Files()) {
				t.Fatal("source pack round trip changed", err)
			}
			if probe != "" {
				assertAuthorPackRuntime(t, probe, dir, pack)
			}
			checkNativeROMMutations(t, d)
			rom[0] ^= 1
			if _, err := NewDecoder(rom); err == nil {
				t.Fatal("modified ROM accepted")
			}
			t.Logf("%s: %d structured records, %d composer records, %d semantic routes", d.ReleaseID(), len(catalog.Messages), len(catalog.Menu.Segments), catalog.SemanticRoutes.RouteCount)
		})
	}
	if t.Failed() {
		return
	}
	reports, err := CheckNativeCoverage(catalogs...)
	if err != nil {
		t.Fatal(err)
	}
	if len(reports) != 5 {
		t.Fatal("missing release coverage")
	}
	for _, report := range reports {
		if report["complete"] != true {
			t.Fatal("incomplete five-release source census", report["blockers"])
		}
	}
	checkCoverageBlockers(t, catalogs)
	for _, catalog := range catalogs {
		report, err := CheckNativeCoverage(catalog)
		if err != nil {
			t.Fatal(err)
		}
		blockers := irRows(report[0], "blockers")
		if report[0]["complete"] != false || len(blockers) != 1 || blockers[0]["id"] != "cross_release_semantic_alignment" {
			t.Fatal("single release hid missing alignment")
		}
	}
}

func checkNativeROMMutations(t *testing.T, d *Decoder) {
	t.Helper()
	sp, dp := sourceProfiles[d.ReleaseID()], destinationFacts.Profiles[d.ReleaseID()]
	mutations := []referenceMutation{}
	brokenOpcode := func(id string, pc int) {
		offset, err := pc24Offset(pc)
		if err != nil {
			t.Fatal(err)
		}
		if d.rom[offset] == 0xea {
			t.Fatal("ineffective opcode mutation", id)
		}
		mutations = append(mutations, referenceMutation{ID: id, Offset: offset, Data: []int{0xea}})
	}
	brokenOpcode("interactive-entry", sp.InteractiveEntry)
	brokenOpcode("composer-entry", sp.ComposerEntry)
	census, err := d.DiscoverNativeSources()
	if err != nil {
		t.Fatal(err)
	}
	for _, wrapper := range irRows(census.DialogueForwarding, "wrappers") {
		for _, call := range irRows(wrapper, "call_sites") {
			pc, err := parsePC(irString(call, "call_site"))
			if err != nil {
				t.Fatal(err)
			}
			brokenOpcode("dialogue-wrapper-call", pc)
			break
		}
	}
	for _, site := range dp.DMA {
		brokenOpcode("dma-"+site.ID, site.Address)
	}
	for _, row := range dp.VRAMPaths {
		for _, site := range row.Sites {
			brokenOpcode("vram-"+row.ID, site)
		}
	}
	for _, row := range dp.Indirect {
		brokenOpcode("indirect-"+row.ID, row.Sites[0])
	}
	for _, row := range dp.RangeRejected {
		brokenOpcode("range-rejected-"+row.ID, row.Address)
	}
	for _, row := range dp.IndirectRejected {
		brokenOpcode("indirect-rejected-"+row.ID, row.Address)
	}
	entries, err := d.assetScript()
	if err != nil {
		t.Fatal(err)
	}
	ending, err := d.endingAssets(entries)
	if err != nil {
		t.Fatal(err)
	}
	for _, offset := range []int{ending.pageSource, ending.fontSource} {
		mutations = append(mutations, referenceMutation{ID: "credits-decoded-extent", Offset: offset, Data: []int{int(d.rom[offset] ^ 1)}})
	}
	for _, mutation := range mutations {
		changed, err := mutatedDecoder(t, d, mutation)
		if err != nil {
			t.Fatal(err)
		}
		if catalog, err := changed.BuildNativeCatalog(); err == nil || catalog != nil {
			t.Fatal("failed proof published a catalog", mutation.ID)
		}
	}
	t.Logf("%s: %d native proof mutations rejected", d.ReleaseID(), len(mutations))
}
