package localizationkit

import (
	"archive/zip"
	"bytes"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func authorAdventureProject(t *testing.T) *AuthorProject {
	t.Helper()
	source, err := LoadAuthorPack(projectMapFS(map[string][]byte{"pack.ini": []byte(authorPackManifest), "text/sky.artext": []byte(authorConfirm + "\n:: action.hud.act_1\nInvented source label.\n@end\n")}))
	if err != nil {
		t.Fatal(err)
	}
	m := source.Manifest().Metadata()
	m.ID = "community.excellent-adventure"
	m.Locale = "en-US"
	m.Name = "Excellent Adventure English"
	p, err := NewTranslationProject(source, m)
	if err != nil {
		t.Fatal(err)
	}
	p, err = p.EditMessage("sky.action_mode.confirm", "# private translator note\n@anchor reset_text_cursor.00\nMost excellent, {master_name}!\n@page\nThis town is ready for an epic adventure.\n@anchor yield.01\n@end\n", TranslationDone)
	if err != nil {
		t.Fatal(err)
	}
	p, err = p.EditMessage("action.hud.act_2", "@empty\n@end\n", TranslationWIP)
	if err != nil {
		t.Fatal(err)
	}
	p, err = p.WithNotes("Private review checklist")
	if err != nil {
		t.Fatal(err)
	}
	return p
}

func TestAuthorProjectPublicationAndBackup(t *testing.T) {
	p := authorAdventureProject(t)
	var backup bytes.Buffer
	if err := p.WriteArchive(&backup, "backup"); err != nil {
		t.Fatal(err)
	}
	reopened, err := ReadAuthorArchive(bytes.NewReader(backup.Bytes()), int64(backup.Len()))
	if err != nil {
		t.Fatal(err)
	}
	if reopened.ProjectRevision() != p.ProjectRevision() || reopened.Notes() != p.Notes() {
		t.Fatal("backup lost private state")
	}
	if _, _, err := p.Publication(PublicationOptions{}); err == nil {
		t.Fatal("rights confirmation bypassed")
	}
	pub, report, err := p.Publication(PublicationOptions{ConfirmRights: true, IncludeWIP: true})
	if err != nil {
		t.Fatal(err)
	}
	if report.Included != 2 || report.WIP != 1 || report.Fallback == 0 {
		t.Fatal("wrong publication coverage", report)
	}
	var shared bytes.Buffer
	if err := pub.WriteArchive(&shared, "publication"); err != nil {
		t.Fatal(err)
	}
	z, err := zip.NewReader(bytes.NewReader(shared.Bytes()), int64(shared.Len()))
	if err != nil {
		t.Fatal(err)
	}
	for _, f := range z.File {
		r, _ := f.Open()
		data, _ := io.ReadAll(r)
		r.Close()
		if strings.Contains(string(data), "private translator") || strings.Contains(string(data), "Private review") || strings.Contains(string(data), "Invented source") || f.Name == "translation-progress.tsv" || f.Name == "author-project.json" {
			t.Fatal("private/reference data leaked", f.Name)
		}
	}
	imported, err := ReadAuthorArchive(bytes.NewReader(shared.Bytes()), int64(shared.Len()))
	if err != nil {
		t.Fatal(err)
	}
	if imported.Notes() != "" || imported.pack.workspace.Stats().MessageCount != 2 {
		t.Fatal("publication import changed content")
	}
	if err := imported.WriteArchive(io.Discard, "publication"); err == nil {
		t.Fatal("import trusted archive export authorization")
	}
	imported, err = imported.EditMessage("action.hud.act_2", "Totally stellar!\n@end\n", TranslationDone)
	if err != nil {
		t.Fatal(err)
	}
	pub2, _, err := imported.Publication(PublicationOptions{ConfirmRights: true, IncludeWIP: true})
	if err != nil {
		t.Fatal(err)
	}
	if err := pub2.WriteArchive(io.Discard, "publication"); err != nil {
		t.Fatal(err)
	}
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	if probe != "" {
		root := t.TempDir()
		writeAuthorTestFiles(t, root, pub2.pack.Files())
		assertAuthorPackRuntime(t, probe, root, pub2.pack)
	}
	// Checking Done does not turn an unchanged native template into authored text.
	body, _ := p.pack.workspace.scripts[0].Body("action.hud.act_1")
	p, err = p.EditMessage("action.hud.act_1", body, TranslationDone)
	if err != nil {
		t.Fatal(err)
	}
	_, report, err = p.Publication(PublicationOptions{ConfirmRights: true})
	if err != nil || report.UnchangedSource != 1 {
		t.Fatal("unchanged source exported", report, err)
	}
}

func TestAuthorProjectNoticesSurviveDirectoryAndArchiveImport(t *testing.T) {
	p := authorAdventureProject(t)
	var err error
	p, err = p.WithNotice("CREDITS.txt", "Original author; translation contributors.\n")
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	writeAuthorTestFiles(t, root, p.projectFiles(true))
	fromDirectory, err := OpenAuthorProjectDirectory(root)
	if err != nil {
		t.Fatal(err)
	}
	if fromDirectory.ProjectRevision() != p.ProjectRevision() {
		t.Fatal("directory import lost notes/credits/progress")
	}
	pub, _, err := p.Publication(PublicationOptions{ConfirmRights: true})
	if err != nil {
		t.Fatal(err)
	}
	installed := filepath.Join(t.TempDir(), "installed")
	if _, err := InstallAuthorProject(installed, pub, false); err != nil {
		t.Fatal(err)
	}
	fromInstall, err := OpenAuthorProjectDirectory(installed)
	if err != nil {
		t.Fatal(err)
	}
	if fromInstall.Notices()["notices/CREDITS.txt"] != p.Notices()["notices/CREDITS.txt"] {
		t.Fatal("installed version lost its attribution notices")
	}
	var buf bytes.Buffer
	if err := pub.WriteArchive(&buf, "publication"); err != nil {
		t.Fatal(err)
	}
	fromArchive, err := ReadAuthorArchive(bytes.NewReader(buf.Bytes()), int64(buf.Len()))
	if err != nil {
		t.Fatal(err)
	}
	if fromArchive.Notices()["notices/CREDITS.txt"] != p.Notices()["notices/CREDITS.txt"] {
		t.Fatal("publication lost attribution")
	}
}

func TestAuthorStoreStaleWritesAndVersionedInstallation(t *testing.T) {
	root := t.TempDir()
	store, err := NewAuthorStore(filepath.Join(root, "projects"))
	if err != nil {
		t.Fatal(err)
	}
	p := authorAdventureProject(t)
	if err := store.Save(p, ""); err != nil {
		t.Fatal(err)
	}
	if err := store.Save(p, ""); !errors.Is(err, ErrProjectConflict) {
		t.Fatal("silent replacement", err)
	}
	opened, err := store.Open(p.pack.manifest.metadata.ID)
	if err != nil || opened.ProjectRevision() != p.ProjectRevision() {
		t.Fatal("saved project changed", err)
	}
	next, err := p.WithNotes("changed")
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Save(next, p.ProjectRevision()); err != nil {
		t.Fatal(err)
	}
	if err := store.Save(p, p.ProjectRevision()); !errors.Is(err, ErrProjectConflict) {
		t.Fatal("stale browser save accepted", err)
	}
	m := p.pack.manifest.Metadata()
	m.ID = "community.another"
	m.Name = "Another English"
	other, err := p.WithMetadata(m)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Save(other, ""); err != nil {
		t.Fatal(err)
	}
	list, err := store.List()
	if err != nil || len(list) != 2 || list[0].ID != m.ID || list[0].Locale != list[1].Locale {
		t.Fatal("same-locale identity/sorting", list, err)
	}
	pub, _, err := p.Publication(PublicationOptions{ConfirmRights: true, IncludeWIP: true})
	if err != nil {
		t.Fatal(err)
	}
	install := filepath.Join(root, "game-assets", "languages", "packs", p.pack.manifest.metadata.ID)
	manifest, err := InstallAuthorProject(install, pub, false)
	if err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(manifest)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = InstallAuthorProject(install, pub, false); !errors.Is(err, ErrProjectConflict) {
		t.Fatal("install overwrote without consent", err)
	}
	if _, err = InstallAuthorProject(install, p, false); err == nil {
		t.Fatal("private project installed without publication")
	}
	if _, err = InstallAuthorProject(install, pub, true); err != nil {
		t.Fatal(err)
	}
	after, _ := os.ReadFile(manifest)
	if bytes.Equal(before, after) {
		t.Fatal("install did not switch immutable version")
	}
	if installed, err := OpenAuthorPack(install); err != nil || installed.workspace.Stats().MessageCount != pub.pack.workspace.Stats().MessageCount {
		t.Fatal("installed pack invalid", err)
	}
	versions, _ := filepath.Glob(filepath.Join(install, "v-*"))
	if len(versions) != 2 {
		t.Fatal("previous version not retained")
	}
	for _, version := range versions {
		if _, err := OpenAuthorPack(version); err != nil {
			t.Fatal("retained version corrupt", err)
		}
	}
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	if probe != "" {
		installed, _ := OpenAuthorPack(install)
		assertAuthorPackRuntime(t, probe, install, installed)
	}
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if p.ProjectRevision() == "" {
				t.Error("missing revision")
			}
		}()
	}
	wg.Wait()
}

func TestAuthorArchiveRejectsUnsafePayloads(t *testing.T) {
	p := authorAdventureProject(t)
	pub, _, err := p.Publication(PublicationOptions{ConfirmRights: true})
	if err != nil {
		t.Fatal(err)
	}
	var valid bytes.Buffer
	pub.WriteArchive(&valid, "publication")
	for _, tc := range []struct {
		name    string
		mode    os.FileMode
		content string
	}{{"../escape", 0644, "x"}, {"/escape", 0644, "x"}, {"pack.ini", 0644, "duplicate"}, {"PACK.INI", 0644, "case collision"}, {"tool.exe", 0755, "binary"}, {"linked", os.ModeSymlink | 0777, "outside"}, {"unused.txt", 0644, "private"}, {"notices/bad.js", 0644, "script"}, {"author-project.json", 0644, "{}"}} {
		t.Run(tc.name, func(t *testing.T) {
			original, _ := zip.NewReader(bytes.NewReader(valid.Bytes()), int64(valid.Len()))
			var buffer bytes.Buffer
			w := zip.NewWriter(&buffer)
			for _, f := range original.File {
				r, _ := f.Open()
				out, _ := w.Create(f.Name)
				io.Copy(out, r)
				r.Close()
			}
			h := zip.FileHeader{Name: tc.name, Method: zip.Deflate}
			h.SetMode(tc.mode)
			out, _ := w.CreateHeader(&h)
			out.Write([]byte(tc.content))
			w.Close()
			if _, err := ReadAuthorArchive(bytes.NewReader(buffer.Bytes()), int64(buffer.Len())); err == nil {
				t.Fatal("unsafe payload accepted")
			}
		})
	}
	if _, err := ReadAuthorArchive(bytes.NewReader(valid.Bytes()), MaxAuthorArchiveBytes+1); err == nil {
		t.Fatal("oversized archive accepted")
	}
	root := t.TempDir()
	store, _ := NewAuthorStore(root)
	outside := filepath.Join(t.TempDir(), "protected")
	os.WriteFile(outside, []byte("keep"), 0600)
	if err := os.Symlink(outside, filepath.Join(root, p.pack.manifest.metadata.ID+".arproject")); err == nil {
		if err := store.Save(p, ""); err == nil {
			t.Fatal("symlink overwritten")
		}
		data, _ := os.ReadFile(outside)
		if string(data) != "keep" {
			t.Fatal("outside file changed")
		}
	}
}

func TestAuthorAtomicWriteFailureKeepsDestination(t *testing.T) {
	path := filepath.Join(t.TempDir(), "pack.ini")
	os.WriteFile(path, []byte("old"), 0600)
	err := atomicAuthorFile(path, func(w io.Writer) error { w.Write([]byte("partial")); return errors.New("injected write failure") })
	if err == nil {
		t.Fatal("failure hidden")
	}
	data, _ := os.ReadFile(path)
	if string(data) != "old" {
		t.Fatal("partial save replaced old data")
	}
	files, _ := os.ReadDir(filepath.Dir(path))
	if len(files) != 1 {
		t.Fatal("temporary file leaked")
	}
}

// A one-message edit must not recompress the pack's fonts. Saving a project
// with a script-specific font is otherwise dominated by deflating megabytes
// that did not change.
func TestAuthorStoreSaveDoesNotRecompressUnchangedFonts(t *testing.T) {
	font := make([]byte, 4<<20)
	for i := range font {
		font[i] = byte(i*7 + i/251)
	}
	copy(font, []byte{0, 1, 0, 0}) /* TTF signature; not a readable face. */
	manifest := strings.Replace(authorPackManifest, "\n[scripts]",
		"\nfallback = fonts/Large.ttf\n[scripts]", 1)
	source, err := LoadAuthorPack(packSnapshotFS(map[string][]byte{
		"pack.ini":        []byte(manifest),
		"text/sky.artext": []byte(authorConfirm + "\n:: action.hud.act_1\nInvented source label.\n@end\n"),
		"fonts/Large.ttf": font,
	}))
	if err != nil {
		t.Fatal(err)
	}
	m := source.Manifest().Metadata()
	m.ID = "community.large-font"
	m.Name = "Large font English"
	p, err := NewTranslationProject(source, m)
	if err != nil {
		t.Fatal(err)
	}
	store, err := NewAuthorStore(filepath.Join(t.TempDir(), "projects"))
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Save(p, ""); err != nil {
		t.Fatal(err)
	}
	deflateCache.Lock()
	afterFirst := deflateCache.compressions
	deflateCache.Unlock()
	if afterFirst == 0 {
		t.Fatal("the font was never compressed at all")
	}

	edited, err := p.EditMessage("action.hud.act_1", "Most excellent!\n@end\n", TranslationDone)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Save(edited, p.ProjectRevision()); err != nil {
		t.Fatal(err)
	}
	deflateCache.Lock()
	afterEdit := deflateCache.compressions
	deflateCache.Unlock()
	if afterEdit != afterFirst {
		t.Fatal("an ordinary edit recompressed unchanged fonts", afterFirst, afterEdit)
	}

	// The archive is still an ordinary, readable .arproject with the same
	// content, and stale saves still conflict.
	reopened, err := store.Open(m.ID)
	if err != nil {
		t.Fatal(err)
	}
	if reopened.ProjectRevision() != edited.ProjectRevision() ||
		reopened.Pack().RuntimeRevision() != edited.Pack().RuntimeRevision() {
		t.Fatal("saved project changed")
	}
	if !bytes.Equal(reopened.Pack().Files()["fonts/Large.ttf"], font) {
		t.Fatal("font bytes changed")
	}
	if err := store.Save(p, p.ProjectRevision()); !errors.Is(err, ErrProjectConflict) {
		t.Fatal("stale save accepted", err)
	}
}
