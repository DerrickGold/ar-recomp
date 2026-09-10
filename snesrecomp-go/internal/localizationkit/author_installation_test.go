package localizationkit

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
	"testing"
)

func TestLocalInstallationPreservesEveryStatusWithoutPublicationConsent(t *testing.T) {
	p := authorAdventureProject(t)
	p, err := p.EditMessage("action.hud.act_1", "Still unreviewed.\n@end\n", TranslationNotStarted)
	if err != nil {
		t.Fatal(err)
	}
	before := p.ProjectRevision()
	prepared, report, err := p.Installation()
	if err != nil {
		t.Fatal(err)
	}
	if report.Messages != p.Pack().Workspace().Stats().MessageCount || prepared.pack != p.pack {
		t.Fatal("local installation filtered supplied messages", report)
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
		old, _ := p.pack.workspace.Message(id)
		got, _ := installed.workspace.Message(id)
		if !got.Present || old.Body != got.Body {
			t.Fatal("installed content changed", id, old.Status)
		}
	}
	for _, path := range []string{"author-project.json", "translation-progress.tsv"} {
		if _, err := os.Stat(filepath.Join(root, path)); !os.IsNotExist(err) {
			t.Fatal("installed private editor metadata", path)
		}
	}
	if p.ProjectRevision() != before {
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
	if _, r, err := imported.Installation(); err != nil || r.Messages != reportPub.Included {
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
