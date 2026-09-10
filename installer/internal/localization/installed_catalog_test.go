package localization

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestInstalledCatalogAndRecoverableUninstall(t *testing.T) {
	project := authorAdventureProject(t)
	pub, _, err := project.Publication(PublicationOptions{ConfirmRights: true, IncludeWIP: true})
	if err != nil {
		t.Fatal(err)
	}
	root := filepath.Join(t.TempDir(), "packs")
	id := pub.Pack().Manifest().Metadata().ID
	dir := filepath.Join(root, id)
	if rows, err := ListInstalledPacks(root); err != nil || len(rows) != 0 {
		t.Fatal(rows, err)
	}
	manifest, err := InstallAuthorProject(dir, pub, false)
	if err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(manifest)
	rows, err := ListInstalledPacks(root)
	if err != nil || len(rows) != 1 || rows[0].Metadata.ID != id || rows[0].Error != "" {
		t.Fatal(rows, err)
	}
	if _, err := UninstallLanguagePack(root, id, id, "stale"); !errors.Is(err, ErrProjectConflict) {
		t.Fatal("stale uninstall accepted", err)
	}
	for _, key := range []string{"../outside", "..", ".", "/", `..\outside`, ""} {
		if _, err := UninstallLanguagePack(root, key, id, rows[0].Revision); err == nil {
			t.Fatal("unsafe directory accepted", key)
		}
	}
	unlock, err := authorLock(dir)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := UninstallLanguagePack(root, id, id, rows[0].Revision); err == nil {
		t.Fatal("ignored active install lock")
	}
	unlock()
	backup, err := UninstallLanguagePack(root, id, id, rows[0].Revision)
	if err != nil {
		t.Fatal(err)
	}
	if got, err := os.ReadFile(backup); err != nil || string(got) != string(before) {
		t.Fatal("recovery manifest lost", err)
	}
	if _, err := os.Stat(manifest); !os.IsNotExist(err) {
		t.Fatal("pack still discoverable", err)
	}
	if got, err := ListInstalledPacks(root); err != nil || len(got) != 0 {
		t.Fatal(got, err)
	}
	if _, err := InstallAuthorProject(dir, pub, false); err != nil {
		t.Fatal("reinstall failed", err)
	}
	if _, err := os.Stat(backup); err != nil {
		t.Fatal("reinstall removed recovery copy", err)
	}
}

func TestInstalledCatalogRejectsSymlinks(t *testing.T) {
	root := t.TempDir()
	outside := t.TempDir()
	if err := os.Symlink(outside, filepath.Join(root, "linked")); err != nil {
		t.Skip(err)
	}
	rows, err := ListInstalledPacks(root)
	if err != nil || len(rows) != 1 || rows[0].Error == "" {
		t.Fatal(rows, err)
	}
	if _, err := UninstallLanguagePack(root, "linked", "test", "anything"); err == nil {
		t.Fatal("followed linked pack")
	}
	if _, err := ListInstalledPacks(filepath.Join(root, "linked")); err == nil {
		t.Fatal("followed linked root")
	}
	if entries, _ := os.ReadDir(outside); len(entries) != 0 {
		t.Fatal("changed outside directory")
	}
	dir := filepath.Join(root, "regular")
	if err := os.Mkdir(dir, 0755); err != nil {
		t.Fatal(err)
	}
	file := filepath.Join(outside, "manifest.ini")
	if err := os.WriteFile(file, []byte("sentinel"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(file, filepath.Join(dir, "pack.ini")); err != nil {
		t.Fatal(err)
	}
	if _, err := UninstallLanguagePack(root, "regular", "test", "anything"); err == nil {
		t.Fatal("followed linked manifest")
	}
	if got, _ := os.ReadFile(file); string(got) != "sentinel" {
		t.Fatal("changed linked target")
	}
}

func TestInstalledChecklistPreservesFilesAndDisabledUpdates(t *testing.T) {
	p, _, err := authorAdventureProject(t).Installation()
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	id := p.Pack().Manifest().Metadata().ID
	dir := filepath.Join(root, id)
	manifest, err := InstallAuthorProject(dir, p, false)
	if err != nil {
		t.Fatal(err)
	}
	original, _ := os.ReadFile(manifest)
	row, err := InspectInstalledPack(root, id)
	if err != nil || !row.Enabled {
		t.Fatal(row, err)
	}
	if err := SetLanguagePackEnabled(root, id, id, row.Revision, false); err != nil {
		t.Fatal(err)
	}
	disabled, err := InspectInstalledPack(root, id)
	if err != nil || disabled.Enabled || row.Revision == disabled.Revision {
		t.Fatal(disabled, err)
	}
	if rows, err := ListInstalledPacks(root); err != nil || len(rows) != 1 || rows[0].Enabled {
		t.Fatal(rows, err)
	}
	if _, err := os.Stat(manifest); !os.IsNotExist(err) {
		t.Fatal("disabled pack discoverable", err)
	}
	if _, err := openInstalledAuthorPack(dir, disabledPackManifest); err != nil {
		t.Fatal("files lost", err)
	}
	if err := SetLanguagePackEnabled(root, id, id, row.Revision, true); !errors.Is(err, ErrProjectConflict) {
		t.Fatal("stale toggle accepted", err)
	}
	if err := SetLanguagePackEnabled(root, id, "wrong.id", disabled.Revision, true); !errors.Is(err, ErrProjectConflict) {
		t.Fatal("wrong ID accepted", err)
	}
	if err := SetLanguagePackEnabled(root, id, id, disabled.Revision, true); err != nil {
		t.Fatal(err)
	}
	if got, _ := os.ReadFile(manifest); string(got) != string(original) {
		t.Fatal("toggle changed manifest bytes")
	}
	row, _ = InspectInstalledPack(root, id)
	if err := SetLanguagePackEnabled(root, id, id, row.Revision, false); err != nil {
		t.Fatal(err)
	}
	if _, err := InstallAuthorProject(dir, p, false); err == nil {
		t.Fatal("disabled pack replaced without permission")
	}
	updated, err := InstallAuthorProject(dir, p, true)
	if err != nil || filepath.Base(updated) != disabledPackManifest {
		t.Fatal(updated, err)
	}
	row, err = InspectInstalledPack(root, id)
	if err != nil || row.Enabled {
		t.Fatal("update re-enabled pack", row, err)
	}
	unlock, err := authorLock(dir)
	if err != nil {
		t.Fatal(err)
	}
	lockedErr := SetLanguagePackEnabled(root, id, id, row.Revision, true)
	unlock()
	if lockedErr == nil {
		t.Fatal("toggle ignored lock")
	}
	beforeUninstall, _ := os.ReadFile(updated)
	backup, err := UninstallLanguagePack(root, id, id, row.Revision)
	if err != nil {
		t.Fatal(err)
	}
	if got, _ := os.ReadFile(backup); string(got) != string(beforeUninstall) {
		t.Fatal("disabled uninstall lost manifest")
	}
	if rows, err := ListInstalledPacks(root); err != nil || len(rows) != 0 {
		t.Fatal(rows, err)
	}
}

func TestInstalledChecklistRejectsBrokenAndAmbiguousPacks(t *testing.T) {
	p, _, err := authorAdventureProject(t).Installation()
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	id := p.Pack().Manifest().Metadata().ID
	dir := filepath.Join(root, id)
	manifest, err := InstallAuthorProject(dir, p, false)
	if err != nil {
		t.Fatal(err)
	}
	row, _ := InspectInstalledPack(root, id)
	for _, key := range []string{"../outside", "..", ".", "/", `..\outside`, ""} {
		if err := SetLanguagePackEnabled(root, key, id, row.Revision, false); err == nil {
			t.Fatal("unsafe path accepted", key)
		}
	}
	if err := SetLanguagePackEnabled(root, id, id, row.Revision, false); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, disabledPackManifest)
	data, _ := os.ReadFile(path)
	// Missing scripts are rejected before the discovery manifest is restored.
	pack, err := openInstalledAuthorPack(dir, disabledPackManifest)
	if err != nil {
		t.Fatal(err)
	}
	for name := range pack.Files() {
		if filepath.Ext(name) == ".artext" {
			if err := os.Remove(filepath.Join(dir, filepath.FromSlash(name))); err != nil {
				t.Fatal(err)
			}
			break
		}
	}
	row, _ = InspectInstalledPack(root, id)
	if err := SetLanguagePackEnabled(root, id, id, row.Revision, true); err == nil {
		t.Fatal("enabled missing script")
	}
	if _, err := os.Stat(manifest); !os.IsNotExist(err) {
		t.Fatal("failed enable published manifest")
	}
	if err := os.WriteFile(manifest, data, 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := InspectInstalledPack(root, id); err == nil {
		t.Fatal("ambiguous manifests accepted")
	}
	if err := SetLanguagePackEnabled(root, id, id, row.Revision, true); err == nil {
		t.Fatal("overwrote existing active manifest")
	}
}
