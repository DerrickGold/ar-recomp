package builder

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"maps"
	"os"
	"path/filepath"
	"slices"
	"strings"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

type languageModChoice struct {
	Key               string `json:"key"`
	Name              string `json:"name"`
	Version           string `json:"version,omitempty"`
	Filename          string `json:"filename"`
	SourcePath        string `json:"sourcePath,omitempty"`
	External          bool   `json:"external,omitempty"`
	UninstallRevision string `json:"uninstallRevision,omitempty"`
	ArchiveURL        string `json:"archiveURL,omitempty"`
	pack              *installedMod
}

type languageModGroup struct {
	ID              string              `json:"id"`
	ModID           string              `json:"modId"`
	Name            string              `json:"name"`
	Locale          string              `json:"locale"`
	Enabled         bool                `json:"enabled"`
	Active          string              `json:"active,omitempty"`
	Choices         []languageModChoice `json:"choices"`
	LocalAlternates []languageModChoice `json:"-"`
}

func languagePacksRoot(assets string) string {
	return filepath.Join(assets, "languages", "packs")
}

func hasModSources(root string, directories []string) bool {
	if root == "" {
		return false
	}
	assets := filepath.Join(root, "game-assets")
	_, err := os.Lstat(filepath.Join(assets, "mods"))
	return len(directories) != 0 || err == nil || regularFileExists(filepath.Join(assets, modOverlayName))
}

func (catalog *modCatalog) readLegacyLanguages(packs string) {
	if catalog.legacyLanguages == nil && catalog.legacyLanguageError == nil {
		catalog.legacyLanguages, catalog.legacyLanguageError = lk.ListInstalledPacks(packs)
	}
}

func (s *modStore) resolveLanguages(catalog *modCatalog) error {
	catalog.languageGroups = nil
	if len(catalog.languages) == 0 {
		return nil
	}
	catalog.readLegacyLanguages(languagePacksRoot(s.assets))
	if catalog.legacyLanguageError != nil {
		return catalog.legacyLanguageError
	}
	byID := map[string][]*installedMod{}
	for i := range catalog.languages {
		pack := &catalog.languages[i]
		key := lk.LanguageModID(pack.language.ID)
		byID[key] = append(byID[key], pack)
	}
	for _, id := range slices.Sorted(maps.Keys(byID)) {
		packs := byID[id]
		slices.SortStableFunc(packs, func(a, b *installedMod) int {
			return lk.CompareLanguageReleases(b.Version, a.Version)
		})
		newest := packs[0]
		group := languageModGroup{ID: newest.language.ID, ModID: id, Name: newest.Name, Locale: newest.language.Locale}
		seen := map[string]bool{}
		versions := map[string]string{}
		for _, pack := range packs {
			choice := languageModChoice{
				Key: pack.digest, Name: pack.Name, Version: pack.Version,
				Filename: pack.Filename, External: pack.External, ArchiveURL: localModArchiveURL(pack),
				UninstallRevision: localModUninstallRevision(pack), pack: pack,
			}
			if version, exists := versions[pack.language.ID]; (exists && version != pack.Version) || seen[pack.language.Content] {
				if choice.UninstallRevision != "" {
					group.LocalAlternates = append(group.LocalAlternates, choice)
				}
				continue
			}
			versions[pack.language.ID] = pack.Version
			seen[pack.language.Content] = true
			group.Choices = append(group.Choices, choice)
		}
		var previous *lk.InstalledPackSummary
		legacyPresent := false
		for i := range catalog.legacyLanguages {
			row := &catalog.legacyLanguages[i]
			if !strings.EqualFold(row.Metadata.ID, group.ID) {
				continue
			}
			legacyPresent = true
			if row.Enabled && row.Error == "" {
				previous = row
			}
		}
		preferred := s.settings.LanguageChoice[id]
		if previous != nil && (!seen[previous.Content] || preferred == "legacy") {
			group.Choices = append(group.Choices, languageModChoice{Key: "legacy", Name: previous.Metadata.Name, Filename: previous.Key})
		}
		// Separate launcher folders can contain competing releases with the same basename.
		if len(group.Choices) > 1 {
			names := make(map[string]int, len(group.Choices))
			for _, choice := range group.Choices {
				names[choice.Filename]++
			}
			for i := range group.Choices {
				choice := &group.Choices[i]
				if choice.External && names[choice.Filename] > 1 {
					choice.SourcePath = choice.pack.path
				}
			}
		}
		disabled, explicit := s.settings.Disabled[id]
		group.Enabled = !disabled && (explicit || !legacyPresent || previous != nil)
		if i := slices.IndexFunc(group.Choices, func(choice languageModChoice) bool { return choice.Key == preferred }); i >= 0 {
			group.Active = group.Choices[i].Key
		} else if len(group.Choices) == 1 || (len(group.Choices) == 2 && preferred != "" &&
			preferred != "legacy" && group.Choices[1].Key == "legacy") {
			// Once the user chooses the O2R line over a legacy copy, a
			// newer unambiguous release continues that choice.
			group.Active = group.Choices[0].Key
		}
		catalog.languageGroups = append(catalog.languageGroups, group)
	}
	return nil
}

func (s *modStore) changeLanguage(catalog *modCatalog, request modChange) (bool, error) {
	i := slices.IndexFunc(catalog.languageGroups, func(group languageModGroup) bool {
		if group.ModID == request.Pack {
			return true
		}
		return slices.ContainsFunc(group.Choices, func(choice languageModChoice) bool {
			return choice.pack != nil && choice.pack.ID == request.Pack
		})
	})
	if i < 0 {
		return false, nil
	}
	group := catalog.languageGroups[i]
	switch request.Action {
	case "enable", "disable":
		s.settings.Disabled[group.ModID] = request.Action == "disable"
	case "select":
		if !slices.ContainsFunc(group.Choices, func(choice languageModChoice) bool { return choice.Key == request.Release }) {
			return true, fmt.Errorf("language releases changed; refresh the language library")
		}
		if s.settings.LanguageChoice == nil {
			s.settings.LanguageChoice = map[string]string{}
		}
		s.settings.LanguageChoice[group.ModID] = request.Release
	default:
		return true, fmt.Errorf("unknown mod action %q", request.Action)
	}
	return true, s.resolveLanguages(catalog)
}

func (catalog *modCatalog) listLanguageMods() {
	for _, group := range catalog.languageGroups {
		row := *group.Choices[0].pack
		for _, choice := range group.Choices {
			if choice.Key == group.Active && choice.pack != nil {
				row = *choice.pack
				row.Enabled = group.Enabled
			}
		}
		if group.Active == "" {
			row.Error = "Choose a release in the language library."
			slot := modSlot{ID: "language:" + group.ID, Type: languageModType}
			for _, choice := range group.Choices {
				slot.Candidates = append(slot.Candidates, choice.Key)
			}
			catalog.Slots = append(catalog.Slots, slot)
		}
		catalog.Packs = append(catalog.Packs, row)
	}
}

// Tracks the last archive written from an editable project. Copies installed
// from disk or supplied by the launcher never acquire project ownership.
type languageProjectInstall struct {
	review  bool
	path    string
	enabled bool
	catalog *modCatalog
}

func (s *modStore) installLanguageProject(project *lk.AuthorProject, review, replace bool, prepare func(*lk.AuthorProject) (*lk.AuthorProject, error)) (languageProjectInstall, error) {
	result := languageProjectInstall{review: review}
	catalog, err := s.scan()
	if err != nil {
		return result, err
	}
	runtimeID := project.Pack().Manifest().Metadata().ID
	groupID := lk.LanguageModID(runtimeID)
	owned := s.settings.LanguageProjects[groupID]
	var latest lk.LanguageRelease
	for _, group := range catalog.languageGroups {
		if group.ModID != groupID {
			continue
		}
		if review && !replace {
			return result, fmt.Errorf("%w: confirm replacing the installed language pack first", lk.ErrProjectConflict)
		}
		for _, choice := range group.Choices {
			if choice.Key == owned && choice.pack != nil && !choice.External {
				result.path = choice.pack.path
				result.enabled = group.Enabled
			}
		}
	}
	if !review && result.path == "" {
		return result, nil
	}
	for _, entry := range catalog.languages {
		if lk.LanguageModID(entry.language.ID) != groupID {
			continue
		}
		version, _ := lk.ParseLanguageRelease(entry.Version)
		if slices.Compare(version[:], latest[:]) > 0 {
			latest = version
		}
		// A shared copy may have replaced the project's canonical filename.
		// Project saves should never replace such an archive.
		if !review && entry.path == filepath.Join(s.directory, groupID+".o2r") && entry.digest != owned {
			return result, fmt.Errorf("the installed release changed; use Review & install to update the game copy")
		}
	}
	if latest[0] == 0 {
		latest = lk.LanguageRelease{1, 0, 0}
	} else if latest[2] < ^uint32(0) {
		latest[2]++
	} else if latest[1] < ^uint32(0) {
		latest[1]++
		latest[2] = 0
	} else {
		return result, fmt.Errorf("cannot increment the installed language release")
	}
	if prepare != nil {
		project, err = prepare(project)
		if err != nil {
			return result, err
		}
	}
	version := fmt.Sprintf("%d.%d.%d", latest[0], latest[1], latest[2])
	var archive bytes.Buffer
	if err := project.WriteInstalledLanguageMod(&archive, version); err != nil {
		return result, err
	}
	err = s.install(&archive, &catalog, languageModType, &result)
	if err == nil && review {
		catalog.listLanguageMods()
		result.catalog = &catalog
	}
	return result, err
}

func (s *modStore) installLanguage(staged string, pack *modIndexEntry, catalog *modCatalog, project *languageProjectInstall) error {
	if slices.ContainsFunc(catalog.Packs, func(other installedMod) bool { return other.ID == pack.ID }) {
		return fmt.Errorf("mod ID %q is already installed with incompatible content", pack.ID)
	}
	groupID := lk.LanguageModID(pack.Language.ID)
	primary := filepath.Join(s.directory, groupID+".o2r")
	dest := primary
	replace := false
	for _, existing := range catalog.languages {
		if lk.LanguageModID(existing.language.ID) != groupID {
			continue
		}
		if existing.language.ID != pack.Language.ID {
			return fmt.Errorf("a language update must keep the exact package ID %q", existing.language.ID)
		}
		if project != nil && existing.digest == s.settings.LanguageProjects[groupID] &&
			!existing.External && existing.language.Content == pack.Language.Content &&
			(!project.review || catalog.activeLanguageRelease(groupID) == existing.digest) {
			project.path, project.enabled = existing.path, catalog.languageEnabled(groupID)
			return s.publish(catalog)
		}
		cmp := lk.CompareLanguageReleases(pack.Version, existing.Version)
		switch {
		case cmp < 0:
			return fmt.Errorf("a newer release of %s is already installed", pack.Name)
		case cmp == 0 && pack.Language.Content == existing.language.Content:
			return s.publish(catalog)
		case cmp == 0:
			dest = filepath.Join(s.directory, groupID+"-"+pack.Digest[:12]+".o2r")
		}
		replace = replace || existing.path == primary
	}
	replace = replace && dest == primary
	if !replace && len(catalog.Packs)+len(catalog.languages) >= maxDiscoveredMods {
		return fmt.Errorf("too many installed mods")
	}
	if !replace {
		if _, err := os.Lstat(dest); !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("cannot publish mod to %s", filepath.Base(dest))
		}
	}
	// A same-directory backup keeps the previous release recoverable if
	// publishing the catalog fails after the atomic replacement.
	var backup string
	if replace {
		var err error
		backup, err = stageLanguageUpdateBackup(dest)
		if err != nil {
			return err
		}
		defer func() {
			if backup != "" {
				_ = os.Remove(backup)
			}
		}()
	}
	if err := os.Rename(staged, dest); err != nil {
		return err
	}
	s.rememberInstalledArchive(dest, pack)
	if project != nil {
		previous := s.settings.LanguageProjects[groupID]
		if s.settings.LanguageProjects == nil {
			s.settings.LanguageProjects = map[string]string{}
		}
		s.settings.LanguageProjects[groupID] = pack.Digest
		if project.review || (previous != "" && s.settings.LanguageChoice[groupID] == previous) {
			if s.settings.LanguageChoice == nil {
				s.settings.LanguageChoice = map[string]string{}
			}
			s.settings.LanguageChoice[groupID] = pack.Digest
		}
	}
	next, err := s.scan()
	if err == nil {
		err = s.publish(&next)
	}
	if err != nil {
		delete(s.index, dest)
		if !replace {
			return errors.Join(err, os.Remove(dest))
		}
		restore := os.Rename(backup, dest)
		if restore != nil {
			path := backup
			backup = ""
			return errors.Join(err, fmt.Errorf("previous release is saved at %s: %w", path, restore))
		}
		return err
	}
	if project != nil {
		project.path, project.enabled = dest, next.languageEnabled(groupID)
	}
	*catalog = next
	return nil
}

func (catalog *modCatalog) activeLanguageRelease(groupID string) string {
	for _, group := range catalog.languageGroups {
		if group.ModID == groupID {
			return group.Active
		}
	}
	return ""
}

func (catalog *modCatalog) languageEnabled(groupID string) bool {
	for _, group := range catalog.languageGroups {
		if group.ModID == groupID {
			return group.Enabled
		}
	}
	return false
}

func stageLanguageUpdateBackup(path string) (string, error) {
	previous, err := openModFile(path)
	if err != nil {
		return "", err
	}
	defer previous.Close()
	info, err := previous.Stat()
	if err != nil {
		return "", err
	}
	stamp := readModFileStamp(previous, info)
	backup, err := os.CreateTemp(filepath.Dir(path), ".language-update-*.backup")
	if err != nil {
		return "", err
	}
	size, copyErr := io.Copy(backup, io.NewSectionReader(previous, 0, info.Size()))
	if copyErr == nil && (size != info.Size() || !modArchiveUnchanged(previous, stamp)) {
		copyErr = fmt.Errorf("installed release changed while preparing its replacement")
	}
	if copyErr == nil {
		copyErr = backup.Sync()
	}
	if err := errors.Join(copyErr, backup.Close()); err != nil {
		_ = os.Remove(backup.Name())
		return "", err
	}
	return backup.Name(), nil
}

// PrepareRuntimeLanguages merges the shared mod scan with the legacy language
// catalog for both direct game startup and launches through the Workshop.
func PrepareRuntimeLanguages(ctx context.Context, root, packsRoot string, log io.Writer) error {
	plan := lk.LanguageModCatalog{}
	expected := languagePacksRoot(filepath.Join(root, "game-assets"))
	if packsRoot == "" {
		packsRoot = expected
	}
	absolute, err := filepath.Abs(packsRoot)
	if err != nil {
		return err
	}
	if absolute == expected && hasModSources(root, nil) {
		if err := prepareNativeUSMod(root); err != nil {
			fmt.Fprintf(log, "[localization] cannot prepare native US text: %v\n", err)
		}
		switch os.Getenv(ModRecoveryEnv) {
		case "prepared":
			cache := filepath.Join(packsRoot, ".arlang-cache", "catalog-v1.tsv")
			if data, _, err := readBoundedRegularFile(cache, 256<<10); err == nil && strings.HasPrefix(string(data), lk.ArchiveCatalogHeader) {
				fmt.Fprintln(log, "[localization] using the last prepared language catalog")
				return nil
			}
			fallthrough
		case "legacy":
			return lk.PrepareLanguageArchives(ctx, packsRoot, log)
		}
		catalog, err := updateModCatalog(root, nil, nil, nil)
		if err != nil {
			// A mod scan failure must not hide the legacy catalog.
			fmt.Fprintf(log, "[localization] cannot scan mods: %v\n", err)
		} else {
			for _, group := range catalog.languageGroups {
				if !group.Enabled || group.Active == "" {
					plan.Blocked = append(plan.Blocked, group.ID)
					continue
				}
				for _, choice := range group.Choices {
					if choice.Key == group.Active && choice.pack != nil {
						plan.Active = append(plan.Active, *choice.pack.language)
					}
				}
			}
		}
	}
	return lk.PrepareLanguageCatalog(ctx, packsRoot, log, plan)
}

// InstallLanguageMod exposes the shared transaction to the authoring CLI.
func InstallLanguageMod(root, path string) error {
	file, err := openModFile(path)
	if err != nil {
		return err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return err
	}
	pack, err := readModArchive(file, info.Size())
	if err != nil {
		return err
	}
	if pack.Type != languageModType {
		return fmt.Errorf("expected an .o2r language pack")
	}
	if err := os.MkdirAll(root, 0755); err != nil {
		return err
	}
	_, err = updateModCatalog(root, nil, file, &modChange{InstallType: languageModType})
	return err
}
