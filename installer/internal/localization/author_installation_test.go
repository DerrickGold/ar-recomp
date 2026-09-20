package localization

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestLocalInstallationPreservesEveryStatusWithoutPublicationConsent(t *testing.T) {
	p := authorAdventureProject(t)
	p, err := p.EditMessage("action.hud.act_1", "Still unreviewed.\n@end\n", TranslationNotStarted)
	if err != nil {
		t.Fatal(err)
	}
	before := p.ProjectRevision()
	files := p.pack.Files()
	prepared, report, err := p.Installation()
	if err != nil {
		t.Fatal(err)
	}
	if report.Upgrade == nil || report.Upgrade.FromVersion != 1 || report.Upgrade.ToVersion != 2 ||
		report.Messages != p.Pack().Workspace().Stats().MessageCount+6 || prepared.pack.manifest.Version() != 2 ||
		prepared.pack.manifest.Metadata() != p.pack.manifest.Metadata() {
		t.Fatal("installation did not prepare v2 with the same identity", report)
	}
	for id := range p.pack.workspace.messageScript {
		old, _ := p.pack.workspace.Message(id)
		got, _ := prepared.pack.workspace.Message(id)
		if !got.Present || got.Status != old.Status {
			t.Fatal("preparation lost a supplied message or progress", id)
		}
	}
	again, repeated, err := prepared.Installation()
	if err != nil || repeated.Upgrade != nil || again.pack != prepared.pack || repeated.Messages != report.Messages {
		t.Fatal("v2 installation converted again", repeated, err)
	}
	if err := prepared.WriteArchive(io.Discard, "publication"); err == nil {
		t.Fatal("installation authorized publication")
	}
	if _, _, err := prepared.Publication(PublicationOptions{}); err == nil {
		t.Fatal("export bypassed rights gate")
	}
	root := t.TempDir()
	if _, err := InstallAuthorProject(root, prepared, false); err != nil {
		t.Fatal(err)
	}
	installed, err := OpenAuthorPack(root)
	if err != nil {
		t.Fatal(err)
	}
	for id := range p.pack.workspace.messageScript {
		old, _ := resolvedAuthorOperations(p.pack.workspace, id)
		got, err := resolvedAuthorOperations(installed.workspace, id)
		if err != nil || presentationDigest(old) != presentationDigest(got) {
			t.Fatal("installed wording or controls changed", id, err)
		}
	}
	for _, path := range []string{"author-project.json", "translation-progress.tsv"} {
		if _, err := os.Stat(filepath.Join(root, path)); !os.IsNotExist(err) {
			t.Fatal("installed private editor metadata", path)
		}
	}
	if p.ProjectRevision() != before || p.pack.manifest.Version() != 1 || !reflect.DeepEqual(files, p.pack.Files()) {
		t.Fatal("installation changed author project")
	}
	pub, reportPub, err := p.Publication(PublicationOptions{ConfirmRights: true})
	if err != nil || reportPub.WIP != 0 || reportPub.Included >= report.Messages {
		t.Fatal("publisher filtering no longer works", reportPub, err)
	}
	var data bytes.Buffer
	if err := pub.WriteArchive(&data, "publication"); err != nil {
		t.Fatal(err)
	}
	imported, err := ReadAuthorArchive(bytes.NewReader(data.Bytes()), int64(data.Len()))
	if err != nil {
		t.Fatal(err)
	}
	if _, r, err := imported.Installation(); err != nil || r.Upgrade == nil || r.Messages != reportPub.Included+6 {
		t.Fatal("shared archive was re-filtered", r, err)
	}
}

func TestLocalInstallationRejectsReferencePacksAndInvalidatesAfterEditing(t *testing.T) {
	p := authorAdventureProject(t)
	native, err := NewSourceProject(p.Pack())
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := native.Installation(); err == nil {
		t.Fatal("native reference installed as a translation")
	}
	m := p.Pack().Manifest().Metadata()
	m.Target = "reference-only"
	regional, err := p.WithMetadata(m)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := regional.Installation(); err == nil {
		t.Fatal("reference-only pack installed")
	}
	// Editing a prepared snapshot drops its installation authorization.
	ready, _, _ := p.Installation()
	edited, err := ready.WithNotes("New private note")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := InstallAuthorProject(t.TempDir(), edited, false); err == nil {
		t.Fatal("mutated snapshot retained installation authorization")
	}
}
