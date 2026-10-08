package localization

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

func TestNativeSourceOptionalHudUpgrade(t *testing.T) {
	metadata := PackMetadata{ID: "native-us", Locale: "en-US", Name: "Native US", Autonym: "English", Author: "Local ROM extraction",
		License: "Local test only", Direction: "auto", Target: "us-runtime", SourceProfile: "us", Coverage: "complete", Fallback: "native-us"}
	// An already-installed pack, which is the shape a supplement really meets:
	// its manifest names the source inside the version directory a previous
	// install created.
	const installedSource = "v-0000000000/text/source.artext"
	manifest, err := NewPackManifestVersion(metadata, PackFonts{Primary: "builtin:actraiser-sans"}, []string{installedSource}, 1)
	if err != nil {
		t.Fatal(err)
	}
	var messages []AuthorMessage
	for _, route := range authorContracts.ordered {
		anchors, native := route.Anchors["us"]
		if !native || route.Optional {
			continue
		}
		ops := []AuthorOperation{}
		for _, anchor := range anchors {
			ops = append(ops, AuthorOperation{Op: "anchor", ID: anchor})
		}
		if route.ID == "dialogue.event.relay.fillmore" {
			ops = append(ops, AuthorOperation{Op: "text", Value: "First fragmentSecond fragmentClosing."})
		}
		if route.ID == "sim.miracle.lightning.description" || route.ID == "simulation.event.aitos.slot_13" {
			ops = append(ops, AuthorOperation{Op: "text", Value: "Your lightning works."})
		}
		ops = append(ops, AuthorOperation{Op: "end"})
		message := AuthorMessage{ID: route.ID, Operations: ops}
		if route.ID == "simulation.event.aitos.slot_14" {
			message = AuthorMessage{ID: route.ID, Alias: "simulation.event.aitos.slot_13"}
		}
		messages = append(messages, message)
	}
	messages = append(messages, AuthorMessage{ID: "action.hud.act_label",
		Operations: []AuthorOperation{{Op: "text", Value: "Chapter"}, {Op: "end"}}})
	script, err := EmitAuthorScript(messages, installedSource)
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	writeAuthorTestFiles(t, root, map[string][]byte{"pack.ini": []byte(manifest.Text()), installedSource: []byte(script.Text()),
		"translation-progress.tsv": []byte("action.hud.act_1\twip\nsimulation.event.aitos.slot_13\twip\ndialogue.event.relay.fillmore\twip\n")})
	old, err := OpenNativeUSSource(root)
	if err != nil {
		t.Fatal(err)
	}
	pack, err := EnsureNativeUSSource(root, nil)
	if err != nil {
		t.Fatal(err)
	}
	// Every transcribed label for this release except the one the pack already
	// carries. Derived, so transcribing another label does not fail this.
	expected := old.workspace.Stats().MessageCount + len(nativeHUDLabels("us")) + len(nativeHUDValues()) + len(nativeHelpMessages())
	if pack.workspace.Stats().MessageCount != expected {
		t.Fatal("optional labels missing")
	}
	for _, message := range messages {
		before, _ := old.MessageOperations(message.ID)
		after, _ := pack.MessageOperations(message.ID)
		if message.ID == "sim.miracle.lightning.description" || message.ID == "simulation.event.aitos.slot_13" || message.ID == "simulation.event.aitos.slot_14" {
			found := false
			for _, op := range after {
				if op.Name == "miracle.lightning.name" {
					found = true
				}
			}
			if !found {
				t.Fatal("existing baseline name was not upgraded")
			}
			continue
		}
		if presentationDigest(before) != presentationDigest(after) {
			t.Fatal("upgrade changed", message.ID)
		}
	}
	// Reused native prose keeps its alias and accepts the new name value on
	// every consuming route; upgrading its target must not invalidate the pack.
	aliasView, _ := pack.workspace.Message("simulation.event.aitos.slot_14")
	if !strings.Contains(aliasView.Body, "@alias simulation.event.aitos.slot_13") {
		t.Fatal("native alias was expanded unnecessarily")
	}
	aliased, err := pack.ResolvedMessage("simulation.event.aitos.slot_14")
	if err != nil {
		t.Fatal(err)
	}
	foundName := false
	for _, op := range aliased.Operations {
		foundName = foundName || op.Name == "miracle.lightning.name"
	}
	if !foundName {
		t.Fatal("aliased native name was not upgraded")
	}
	nameView, _ := pack.workspace.Message("simulation.event.aitos.slot_13")
	if nameView.Status != TranslationWIP {
		t.Fatal("name migration lost progress")
	}
	// The installer owns the version directory. Supplementing an installed pack
	// must not stack another one onto the paths it already carries, or every
	// upgrade buries the source one level deeper.
	for _, source := range pack.Manifest().Sources() {
		if versions := strings.Count(source, "v-"); versions != 1 {
			t.Fatal("source path nests version directories:", source)
		}
	}
	view, _ := pack.workspace.Message("action.hud.act_1")
	if view.Status != TranslationWIP {
		t.Fatal("progress lost")
	}
	if data, err := os.ReadFile(filepath.Join(root, installedSource)); err != nil || string(data) != script.Text() {
		t.Fatal("old source was overwritten", err)
	}
	again, err := EnsureNativeUSSource(root, nil)
	if err != nil || again.RuntimeRevision() != pack.RuntimeRevision() {
		t.Fatal("non-idempotent upgrade", err)
	}
	// A newer extraction may discover prose, not just transcribed HUD art.
	// Supplement it through the same installer while preserving existing edits.
	source, err := pack.EditMessage("dialogue.event.relay.fillmore", "First fragment\n@preferred-line\nSecond fragment Closing.\n@end\n", TranslationNotStarted)
	if err != nil {
		t.Fatal(err)
	}
	for _, id := range []string{"sim.town_status.construction", "sim.town_status.sealing_lair"} {
		source, err = source.AddMessage(id, source.Manifest().Sources()[0], "New status words", TranslationNotStarted)
		if err != nil {
			t.Fatal(err)
		}
	}
	upgraded, err := InstallNativeUSSource(root, source)
	if err != nil {
		t.Fatal(err)
	}
	if upgraded.Workspace().Stats().MessageCount != pack.Workspace().Stats().MessageCount+2 {
		t.Fatal("newly discovered native messages were not installed")
	}
	relayView, _ := upgraded.workspace.Message("dialogue.event.relay.fillmore")
	if relayView.Status != TranslationWIP {
		t.Fatal("spacing repair lost progress")
	}
	for _, message := range messages {
		before, _ := pack.MessageOperations(message.ID)
		after, _ := upgraded.MessageOperations(message.ID)
		if message.ID == "dialogue.event.relay.fillmore" {
			if !slices.ContainsFunc(after, func(op AuthorOperation) bool { return op.Op == "preferred_line" }) {
				t.Fatal("old relay spacing was not upgraded")
			}
			continue
		}
		if presentationDigest(before) != presentationDigest(after) {
			t.Fatal("new source replaced existing wording", message.ID)
		}
	}
	again, err = InstallNativeUSSource(root, source)
	if err != nil || again.RuntimeRevision() != upgraded.RuntimeRevision() {
		t.Fatal("new native route supplement is not idempotent", err)
	}
}
