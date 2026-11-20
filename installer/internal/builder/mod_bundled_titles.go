package builder

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

const bundledTitleVersion = "1.0.0"

type bundledTitle struct {
	id, variant, name string
}

var bundledTitles = []bundledTitle{
	{"actraiser-hd-title-en", "en", "ActRaiser HD title · English"},
	{"actraiser-hd-title-ja", "ja", "ActRaiser HD title · Japanese"},
}

type bundledTitleRecord struct {
	Digest   string `json:"digest"`
	Previous string `json:"previous,omitempty"`
	Version  string `json:"version"`
}

func (s *modStore) bundledTitlePath(id string) string {
	return filepath.Join(s.bundledTitleDirectory(), id+".o2r")
}

func (s *modStore) bundledTitleDirectory() string {
	return filepath.Join(s.directory, ".bundled")
}

func (s *modStore) readBundledTitles() map[string]bundledTitleRecord {
	records := map[string]bundledTitleRecord{}
	directory := s.bundledTitleDirectory()
	if info, err := os.Lstat(directory); err != nil || !info.IsDir() {
		return records
	}
	content, _, err := readBoundedRegularFile(filepath.Join(directory, "titles.json"), maxModMetadata)
	if err == nil {
		if err := json.Unmarshal(content, &records); err == nil && records != nil {
			return records
		}
	}
	return map[string]bundledTitleRecord{}
}

func (s *modStore) ownsBundledTitle(pack installedMod) bool {
	record, owned := s.bundled[pack.ID]
	return owned && pack.Type == titleModType && pack.path == s.bundledTitlePath(pack.ID) &&
		pack.Error == "" && pack.digest != "" && (pack.digest == record.Digest || pack.digest == record.Previous)
}

func prepareBundledTitles(root string, dirs []string) error {
	store, unlock, err := openModStore(root, dirs)
	if err != nil {
		return err
	}
	defer unlock()
	defer store.saveIndex()
	current := func(title bundledTitle) bool {
		record := store.bundled[title.id]
		info, err := os.Lstat(store.bundledTitlePath(title.id))
		return err == nil && info.Mode().IsRegular() && record.Previous == "" &&
			record.Digest != "" && lk.CompareLanguageReleases(record.Version, bundledTitleVersion) >= 0
	}
	if slices.ContainsFunc(bundledTitles, func(title bundledTitle) bool { return !current(title) }) {
		if err := store.seedBundledTitles(); err != nil {
			return err
		}
	}
	return store.migrateBundledTitle(root)
}

func (s *modStore) migrateBundledTitle(root string) error {
	manifest, path, err := readAssetManifest(root)
	if err != nil {
		return err
	}
	item, found := legacyTitleItem(root, path, manifest)
	if !found || item.Reason != "" {
		return nil
	}
	assets, err := os.OpenRoot(s.assets)
	if err != nil {
		return err
	}
	checkLegacyModSource(assets, &item)
	_ = assets.Close()
	if !slices.ContainsFunc(bundledTitles, func(title bundledTitle) bool { return item.Pack == title.id }) {
		return nil
	}

	catalog, err := s.scan()
	if err != nil {
		return err
	}
	if !slices.ContainsFunc(catalog.Packs, func(pack installedMod) bool { return pack.Bundled && pack.ID == item.Pack }) {
		return nil // Another archive owns the ID. Leave the legacy title available.
	}
	if slices.ContainsFunc(catalog.Packs, func(pack installedMod) bool {
		return pack.Type == titleModType && pack.Enabled && pack.ID != item.Pack
	}) {
		return nil // Preserve an existing title choice and its legacy fallback.
	}
	if _, err := s.backupLegacyManifest(manifest); err != nil {
		return err
	}
	if err := s.convertLegacyMod(root, &item, &catalog); err != nil {
		return err
	}
	updated, _, err := readAssetManifest(root)
	if err != nil {
		return err
	}
	if updated != manifest {
		return errLegacyReviewChanged
	}
	for _, section := range item.Sections {
		updated = removeManifestSection(updated, section)
	}
	return writeAtomicFile(path, []byte(updated))
}

func (s *modStore) seedBundledTitles() error {
	type seed struct {
		id, path string
		content  []byte
	}
	var pending []seed
	for _, title := range bundledTitles {
		dest := s.bundledTitlePath(title.id)
		record := s.bundled[title.id]
		previous := ""
		if info, err := os.Lstat(dest); err == nil {
			// Only open known Builder-managed archives here. Launcher mods
			// are discovered by the regular catalog refresh and win any ID collision.
			if record.Digest == "" || !info.Mode().IsRegular() || info.Size() > maxTitlePNGBytes+2*maxModMetadata {
				continue
			}
			pack := s.inspectMod(dest, false, nil)
			if !s.ownsBundledTitle(pack) {
				continue
			}
			if lk.CompareLanguageReleases(pack.Version, bundledTitleVersion) >= 0 {
				if record.Previous != "" && pack.digest == record.Digest {
					record.Previous = ""
					s.bundled[title.id] = record
				}
				continue
			}
			previous = pack.digest
		} else if !errors.Is(err, os.ErrNotExist) {
			return err
		}
		art, _ := bundledTitleArtwork(title.variant)
		var archive bytes.Buffer
		metadata := modMetadata{Name: title.name, Version: bundledTitleVersion, Author: "ActRaiser Recomp"}
		if err := exportTitleMod(&archive, metadata, title.id, bytes.NewReader(art)); err != nil {
			return fmt.Errorf("package %s: %w", title.name, err)
		}
		s.bundled[title.id] = bundledTitleRecord{
			Digest: fmt.Sprintf("%x", sha256.Sum256(archive.Bytes())), Previous: previous, Version: bundledTitleVersion,
		}
		pending = append(pending, seed{title.id, dest, archive.Bytes()})
	}
	if len(pending) == 0 && len(s.bundled) == 0 {
		return nil
	}
	if err := ensureModDir(s.bundledTitleDirectory()); err != nil {
		return err
	}
	// Record ownership before publishing an archive. A fresh Builder creates
	// no activation overlay here, leaving it available for installation import.
	if err := s.saveBundledTitles(); err != nil {
		return err
	}
	for _, pack := range pending {
		if err := writeAtomicFile(pack.path, pack.content); err != nil {
			return err
		}
	}
	if len(pending) == 0 {
		return nil
	}
	for _, pack := range pending {
		record := s.bundled[pack.id]
		record.Previous = ""
		s.bundled[pack.id] = record
	}
	return s.saveBundledTitles()
}

func (s *modStore) saveBundledTitles() error {
	content, err := json.Marshal(s.bundled)
	if err != nil {
		return err
	}
	return writeAtomicFile(filepath.Join(s.bundledTitleDirectory(), "titles.json"), content)
}
