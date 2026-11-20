package builder

import (
	"bytes"
	"crypto/sha256"
	"errors"
	"fmt"
	"image/png"
	"io"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
)

const legacyModBackupDir = "legacy-mod-backup"

type legacyModItem struct {
	Type     string   `json:"type"`
	Target   string   `json:"target"`
	File     string   `json:"file"`
	Pack     string   `json:"pack,omitempty"`
	Reason   string   `json:"reason,omitempty"`
	Detail   string   `json:"detail,omitempty"`
	Existing string   `json:"existing,omitempty"`
	Sections []string `json:"-"`
	stamp    modFileStamp
	path     string
	block    string
}

type legacyModReview struct {
	Revision string          `json:"revision"`
	Items    []legacyModItem `json:"items"`
	Backup   string          `json:"backup,omitempty"`
	Rebuild  bool            `json:"rebuild,omitempty"`
	manifest string
}

func legacyAsset(root *os.Root, name string, limit int64) (*os.File, modFileStamp, error) {
	local, err := filepath.Localize(filepath.ToSlash(name))
	if err != nil {
		return nil, modFileStamp{}, err
	}
	info, err := root.Stat(local)
	if err != nil {
		return nil, modFileStamp{}, err
	}
	if !info.Mode().IsRegular() || info.Size() <= 0 || info.Size() > limit {
		return nil, modFileStamp{}, fmt.Errorf("invalid or oversized source file")
	}
	f, err := root.Open(local)
	if err != nil {
		return nil, modFileStamp{}, err
	}
	current, err := f.Stat()
	if err != nil || !os.SameFile(info, current) {
		_ = f.Close()
		return nil, modFileStamp{}, fmt.Errorf("source file changed while opening")
	}
	return f, readModFileStamp(f, current), nil
}

func legacyMusicItems(project, manifestPath, manifest string) []legacyModItem {
	var items []legacyModItem
	type musicRule struct {
		source string
		gated  bool
	}
	// Moving a rule to the overlay changes its ordering relative to hand-
	// authored rules of equal specificity. Leave that whole set in place.
	unsupported := map[musicRule]bool{}
	rules := []musicRule{}
	seen := map[string]int{}
	for _, section := range manifestSections(manifest) {
		id, music := strings.CutPrefix(section.Name, "music:")
		if !music {
			continue
		}
		seen[id]++
		block := manifest[section.Start:section.End]
		values := manifestSectionValues(block)
		file := values["file"]
		if file == "" || !regularFileExists(resolveManifestFile(manifestPath, file)) {
			continue
		}
		item := legacyModItem{Type: musicModType, Target: id, File: file, Sections: []string{section.Name}, block: block}
		spec, supported := musicModTargets[id]
		rule := musicRule{legacyMusicSource(values["src"]), values["when"] != ""}
		if !supported || !strings.EqualFold(values["src"], spec.Song.Src) ||
			withoutModWhitespace(values["when"]) != withoutModWhitespace(spec.Gate) || values["song"] != "" {
			item.Reason = "music_rules"
		} else if path, ok := installedAudioPath(project, manifestPath, file); !ok {
			item.Reason = "source"
		} else {
			item.path, _ = filepath.Rel(filepath.Join(project, "game-assets"), path)
			if _, err := exportedModTrack(id, spec, values); err != nil {
				item.Reason = "music_rules"
			}
		}
		if item.Reason != "" {
			unsupported[rule] = true
		}
		rules = append(rules, rule)
		items = append(items, item)
	}
	for i := range items {
		if unsupported[rules[i]] || seen[items[i].Target] > 1 {
			items[i].Reason = "music_rules"
		}
	}
	return items
}

func legacyMusicSource(raw string) string {
	bank, address, found := strings.Cut(strings.ToLower(raw), ":")
	b, bankErr := strconv.ParseUint(strings.TrimPrefix(strings.TrimSpace(bank), "0x"), 16, 8)
	a, addressErr := strconv.ParseUint(strings.TrimPrefix(strings.TrimSpace(address), "0x"), 16, 16)
	if !found || bankErr != nil || addressErr != nil {
		return raw
	}
	return fmt.Sprintf("%02x:%04x", b, a)
}

func legacyTitleItem(project, manifestPath, manifest string) (legacyModItem, bool) {
	logo, _ := manifestSectionValue(manifest, "replace:title-logo", "image")
	swirl, _ := manifestSectionValue(manifest, "replace:title-swirl", "image")
	logoExists := regularFileExists(resolveManifestFile(manifestPath, logo))
	swirlExists := regularFileExists(resolveManifestFile(manifestPath, swirl))
	if !logoExists && !swirlExists {
		return legacyModItem{}, false
	}
	item := legacyModItem{Type: titleModType, Target: titleModTarget, File: logo,
		Sections: []string{"replace:title-logo", "replace:title-swirl"}}
	if logo == "" {
		item.File = swirl
	}
	target, inside := manifestAudioTarget(project, manifestPath, logo)
	if !inside || !logoExists || !swirlExists || logo != swirl {
		item.Reason = "title_rules"
		return item, true
	}
	logoFields, swirlFields := titleManifestValues(logo)
	for n, expected := range [][]manifestValue{logoFields, swirlFields} {
		values := make(map[string]string, len(expected))
		for _, field := range expected {
			values[field.Key] = field.Value
		}
		count := 0
		for _, section := range manifestSections(manifest) {
			if section.Name == item.Sections[n] {
				count++
				if !maps.Equal(manifestSectionValues(manifest[section.Start:section.End]), values) {
					item.Reason = "title_rules"
				}
			}
		}
		if count != 1 {
			item.Reason = "title_rules"
		}
	}
	item.path, _ = filepath.Rel(filepath.Join(project, "game-assets"), target)
	return item, true
}

func reviewLegacyMods(project string, summary bool) (legacyModReview, error) {
	manifest, path, err := readAssetManifest(project)
	review := legacyModReview{manifest: manifest}
	if err != nil {
		return review, err
	}
	review.Items = legacyMusicItems(project, path, manifest)
	if title, ok := legacyTitleItem(project, path, manifest); ok {
		review.Items = append(review.Items, title)
	}
	manual := legacyModItem{Type: manualModType, Target: manualModTarget, File: manualRelativePath, path: manualRelativePath}
	if _, err := os.Lstat(manualPath(project)); !errors.Is(err, os.ErrNotExist) {
		review.Items = append(review.Items, manual)
	}
	if summary || len(review.Items) == 0 {
		return review, nil
	}
	assets, err := os.OpenRoot(filepath.Join(project, "game-assets"))
	if err != nil {
		return review, err
	}
	defer assets.Close()
	revision := sha256.New()
	revision.Write([]byte(manifest))
	for i := range review.Items {
		item := &review.Items[i]
		if item.Reason == "" {
			checkLegacyModSource(assets, item)
		}
		fmt.Fprintf(revision, "\n%s %s %s %v", item.Target, item.Pack, item.Reason, item.stamp)
	}
	review.Revision = fmt.Sprintf("%x", revision.Sum(nil))
	return review, nil
}

func checkLegacyModSource(assets *os.Root, item *legacyModItem) {
	limit := int64(maxTrackBytes)
	switch item.Type {
	case titleModType:
		limit = maxTitlePNGBytes
	case manualModType:
		limit = maxManualBytes
	}
	f, stamp, err := legacyAsset(assets, item.path, limit)
	if err != nil {
		item.Reason = "source"
		return
	}
	defer f.Close()
	item.stamp = stamp
	switch item.Type {
	case titleModType:
		for _, title := range bundledTitles {
			art, _ := bundledTitleArtwork(title.variant)
			if item.File == bundledTitleRelativePath(art) && stamp.Size == int64(len(art)) {
				data, err := io.ReadAll(io.LimitReader(f, stamp.Size+1))
				if err == nil && bytes.Equal(data, art) {
					item.Pack = title.id
					return
				}
				_, _ = f.Seek(0, io.SeekStart)
			}
		}
		config, err := png.DecodeConfig(f)
		if err != nil || int64(config.Width)*int64(config.Height) > maxTitlePixels {
			item.Reason = "title_format"
		}
	case manualModType:
		info, err := f.Stat()
		if err != nil || !info.Mode().IsRegular() {
			item.Reason = "source"
			break
		}
		if linkInfo, err := assets.Lstat(item.path); err != nil || !os.SameFile(info, linkInfo) {
			item.Reason = "source"
			break
		}
		if _, err := copyModManual(io.Discard, f); err != nil {
			item.Reason = "manual_format"
		}
	case musicModType:
		if _, err := readModVorbisHeader(f); err != nil {
			item.Reason = "music_format"
		}
	}
	if item.Reason == "" {
		fingerprint := sha256.New()
		fmt.Fprintf(fingerprint, "%s\n%s\n%v\n%s", item.Target, item.File, stamp, item.block)
		item.Pack = fmt.Sprintf("legacy-%s-%x", item.Target, fingerprint.Sum(nil)[:8])
	}
}

// Backup and publish in that order. A cancelled or interrupted run can leave
// both the original configuration and its active mod in place; the same
// review then finds the deterministic archive and completes retirement.
func applyLegacyMods(project string, dirs []string, revision string) (legacyModReview, error) {
	store, unlock, err := openModStore(project, dirs)
	if err != nil {
		return legacyModReview{}, err
	}
	defer unlock()
	defer store.saveIndex()
	review, err := reviewLegacyMods(project, false)
	if err != nil {
		return review, err
	}
	if revision == "" || review.Revision != revision {
		return review, errLegacyReviewChanged
	}
	catalog, err := store.scan()
	if err != nil {
		return review, err
	}
	if _, err := store.render(&catalog); err != nil {
		return review, err
	}
	review.existingSelections(catalog)
	backup := filepath.Join(store.assets, legacyModBackupDir)
	keep := review.manifest
	for i := range review.Items {
		item := &review.Items[i]
		if item.Reason != "" {
			continue
		}
		if review.Backup == "" {
			review.Backup, err = store.backupLegacyManifest(review.manifest)
			if err != nil {
				return review, err
			}
		}
		if err := store.convertLegacyMod(project, item, &catalog); err != nil {
			item.Reason = "conversion"
			item.Detail = err.Error()
			continue
		}
		if !slices.ContainsFunc(catalog.Packs, func(pack installedMod) bool { return pack.ID == item.Pack && pack.Enabled }) {
			item.Reason = "disabled"
			continue
		}
		if item.Type == manualModType {
			// Moving the file also creates its recovery copy atomically.
			dest := filepath.Join(backup, item.Pack+".pdf")
			if _, err := os.Lstat(dest); !errors.Is(err, os.ErrNotExist) {
				item.Reason = "conversion"
				item.Detail = "manual recovery path already exists"
				continue
			}
			f, err := os.Open(manualPath(project))
			if err != nil {
				item.Reason, item.Detail = "conversion", err.Error()
				continue
			}
			same := modArchiveUnchanged(f, item.stamp)
			_ = f.Close()
			if !same {
				item.Reason, item.Detail = "conversion", errLegacyReviewChanged.Error()
				continue
			}
			if err := os.Rename(manualPath(project), dest); err != nil {
				item.Reason = "conversion"
				item.Detail = err.Error()
			}
		} else {
			for _, section := range item.Sections {
				keep = removeManifestSection(keep, section)
			}
		}
	}
	if keep != review.manifest {
		if err := writeAtomicFile(liveAssetManifestPath(project), []byte(keep)); err != nil {
			return review, err
		}
	}
	return review, nil
}

func (s *modStore) backupLegacyManifest(manifest string) (string, error) {
	backup := filepath.Join(s.assets, legacyModBackupDir)
	if err := ensureModDir(backup); err != nil {
		return "", err
	}
	filename := fmt.Sprintf("manifest-%x.ini", sha256.Sum256([]byte(manifest)))
	if err := writeAtomicFile(filepath.Join(backup, filename), []byte(manifest)); err != nil {
		return "", err
	}
	return filepath.ToSlash(filepath.Join("game-assets", legacyModBackupDir, filename)), nil
}

func (review *legacyModReview) existingSelections(catalog modCatalog) {
	for i := range review.Items {
		item := &review.Items[i]
		if item.Reason != "" {
			continue
		}
		for _, slot := range catalog.Slots {
			if slot.ID != item.Target || slot.Winner == item.Pack {
				continue
			}
			if slot.Winner == "" {
				item.Reason = "conflict"
			} else if active := slices.IndexFunc(catalog.Packs, func(pack installedMod) bool { return pack.ID == slot.Winner }); active >= 0 {
				item.Existing = catalog.Packs[active].Name
			}
		}
	}
}

var errLegacyReviewChanged = errors.New("legacy customizations changed; review them again")

func (s *modStore) convertLegacyMod(project string, item *legacyModItem, catalog *modCatalog) error {
	if strings.HasPrefix(item.Pack, "actraiser-hd-title-") {
		index := slices.IndexFunc(catalog.Packs, func(pack installedMod) bool { return pack.ID == item.Pack && pack.Bundled && pack.Error == "" })
		if index < 0 {
			return fmt.Errorf("the bundled title pack is unavailable")
		}
		pack := &catalog.Packs[index]
		enabled := pack.Enabled
		// A conversion preserves a legacy fallback alongside an existing mod.
		// Only an explicit enable asks to switch between the included titles.
		s.setBundledTitleEnabled(pack, true)
		if err := s.publish(catalog); err != nil {
			s.setBundledTitleEnabled(pack, enabled)
			return err
		}
		return nil
	}
	archive, err := os.CreateTemp(s.cache, ".legacy-pack-*")
	if err != nil {
		return err
	}
	defer os.Remove(archive.Name())
	defer archive.Close()
	metadata := modMetadata{Name: "Imported " + item.Target, Version: "1.0.0", Author: "Local customization"}
	assets, err := os.OpenRoot(s.assets)
	if err != nil {
		return err
	}
	defer assets.Close()
	f, stamp, err := legacyAsset(assets, item.path, item.stamp.Size)
	if err != nil {
		return err
	}
	defer f.Close()
	if stamp != item.stamp {
		return errLegacyReviewChanged
	}
	switch item.Type {
	case musicModType:
		request := modExportRequest{modMetadata: metadata, ID: item.Pack, Targets: []string{item.Target}}
		err = exportMusicMod(project, archive, request)
	case titleModType:
		err = exportTitleMod(archive, metadata, item.Pack, f)
	default:
		err = exportManualMod(archive, metadata, item.Pack, f)
	}
	if err != nil {
		return err
	}
	if !modArchiveUnchanged(f, stamp) {
		return errLegacyReviewChanged
	}
	if _, err := archive.Seek(0, io.SeekStart); err != nil {
		return err
	}
	return s.install(archive, catalog, item.Type, nil)
}
