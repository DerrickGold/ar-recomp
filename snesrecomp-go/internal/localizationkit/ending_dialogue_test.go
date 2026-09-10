package localizationkit

import (
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// Ending dialogue is distinct from the twenty graphical credit pages. It
// already uses the ordinary interpreter, through the same wrapper identities
// as simulation dialogue. Prove the author-visible source aliases reach those
// consumers rather than introducing a competing ending renderer/ID map.
func TestEndingDialogueAuthorAliasesROM(t *testing.T) {
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
			catalog, err := d.BuildNativeCatalog()
			if err != nil {
				t.Fatal(err)
			}
			pack, err := d.BuildNativeAuthorPack(d.NativeSourceMetadata())
			if err != nil {
				t.Fatal(err)
			}
			routes := map[string]*NativeSemanticRoute{}
			for _, route := range catalog.SemanticRoutes.Routes {
				routes[route.ID] = route
			}
			wrappers := irRows(catalog.source.DialogueForwarding, "wrappers")
			calls := irRows(wrappers[0], "call_sites")
			if irString(calls[0], "source_origin") != "indexed_long_pointer_table" || calls[0]["pointer_count"] != 7 {
				t.Fatal("ending montage consumer changed")
			}
			for slot := 0; slot < 8; slot++ {
				consumer := fmt.Sprintf("dialogue.event.wrapper_00.call_00.source_%02d", slot)
				if slot == 7 {
					consumer = "dialogue.event.wrapper_05.call_06.source_00"
				}
				route := routes[consumer]
				if route == nil || !slices.ContainsFunc(route.Provenance, func(p string) bool { return strings.HasPrefix(p, "dialogue_wrapper_") }) {
					t.Fatal("ending has no verified interpreter consumer", consumer)
				}
				// Western catalogues also expose the physical ending table.
				// JP exposes these through its equivalent consumer identities.
				id := consumer
				if d.ReleaseID() != "jp" {
					id = fmt.Sprintf("dialogue.ending.slot_%02d", slot)
					source := routes[id]
					if source == nil || source.RecordID != route.RecordID || source.SourceOffset != route.SourceOffset {
						t.Fatal("ending source/consumer identity drift", id)
					}
				}
				ops, err := pack.MessageOperations(id)
				if err != nil {
					t.Fatal(err)
				}
				marker := fmt.Sprintf("Ending-check-%02d", slot)
				index := slices.IndexFunc(ops, func(op AuthorOperation) bool { return op.Op == "text" && op.Value != "" })
				if index < 0 {
					t.Fatal("ending contains no editable text", id)
				}
				ops[index].Value = marker
				script, err := EmitAuthorScript([]AuthorMessage{{ID: id, Operations: ops}}, "test.artext")
				if err != nil {
					t.Fatal(err)
				}
				pack, err = pack.EditMessage(id, script.text[script.spans[0].bodyStart:script.spans[0].end], TranslationWIP)
				if err != nil {
					t.Fatal(err)
				}
				actual, err := pack.MessageOperations(consumer)
				if err != nil || !slices.ContainsFunc(actual, func(op AuthorOperation) bool { return op.Op == "text" && strings.Contains(op.Value, marker) }) {
					t.Fatal("author edit did not reach the live ending route", id, consumer, err)
				}
				if slot == 0 && d.ReleaseID() != "jp" {
					actual, err = pack.MessageOperations("dialogue.event.wrapper_05.call_03.source_00")
					if err != nil || !slices.ContainsFunc(actual, func(op AuthorOperation) bool { return strings.Contains(op.Value, marker) }) {
						t.Fatal("Sky Palace ending introduction lost its shared source", err)
					}
				}
			}
			if probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE"); probe != "" {
				dir := t.TempDir()
				writeAuthorTestFiles(t, dir, pack.Files())
				assertAuthorPackRuntime(t, probe, dir, pack)
			}
		})
	}
}
