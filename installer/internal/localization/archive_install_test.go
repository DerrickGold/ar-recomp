package localization

import (
	"archive/zip"
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func distributionFixture(t *testing.T, name string) []byte {
	t.Helper()
	p := authorAdventureProject(t)
	m := p.Pack().Manifest().Metadata()
	m.Name = name
	p, err := p.WithMetadata(m)
	if err != nil {
		t.Fatal(err)
	}
	p, _, err = p.Publication(PublicationOptions{ConfirmRights: true, IncludeWIP: true})
	if err != nil {
		t.Fatal(err)
	}
	var b bytes.Buffer
	if err := p.WriteArchive(&b, "publication"); err != nil {
		t.Fatal(err)
	}
	return b.Bytes()
}

func putDistribution(t *testing.T, root, name string, data []byte) string {
	t.Helper()
	path := filepath.Join(root, name)
	if err := os.WriteFile(path, data, 0644); err != nil {
		t.Fatal(err)
	}
	return path
}

func prepareDistribution(t *testing.T, root string) string {
	t.Helper()
	if err := PrepareLanguageArchives(context.Background(), root, io.Discard); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(root, archiveCacheDirectory, "catalog-v1.tsv"))
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

func TestArchiveDistributionLifecycleAndCache(t *testing.T) {
	root := t.TempDir()
	data := distributionFixture(t, "Excellent English")
	path := putDistribution(t, root, "A fun name Français.arlang", data)
	rows, err := ListInstalledPacks(root)
	if err != nil || len(rows) != 1 || rows[0].Error != "" || !rows[0].Archive || !rows[0].Enabled {
		t.Fatal(rows, err)
	}
	row := rows[0]
	first := prepareDistribution(t, root)
	fields := strings.Split(strings.TrimSpace(strings.TrimPrefix(first, ArchiveCatalogHeader)), "\t")
	if len(fields) != 3 || fields[0] != "pack" || fields[1] != row.Metadata.ID {
		t.Fatal(first)
	}
	manifest := filepath.Join(root, archiveCacheDirectory, filepath.FromSlash(fields[2]), "pack.ini")
	pack, err := OpenAuthorPack(filepath.Dir(manifest))
	if err != nil || pack.Manifest().Metadata().ID != row.Metadata.ID || pack.Manifest().Version() != 1 {
		t.Fatal(err)
	}
	if probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE"); probe != "" {
		if out, err := exec.Command(probe, manifest).CombinedOutput(); err != nil {
			t.Fatal(err, string(out))
		}
	}
	if again := prepareDistribution(t, root); again != first {
		t.Fatal("unchanged archive did not reuse its cache", again)
	}
	// Same-length cache corruption is repaired into a new snapshot, not served
	// as a valid cache hit or overwritten under a running game's manifest path.
	script := filepath.Join(filepath.Dir(manifest), pack.Manifest().Sources()[0])
	before, _ := os.ReadFile(script)
	corrupt := append([]byte{}, before...)
	corrupt[len(corrupt)-1] ^= 1
	if err := os.WriteFile(script, corrupt, 0644); err != nil {
		t.Fatal(err)
	}
	repaired := prepareDistribution(t, root)
	if repaired == first {
		t.Fatal("corrupt cache was reused")
	}
	if current, _ := os.ReadFile(script); !bytes.Equal(current, corrupt) {
		t.Fatal("repaired by overwriting a previously published snapshot")
	}
	// Restore the test's old snapshot before testing update preservation below.
	if err := os.WriteFile(script, before, 0644); err != nil {
		t.Fatal(err)
	}
	if err := SetLanguagePackEnabled(root, row.Key, row.Metadata.ID, row.Revision, false); err != nil {
		t.Fatal(err)
	}
	if got := prepareDistribution(t, root); got != ArchiveCatalogHeader {
		t.Fatal("disabled archive still selectable", got)
	}
	if current, _ := os.ReadFile(path); !bytes.Equal(current, data) {
		t.Fatal("disable modified archive bytes")
	}
	// State belongs to the ID, not the arbitrary filename or archive digest.
	newPath := filepath.Join(root, "renamed.arlang")
	if err := os.Rename(path, newPath); err != nil {
		t.Fatal(err)
	}
	updated := putDistribution(t, t.TempDir(), "new.arlang", distributionFixture(t, "Updated English"))
	if _, _, err := InstallLanguageArchive(root, updated, false); err == nil {
		t.Fatal("replaced without consent")
	}
	if installed, report, err := InstallLanguageArchive(root, updated, true); err != nil || installed != newPath || report.Upgrade == nil {
		t.Fatal(installed, err)
	}
	upgraded, err := OpenAuthorInput(newPath)
	if err != nil || upgraded.Pack().Manifest().Version() != 2 || upgraded.Pack().Manifest().Metadata().ID != row.Metadata.ID {
		t.Fatal("archive install did not upgrade with the same ID", err)
	}
	original, err := OpenAuthorInput(updated)
	if err != nil || original.Pack().Manifest().Version() != 1 {
		t.Fatal("archive install changed the source", err)
	}
	rows, err = ListInstalledPacks(root)
	if err != nil || len(rows) != 1 || rows[0].Enabled {
		t.Fatal(rows, err)
	}
	row = rows[0]
	if err := SetLanguagePackEnabled(root, row.Key, row.Metadata.ID, row.Revision, true); err != nil {
		t.Fatal(err)
	}
	second := prepareDistribution(t, root)
	if second == first {
		t.Fatal("updated archive reused stale content")
	}
	if _, err := OpenAuthorPack(filepath.Dir(manifest)); err != nil {
		t.Fatal("update damaged snapshot used by running game", err)
	}
	row, _ = InspectInstalledPack(root, row.Key)
	backup, err := UninstallLanguagePack(root, row.Key, row.Metadata.ID, row.Revision)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(backup); err != nil {
		t.Fatal("recovery archive missing", err)
	}
	if got := prepareDistribution(t, root); got != ArchiveCatalogHeader {
		t.Fatal("removed archive still selectable", got)
	}
}

func TestArchiveConflictsRecoveryAndCacheIsolation(t *testing.T) {
	root := t.TempDir()
	data := distributionFixture(t, "Excellent")
	putDistribution(t, root, "one.arlang", data)
	putDistribution(t, root, "two.arlang", data)
	rows, _ := ListInstalledPacks(root)
	if len(rows) != 2 || rows[0].Error == "" || rows[1].Error == "" {
		t.Fatal(rows)
	}
	index := prepareDistribution(t, root)
	if strings.Contains(index, "pack\t") || !strings.Contains(index, "conflict\t") {
		t.Fatal(index)
	}
	if err := SetLanguagePackEnabled(root, rows[0].Key, rows[0].Metadata.ID, rows[0].Revision, false); err != nil {
		t.Fatal(err)
	}
	if got := prepareDistribution(t, root); got != ArchiveCatalogHeader {
		t.Fatal(got)
	}
	// A symbolic-link cache must never cause writes outside the pack root.
	otherRoot, outside := t.TempDir(), t.TempDir()
	putDistribution(t, otherRoot, "one.arlang", data)
	if err := os.Symlink(outside, filepath.Join(otherRoot, archiveCacheDirectory)); err == nil {
		if err := PrepareLanguageArchives(context.Background(), otherRoot, io.Discard); err == nil {
			t.Fatal("followed cache symlink")
		}
		if files, _ := os.ReadDir(outside); len(files) != 0 {
			t.Fatal("wrote outside cache")
		}
	}
	// GUI drafts cannot silently shadow an archive installation.
	draft, _, _ := authorAdventureProject(t).Installation()
	if _, err := InstallProjectInLibrary(root, draft, true); err == nil {
		t.Fatal("created a shadowing draft directory")
	}
}

func TestOrdinaryZipDirectoriesAndPublicationOnly(t *testing.T) {
	data := distributionFixture(t, "Directory ZIP")
	z, _ := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	var b bytes.Buffer
	w := zip.NewWriter(&b)
	for _, path := range []string{"text/", "notices/"} {
		h := &zip.FileHeader{Name: path}
		h.SetMode(os.ModeDir | 0755)
		if _, err := w.CreateHeader(h); err != nil {
			t.Fatal(err)
		}
	}
	for _, member := range z.File {
		r, _ := member.Open()
		dst, _ := w.CreateHeader(&member.FileHeader)
		if _, err := io.Copy(dst, r); err != nil {
			t.Fatal(err)
		}
		r.Close()
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := ReadAuthorArchive(bytes.NewReader(b.Bytes()), int64(b.Len())); err != nil {
		t.Fatal(err)
	}
	var backup bytes.Buffer
	if err := authorAdventureProject(t).WriteArchive(&backup, "backup"); err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	putDistribution(t, root, "not-a-publication.arlang", backup.Bytes())
	if index := prepareDistribution(t, root); index != ArchiveCatalogHeader {
		t.Fatal(index)
	}
	if _, _, err := InstallLanguageArchive(t.TempDir(), filepath.Join(root, "not-a-publication.arlang"), false); err == nil {
		t.Fatal("installed private backup")
	}
}

func TestShippedExamplePublication(t *testing.T) {
	p, err := OpenAuthorProjectDirectory("../../../examples/language-pack")
	if err != nil {
		t.Fatal(err)
	}
	p, err = p.ReviewedAll()
	if err != nil {
		t.Fatal(err)
	}
	p, _, err = p.Publication(PublicationOptions{ConfirmRights: true})
	if err != nil {
		t.Fatal(err)
	}
	var expected bytes.Buffer
	if err := p.WriteArchive(&expected, "publication"); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile("../../../examples/example.fr-ca.arlang")
	if err != nil || !bytes.Equal(data, expected.Bytes()) {
		t.Fatal("regenerate authored example publication", err)
	}
	if _, err := ReadAuthorArchive(bytes.NewReader(data), int64(len(data))); err != nil {
		t.Fatal(err)
	}
}
