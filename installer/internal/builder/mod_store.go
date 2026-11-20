package builder

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/desktop"
	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"github.com/DerrickGold/ar-recomp/installer/internal/modconfig"
)

const modOverlayName = modconfig.OverlayName
const maxDiscoveredMods = 128

type installedMod struct {
	modMetadata
	modPayload
	Filename          string   `json:"filename"`
	External          bool     `json:"external"`
	Bundled           bool     `json:"bundled,omitempty"`
	Enabled           bool     `json:"enabled"`
	Error             string   `json:"error,omitempty"`
	Previews          []string `json:"previews,omitempty"`
	UninstallRevision string   `json:"uninstallRevision,omitempty"`
	ArchiveURL        string   `json:"archiveURL,omitempty"`
	ManualPages       int      `json:"manualPages,omitempty"`
	stamp             modFileStamp
	digest            string
	path              string
	language          *lk.PreparedLanguage
	manualBook        manualAlbumVerdict
}

type modSlot struct {
	ID         string   `json:"id"`
	Track      string   `json:"track,omitempty"`
	Type       string   `json:"type"`
	Region     string   `json:"region,omitempty"`
	Candidates []string `json:"candidates"`
	Winner     string   `json:"winner,omitempty"`
}

type modCatalog struct {
	Packs                 []installedMod `json:"packs"`
	Slots                 []modSlot      `json:"slots"`
	LegacyManual          bool           `json:"legacyManual,omitempty"`
	InstalledLegacyManual bool           `json:"installedLegacyManual,omitempty"`
	InstalledType         string         `json:"installedType,omitempty"`
	InstalledID           string         `json:"installedId,omitempty"`
	InstalledName         string         `json:"installedName,omitempty"`
	TitleBundleConflicts  []string       `json:"titleBundleConflicts,omitempty"`
	languages             []installedMod
	languageGroups        []languageModGroup
	legacyLanguages       []lk.InstalledPackSummary
	legacyLanguageError   error
}

type modChange struct {
	Action       string `json:"action"`
	Pack         string `json:"pack"`
	Slot         string `json:"slot,omitempty"`
	Release      string `json:"release,omitempty"`
	Filename     string `json:"filename,omitempty"`
	Revision     string `json:"revision,omitempty"`
	InstallType  string `json:"-"`
	LanguageOnly bool   `json:"-"`
}

type modStore struct {
	assets       string
	directory    string
	cache        string
	settings     modconfig.State
	bundled      map[string]bundledTitleRecord
	index        map[string]*modIndexEntry
	indexChanged bool
}

func openModStore(root string, directories []string) (*modStore, func(), error) {
	abs, err := filepath.Abs(root)
	if err != nil {
		return nil, nil, err
	}
	unlock, err := desktop.LockGameData(abs)
	if err != nil {
		return nil, nil, err
	}
	assets := filepath.Join(abs, "game-assets")
	store := &modStore{
		assets: assets, directory: filepath.Join(assets, "mods"),
		cache: filepath.Join(assets, "mods-cache"),
	}
	if err := store.prepare(directories); err != nil {
		unlock()
		return nil, nil, err
	}
	store.index = readModIndex(store.cache)
	store.bundled = store.readBundledTitles()
	return store, unlock, nil
}

func (s *modStore) prepare(directories []string) error {
	for _, dir := range []string{s.assets, s.directory, s.cache} {
		if err := ensureModDir(dir); err != nil {
			return err
		}
	}
	overlay := filepath.Join(s.assets, modOverlayName)
	var err error
	s.settings, err = modconfig.Read(overlay)
	if err != nil {
		return err
	}
	root := filepath.Dir(s.assets)
	saved := slices.DeleteFunc(slices.Clone(s.settings.Directories), func(directory string) bool { return !filepath.IsAbs(directory) })
	for _, directory := range append(saved, directories...) {
		if directory == "" {
			return fmt.Errorf("empty mod directory")
		}
		full, err := filepath.Abs(directory)
		if err != nil {
			return err
		}
		if full != s.directory && full != s.bundledTitleDirectory() {
			if err := s.settings.Register(root, full); err != nil {
				return err
			}
		}
	}
	return nil
}

func ensureModDir(path string) error {
	if err := os.Mkdir(path, 0755); err != nil && !errors.Is(err, os.ErrExist) {
		return err
	}
	if info, err := os.Lstat(path); err != nil {
		return err
	} else if !info.IsDir() {
		return fmt.Errorf("%s must be a directory", path)
	}
	return nil
}

func (s *modStore) scan() (modCatalog, error) {
	catalog := modCatalog{Packs: []installedMod{}, Slots: []modSlot{}}
	index := make(map[string]*modIndexEntry, len(s.index))
	seen := map[string]int{}
	duplicate := map[string]bool{}
	for _, dir := range append([]string{s.directory}, s.settings.ResolveDirectories(filepath.Dir(s.assets))...) {
		entries, err := os.ReadDir(dir)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			if dir == s.directory {
				return catalog, fmt.Errorf("scan mod directory: %w", err)
			}
			catalog.Packs = append(catalog.Packs, installedMod{Filename: dir, External: true, Error: err.Error()})
			continue
		}
		for _, entry := range entries {
			if entry.IsDir() || !strings.EqualFold(filepath.Ext(entry.Name()), ".o2r") {
				continue
			}
			if len(catalog.Packs)+len(catalog.languages) >= maxDiscoveredMods {
				return catalog, fmt.Errorf("too many installed mods (limit %d)", maxDiscoveredMods)
			}
			path := filepath.Join(dir, entry.Name())
			pack := s.inspectMod(path, dir != s.directory, index)
			if pack.Error != "" {
				catalog.Packs = append(catalog.Packs, pack)
				continue
			}
			if pack.Type == languageModType {
				catalog.languages = append(catalog.languages, pack)
				continue
			}
			if i, exists := seen[pack.ID]; exists {
				if catalog.Packs[i].digest == pack.digest {
					continue
				}
				duplicate[pack.ID] = true
			} else {
				seen[pack.ID] = len(catalog.Packs)
			}
			catalog.Packs = append(catalog.Packs, pack)
		}
	}
	languages := catalog.languages[:0]
	for _, pack := range catalog.languages {
		if _, exists := seen[pack.ID]; exists {
			duplicate[pack.ID] = true
			pack.language = nil
			catalog.Packs = append(catalog.Packs, pack)
		} else {
			languages = append(languages, pack)
		}
	}
	catalog.languages = languages
	for i := range catalog.Packs {
		pack := &catalog.Packs[i]
		if duplicate[pack.ID] && pack.Error == "" {
			pack.Error = "More than one archive uses this mod ID. Keep one version in the mod directories."
		}
		pack.Enabled = pack.Error == "" && !s.settings.Disabled[pack.ID]
	}
	for _, title := range bundledTitles {
		occupied := func(pack installedMod) bool { return pack.ID == title.id }
		if slices.ContainsFunc(catalog.Packs, occupied) || slices.ContainsFunc(catalog.languages, occupied) {
			catalog.TitleBundleConflicts = append(catalog.TitleBundleConflicts, title.id)
			continue
		}
		path := s.bundledTitlePath(title.id)
		if _, err := os.Lstat(path); errors.Is(err, os.ErrNotExist) {
			continue
		}
		pack := s.inspectMod(path, false, index)
		if !s.ownsBundledTitle(pack) {
			catalog.TitleBundleConflicts = append(catalog.TitleBundleConflicts, title.id)
			continue
		}
		pack.Bundled = true
		pack.Enabled = s.settings.BundledTitles[pack.ID]
		catalog.Packs = append(catalog.Packs, pack)
	}
	s.indexChanged = s.indexChanged || !maps.Equal(s.index, index)
	s.index = index
	err := s.resolveLanguages(&catalog)
	return catalog, err
}

func (s *modStore) inspectMod(path string, external bool, index map[string]*modIndexEntry) installedMod {
	pack := installedMod{Filename: filepath.Base(path), External: external, path: path}
	prepared, err := s.prepareArchive(path, s.index[path])
	if prepared != nil {
		pack.modMetadata, pack.modPayload, pack.digest = prepared.modMetadata, prepared.modPayload, prepared.Digest
		pack.stamp = prepared.Source
		pack.language, pack.manualBook = prepared.Language, prepared.ManualBook
	}
	if err != nil {
		pack.Error = err.Error()
	} else if index != nil && prepared.Source.usable() && len(index) < maxDiscoveredMods {
		index[path] = prepared
	}
	return pack
}

func (s *modStore) prepareArchive(path string, previous *modIndexEntry) (*modIndexEntry, error) {
	f, err := openModFile(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	stat, err := f.Stat()
	if err != nil {
		return nil, err
	}
	stamp := readModFileStamp(f, stat)
	if previous.matches(s.cache, stamp) {
		return previous, nil
	}
	pack, err := readModArchive(f, stat.Size())
	if err != nil {
		return &modIndexEntry{modMetadata: pack.modMetadata, modPayload: pack.modPayload, Source: stamp}, err
	}
	hash := sha256.New()
	if n, err := io.Copy(hash, io.NewSectionReader(f, 0, stat.Size())); err != nil ||
		n != stat.Size() || !modArchiveUnchanged(f, stamp) {
		return nil, fmt.Errorf("mod archive changed while reading")
	}
	paths := pack.assetPaths()
	prepared := &modIndexEntry{
		modMetadata: pack.modMetadata, modPayload: pack.modPayload,
		Source: stamp, Digest: hex.EncodeToString(hash.Sum(nil)), AssetSizes: make([]uint64, len(paths)),
	}
	if pack.Type == languageModType {
		prepared.Language, err = lk.PrepareLanguageMod(languagePacksRoot(s.assets), prepared.Digest, f, stat.Size())
		if err != nil {
			return prepared, err
		}
	}
	for i, name := range paths {
		size := pack.files[name].UncompressedSize64
		prepared.AssetSizes[i] = size
		dest := filepath.Join(s.cache, pack.cacheFilename(prepared.Digest, i))
		info, err := os.Lstat(dest)
		if err == nil && matchesModAsset(info, size) {
			if pack.Type == manualModType {
				prepared.ManualBook, err = pack.copyAsset(io.Discard, name)
				if err != nil {
					return prepared, fmt.Errorf("%s: %w", name, err)
				}
			}
			continue
		}
		if err == nil && !info.Mode().IsRegular() {
			return nil, fmt.Errorf("invalid mod cache entry")
		}
		album, err := s.cacheAsset(dest, pack, name)
		if err != nil {
			return prepared, fmt.Errorf("%s: %w", name, err)
		}
		prepared.ManualBook = album
	}
	if !modArchiveUnchanged(f, stamp) {
		return nil, fmt.Errorf("mod archive changed while reading")
	}
	return prepared, nil
}

func modArchiveUnchanged(file *os.File, stamp modFileStamp) bool {
	info, err := file.Stat()
	return err == nil && readModFileStamp(file, info) == stamp
}

func (payload modPayload) cacheFilename(digest string, index int) string {
	extension := ".ogg"
	switch payload.Type {
	case titleModType:
		extension = ".png"
	case manualModType:
		extension = ".pdf"
	}
	return fmt.Sprintf("%s-%d%s", digest, index, extension)
}

func (s *modStore) cacheAsset(dest string, pack modArchive, name string) (manualAlbumVerdict, error) {
	tmp, err := os.CreateTemp(s.cache, ".asset-stage-*")
	if err != nil {
		return manualAlbumVerdict{}, err
	}
	defer os.Remove(tmp.Name())
	album, err := pack.copyAsset(tmp, name)
	if err == nil {
		err = tmp.Sync()
	}
	closeErr := tmp.Close()
	if err != nil {
		return album, err
	}
	if closeErr != nil {
		return album, closeErr
	}
	return album, os.Rename(tmp.Name(), dest)
}

func (pack modArchive) copyAsset(out io.Writer, name string) (manualAlbumVerdict, error) {
	source, err := pack.files[name].Open()
	if err != nil {
		return manualAlbumVerdict{}, err
	}
	defer source.Close()
	switch pack.Type {
	case titleModType:
		return manualAlbumVerdict{}, copyModTitle(out, source)
	case manualModType:
		return copyModManual(out, source)
	default:
		return manualAlbumVerdict{}, copyModAudio(out, source)
	}
}

func (s *modStore) render(catalog *modCatalog) ([]byte, error) {
	type candidate struct {
		pack  *installedMod
		index int
	}
	targets := map[string][]candidate{}
	for i := range catalog.Packs {
		pack := &catalog.Packs[i]
		if pack.Enabled {
			for index, target := range pack.targets() {
				targets[target] = append(targets[target], candidate{pack, index})
			}
		}
	}
	var sections strings.Builder
	catalog.Slots = []modSlot{}
	for _, key := range slices.Sorted(maps.Keys(targets)) {
		options := targets[key]
		spec := musicModTargets[key]
		slot := modSlot{ID: key, Type: options[0].pack.Type, Track: spec.Song.ID, Region: spec.Region}
		for _, option := range options {
			slot.Candidates = append(slot.Candidates, option.pack.ID)
		}
		winner := -1
		if len(options) == 1 {
			winner = 0
		} else {
			for _, preferred := range s.settings.Preferred[key] {
				winner = slices.Index(slot.Candidates, preferred)
				if winner >= 0 {
					break
				}
			}
			if winner < 0 {
				winner = slices.Index(slot.Candidates, s.settings.Automatic[key])
			}
		}
		if winner >= 0 {
			option := options[winner]
			slot.Winner = option.pack.ID
			if len(options) == 1 && len(s.settings.Preferred[key]) == 0 && s.settings.Automatic[key] == "" {
				if s.settings.Automatic == nil {
					s.settings.Automatic = map[string]string{}
				}
				s.settings.Automatic[key] = slot.Winner
			}
			file := "mods-cache/" + option.pack.cacheFilename(option.pack.digest, option.index)
			switch slot.Type {
			case titleModType:
				writeModTitleSections(&sections, file)
			case manualModType:
				book := option.pack.manualBook
				fmt.Fprintf(&sections, "\n[manual]\nfile = %s\nalbum = %d,%d,%d\n", file, book.Pages, book.Width, book.Height)
			default:
				writeModMusicSection(&sections, key, spec, option.pack.Tracks[option.index], file)
			}
		}
		catalog.Slots = append(catalog.Slots, slot)
	}
	header, err := s.settings.Header()
	if err != nil {
		return nil, err
	}
	return append(header, sections.String()...), nil
}

func (s *modStore) remember(slot, id string) {
	previous := s.settings.Preferred[slot]
	if len(previous) != 0 && previous[0] == id {
		return
	}
	previous = slices.DeleteFunc(previous, func(other string) bool { return other == id })
	s.settings.Preferred[slot] = append([]string{id}, previous[:min(len(previous), maxDiscoveredMods-1)]...)
}

func writeModMusicSection(out *strings.Builder, key string, spec musicModTarget, track modTrack, file string) {
	fmt.Fprintf(out, "\n[music:%s]\nsrc = %s\nfile = %s\n", key, spec.Song.Src, file)
	if spec.Gate != "" {
		fmt.Fprintf(out, "when = %s\n", spec.Gate)
	}
	loop := track.Loop
	if loop == nil {
		loop = spec.Song.Loop
	}
	if loop != nil {
		value := 0
		if *loop {
			value = 1
		}
		fmt.Fprintf(out, "loop = %d\n", value)
	}
	if track.LoopStart != nil {
		fmt.Fprintf(out, "loop_start = %d\n", *track.LoopStart)
	}
	if track.LoopEnd != nil {
		fmt.Fprintf(out, "loop_end = %d\n", *track.LoopEnd)
	}
	if track.Gain != nil {
		fmt.Fprintf(out, "gain = %d\n", *track.Gain)
	}
}

func writeModTitleSections(out *strings.Builder, file string) {
	logo, swirl := titleManifestValues(file)
	for _, entry := range []struct {
		name   string
		values []manifestValue
	}{
		{name: "title-logo", values: logo},
		{name: "title-swirl", values: swirl},
	} {
		fmt.Fprintf(out, "\n[replace:%s]\n", entry.name)
		for _, value := range entry.values {
			fmt.Fprintf(out, "%s = %s\n", value.Key, value.Value)
		}
	}
}

func (s *modStore) publish(catalog *modCatalog) error {
	data, err := s.render(catalog)
	if err != nil {
		return err
	}
	overlay := filepath.Join(s.assets, modOverlayName)
	current, absent, err := readBoundedRegularFile(overlay, maxAssetManifestBytes)
	if err != nil {
		return err
	}
	if bytes.Equal(current, data) {
		return nil
	}
	if absent && !s.settings.HasChoices() && len(s.settings.Directories)+len(s.settings.RelativeDirectories) == 0 {
		return nil
	}
	return writeAtomicFile(overlay, data)
}

func (s *modStore) change(catalog *modCatalog, request modChange) error {
	if handled, err := s.changeLanguage(catalog, request); handled {
		return err
	}
	if request.LanguageOnly {
		return fmt.Errorf("language pack is no longer available")
	}
	index := slices.IndexFunc(catalog.Packs, func(pack installedMod) bool {
		return pack.ID == request.Pack && pack.Error == ""
	})
	if index < 0 {
		return fmt.Errorf("no usable mod with ID %q", request.Pack)
	}
	pack := &catalog.Packs[index]
	switch request.Action {
	case "enable":
		if pack.Bundled {
			// The included artwork is a choice between two designs. Other
			// title packs can still compete for the slot through normal priority.
			for i := range catalog.Packs {
				other := &catalog.Packs[i]
				if other.Bundled && other.ID != pack.ID {
					s.setBundledTitleEnabled(other, false)
				}
			}
			s.setBundledTitleEnabled(pack, true)
		} else {
			delete(s.settings.Disabled, pack.ID)
			pack.Enabled = true
		}
	case "disable":
		if pack.Bundled {
			s.setBundledTitleEnabled(pack, false)
		} else {
			s.settings.Disabled[pack.ID] = true
			pack.Enabled = false
		}
	case "select":
		if !pack.Enabled || !slices.Contains(pack.targets(), request.Slot) {
			return fmt.Errorf("enable the mod and choose a slot it supplies")
		}
		s.remember(request.Slot, pack.ID)
	default:
		return fmt.Errorf("unknown mod action %q", request.Action)
	}
	return nil
}

func (s *modStore) setBundledTitleEnabled(pack *installedMod, enabled bool) {
	if enabled {
		if s.settings.BundledTitles == nil {
			s.settings.BundledTitles = map[string]bool{}
		}
		s.settings.BundledTitles[pack.ID] = true
	} else {
		delete(s.settings.BundledTitles, pack.ID)
	}
	pack.Enabled = enabled
}

func (s *modStore) install(source io.Reader, catalog *modCatalog, expectedType string, projectInstall *languageProjectInstall) error {
	tmp, err := os.CreateTemp(s.directory, ".mod-stage-*")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	size, writeErr := io.Copy(tmp, io.LimitReader(source, maxModBytes+1))
	if writeErr == nil {
		writeErr = tmp.Sync()
	}
	closeErr := tmp.Close()
	if writeErr != nil {
		return writeErr
	}
	if closeErr != nil {
		return closeErr
	}
	if size > maxModBytes {
		return fmt.Errorf("mod archive exceeds %d MiB", maxModBytes>>20)
	}
	pack, err := s.prepareArchive(tmp.Name(), nil)
	if err != nil {
		return err
	}
	if expectedType != "" && pack.Type != expectedType {
		switch expectedType {
		case manualModType:
			return fmt.Errorf("select an .o2r manual pack")
		case languageModType:
			return fmt.Errorf("select an .o2r language pack")
		default:
			return fmt.Errorf("select an .o2r pack of type %s", expectedType)
		}
	}
	if pack.Type == languageModType {
		if err := s.installLanguage(tmp.Name(), pack, catalog, projectInstall); err != nil {
			return err
		}
		catalog.InstalledType, catalog.InstalledID, catalog.InstalledName = pack.Type, lk.LanguageModID(pack.Language.ID), pack.Name
		return nil
	}
	catalog.InstalledType, catalog.InstalledID, catalog.InstalledName = pack.Type, pack.ID, pack.Name
	if slices.ContainsFunc(catalog.languages, func(language installedMod) bool { return language.ID == pack.ID }) {
		return fmt.Errorf("mod ID %q is already used by a language pack", pack.ID)
	}
	for _, existing := range catalog.Packs {
		if existing.ID == pack.ID {
			if existing.digest == pack.Digest && existing.Error == "" {
				return s.publish(catalog)
			}
			return fmt.Errorf("mod %q is already installed with different content", pack.ID)
		}
	}
	if len(catalog.Packs)+len(catalog.languages) >= maxDiscoveredMods {
		return fmt.Errorf("too many installed mods")
	}
	filename := pack.ID + "-" + pack.Digest[:12] + ".o2r"
	dest := filepath.Join(s.directory, filename)
	if _, err := os.Lstat(dest); !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("cannot publish mod to %s", filename)
	}
	if err := os.Rename(tmp.Name(), dest); err != nil {
		return err
	}
	catalog.Packs = append(catalog.Packs, installedMod{
		modMetadata: pack.modMetadata, modPayload: pack.modPayload, Filename: filename, Enabled: !s.settings.Disabled[pack.ID], digest: pack.Digest,
		manualBook: pack.ManualBook, path: dest,
	})
	if err := s.publish(catalog); err != nil {
		catalog.Packs = catalog.Packs[:len(catalog.Packs)-1]
		return errors.Join(err, os.Remove(dest))
	}
	s.rememberInstalledArchive(dest, pack)
	catalog.Packs[len(catalog.Packs)-1].stamp = pack.Source
	return nil
}

func localModUninstallRevision(pack *installedMod) string {
	if pack.External || pack.Bundled || pack.path == "" || !pack.stamp.usable() || !localModFilename(pack.Filename) {
		return ""
	}
	// Include the current file identity even if parsing failed or the file
	// was replaced with a byte-for-byte copy after the user reviewed it.
	stamp, _ := json.Marshal(pack.stamp)
	digest := sha256.Sum256(stamp)
	return hex.EncodeToString(digest[:])
}

func (s *modStore) uninstall(catalog *modCatalog, request modChange) error {
	path := filepath.Join(s.directory, request.Filename)
	var selected *installedMod
	for _, list := range [][]installedMod{catalog.Packs, catalog.languages} {
		for i := range list {
			pack := &list[i]
			if pack.path == path && localModUninstallRevision(pack) == request.Revision {
				selected = pack
				break
			}
		}
	}
	if request.Revision == "" || !localModFilename(request.Filename) || selected == nil {
		return fmt.Errorf("mod changed or is managed outside the local mod library; refresh the list")
	}
	file, err := openModFile(path)
	if err != nil {
		return err
	}
	unchanged := modArchiveUnchanged(file, selected.stamp)
	file.Close()
	if !unchanged {
		return fmt.Errorf("mod archive changed; refresh the list before uninstalling")
	}
	backupRoot := filepath.Join(s.directory, ".uninstalled")
	if err := ensureModDir(backupRoot); err != nil {
		return err
	}
	backup, err := os.MkdirTemp(backupRoot, "archive-")
	if err != nil {
		return err
	}
	defer os.Remove(backup) // only removes the directory if empty
	saved := filepath.Join(backup, request.Filename)
	if err := os.Rename(path, saved); err != nil {
		return err
	}
	next, err := s.scan()
	if err == nil {
		for group, digest := range s.settings.LanguageProjects {
			if digest == selected.digest {
				delete(s.settings.LanguageProjects, group)
			}
		}
		err = s.publish(&next)
	}
	if err != nil {
		return errors.Join(err, os.Rename(saved, path))
	}
	*catalog = next
	return nil
}

func updateModCatalog(root string, directories []string, source io.Reader, change *modChange) (modCatalog, error) {
	store, unlock, err := openModStore(root, directories)
	if err != nil {
		return modCatalog{}, err
	}
	defer unlock()
	defer store.saveIndex()
	catalog, err := store.scan()
	if err != nil {
		return catalog, err
	}
	if source != nil {
		expected := ""
		if change != nil {
			expected = change.InstallType
		}
		err = store.install(source, &catalog, expected, nil)
	} else if change != nil && change.Action == "uninstall" {
		err = store.uninstall(&catalog, *change)
	} else {
		if change != nil && change.Action != "" {
			if err := store.change(&catalog, *change); err != nil {
				return catalog, err
			}
		}
		err = store.publish(&catalog)
	}
	if err == nil {
		catalog.listLanguageMods()
	}
	return catalog, err
}
