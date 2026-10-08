package localization

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
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
			ops, err := pack.MessageOperations("dialogue.event.wrapper_05.call_00.source_00")
			if err != nil {
				t.Fatal(err)
			}
			buttons := map[string]int{}
			for _, op := range ops {
				if op.Op == "placeholder" {
					buttons[op.Name]++
				}
			}
			for _, name := range []string{"icon.button.b", "icon.button.y", "icon.button.start"} {
				if buttons[name] != 1 {
					t.Fatalf("%s: native button reference missing or duplicated: %s (%d)", d.ReleaseID(), name, buttons[name])
				}
			}
			if view, found := pack.workspace.Message("title.start_prompt"); found && view.Present {
				ops, err := pack.MessageOperations("title.start_prompt")
				if err != nil {
					t.Fatal(err)
				}
				count := 0
				for _, op := range ops {
					if op.Op == "placeholder" && op.Name == "icon.button.start" {
						count++
					}
				}
				if count != 1 {
					t.Fatal("title Start reference missing", d.ReleaseID(), count)
				}
			}
			if d.ReleaseID() == "us" {
				for _, name := range abilityNames {
					id := "sky.magic.selected." + strings.Split(name.Placeholder, ".")[1]
					if strings.HasPrefix(name.Placeholder, "miracle.") {
						id = "sim.miracle." + strings.Split(name.Placeholder, ".")[1] + ".confirm"
					}
					ops, err := pack.MessageOperations(id)
					if err != nil {
						t.Fatal(err)
					}
					found := false
					for _, op := range ops {
						if op.Op == "placeholder" && op.Name == name.Placeholder {
							found = true
						}
					}
					if !found {
						t.Fatal("US ability name remains literal", id, name.Placeholder)
					}
				}
				for _, help := range nativeHelpMessages() {
					if view, found := pack.workspace.Message(help.ID); !found || !view.Present {
						t.Fatal("missing Help reference", help.ID)
					}
				}
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
			for _, id := range []string{"sim.town_status.construction", "sim.town_status.sealing_lair"} {
				ops, err := pack.MessageOperations(id)
				if err != nil || len(ops) != 2 || ops[0].Op != "text" || ops[0].Value == "" || ops[1].Op != "end" {
					t.Fatal("town notice is not a complete localized status label", id, err)
				}
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
	brokenOpcode("town-status-selector", sp.Flow.TownStatus)
	brokenOpcode("town-status-blank-branch", sp.Flow.TownStatus+15)
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
