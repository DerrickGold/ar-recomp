package localization

import (
	"os"
	"path/filepath"
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
		ops = append(ops, AuthorOperation{Op: "end"})
		messages = append(messages, AuthorMessage{ID: route.ID, Operations: ops})
	}
	messages = append(messages, AuthorMessage{ID: "action.hud.act_label",
		Operations: []AuthorOperation{{Op: "text", Value: "Chapter"}, {Op: "end"}}})
	script, err := EmitAuthorScript(messages, installedSource)
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	writeAuthorTestFiles(t, root, map[string][]byte{"pack.ini": []byte(manifest.Text()), installedSource: []byte(script.Text()),
		"translation-progress.tsv": []byte("action.hud.act_1\twip\n")})
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
	expected := old.workspace.Stats().MessageCount + len(nativeHUDLabels("us")) + len(nativeHUDValues())
	if pack.workspace.Stats().MessageCount != expected {
		t.Fatal("optional labels missing")
	}
	for _, message := range messages {
		before, _ := old.MessageOperations(message.ID)
		after, _ := pack.MessageOperations(message.ID)
		if presentationDigest(before) != presentationDigest(after) {
			t.Fatal("upgrade changed", message.ID)
		}
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
}
