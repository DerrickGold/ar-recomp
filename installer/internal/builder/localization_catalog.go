package builder

import (
	"path/filepath"
	"slices"
	"strings"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

type localizationCatalogEntry struct {
	ID                string              `json:"id"`
	Name              string              `json:"name"`
	Locale            string              `json:"locale"`
	Project           bool                `json:"project"`
	Installed         bool                `json:"installed"`
	Enabled           bool                `json:"enabled"`
	InstalledName     string              `json:"installedName,omitempty"`
	Key               string              `json:"key,omitempty"`
	InstalledRevision string              `json:"installedRevision,omitempty"`
	Error             string              `json:"error,omitempty"`
	ReadOnly          bool                `json:"readOnly"`
	Required          bool                `json:"required,omitempty"`
	ModID             string              `json:"modId,omitempty"`
	Version           string              `json:"version,omitempty"`
	External          bool                `json:"external,omitempty"`
	Archive           bool                `json:"archive,omitempty"`
	UninstallRevision string              `json:"uninstallRevision,omitempty"`
	Filename          string              `json:"filename,omitempty"`
	ArchiveURL        string              `json:"archiveURL,omitempty"`
	Release           string              `json:"release,omitempty"`
	Choices           []languageModChoice `json:"choices,omitempty"`
	LocalAlternates   []languageModChoice `json:"localAlternates,omitempty"`
}

// installedLanguageCatalog projects the same resolved mod snapshot used by
// the general catalogue. Workshop project summaries are joined by the editor.
func (mods modCatalog) installedLanguageCatalog() []localizationCatalogEntry {
	rows := []localizationCatalogEntry{}
	modIDs := map[string]bool{}
	for _, pack := range mods.Packs {
		if pack.Type == languageModType && pack.language == nil && pack.Error != "" {
			rows = append(rows, localizationCatalogEntry{
				ID: pack.ID, Name: pack.Filename, InstalledName: pack.Filename, Key: pack.Filename,
				Installed: true, Archive: true, Error: pack.Error, External: pack.External,
				Filename: pack.Filename, UninstallRevision: localModUninstallRevision(&pack),
			})
		}
	}
	for _, group := range mods.languageGroups {
		row := localizationCatalogEntry{
			ID: group.ID, Name: group.Name, Locale: group.Locale, InstalledName: group.Name,
			Installed: true, Archive: true, Enabled: group.Enabled, ModID: group.ModID,
			Key: group.ModID, Release: group.Active, LocalAlternates: group.LocalAlternates,
		}
		for _, choice := range group.Choices {
			if choice.Key == group.Active {
				row.InstalledName, row.Version, row.External = choice.Name, choice.Version, choice.External
				row.ArchiveURL, row.Filename, row.UninstallRevision = choice.ArchiveURL, choice.Filename, choice.UninstallRevision
				if choice.pack != nil {
					row.ID, row.Locale = choice.pack.language.ID, choice.pack.language.Locale
				}
			}
		}
		if len(group.Choices) > 1 {
			row.Choices = group.Choices
		}
		modIDs[strings.ToLower(group.ID)] = true
		rows = append(rows, row)
	}
	for _, pack := range mods.legacyLanguages {
		m := pack.Metadata
		if modIDs[strings.ToLower(m.ID)] && pack.Error == "" {
			continue
		}
		row := localizationCatalogEntry{
			ID: m.ID, Name: m.Name, Locale: m.Locale, Installed: true, Enabled: pack.Enabled,
			InstalledName: m.Name, Key: pack.Key, InstalledRevision: pack.Revision, Error: pack.Error, Archive: pack.Archive,
		}
		if row.Name == "" {
			row.Name = pack.Key
		}
		rows = append(rows, row)
	}
	sortLanguageCatalog(rows)
	return rows
}

func (work *localizationWork) localizationCatalog() ([]localizationCatalogEntry, error) {
	projects, err := work.store.List()
	if err != nil {
		return nil, err
	}
	var mods modCatalog
	if hasModSources(work.modRoot, work.modDirs) {
		mods, err = updateModCatalog(work.modRoot, work.modDirs, nil, nil)
		if err != nil {
			return nil, err
		}
	}
	mods.readLegacyLanguages(filepath.Join(work.root, "packs"))
	if mods.legacyLanguageError != nil {
		return nil, mods.legacyLanguageError
	}

	rows := mods.installedLanguageCatalog()
	byID := make(map[string]lk.AuthorProjectSummary, len(projects))
	for _, project := range projects {
		byID[project.ID] = project
	}
	seen := map[string]bool{}
	for i := range rows {
		row := &rows[i]
		if row.Archive && row.ModID == "" && row.Filename != "" {
			continue // An unreadable O2R, which is not linked to a project.
		}
		if p, ok := byID[row.ID]; ok && p.Error == "" {
			row.Project, row.Name = true, p.Name
			if row.ModID == "" {
				row.Locale = p.Locale
				row.ReadOnly = p.Origin == "native-source"
			}
			seen[p.ID] = true
		}
	}
	for _, p := range projects {
		if !seen[p.ID] {
			rows = append(rows, localizationCatalogEntry{ID: p.ID, Name: p.Name, Locale: p.Locale, Project: p.Error == "", Error: p.Error, ReadOnly: p.Origin == "native-source"})
		}
	}
	sortLanguageCatalog(rows)
	return rows, nil
}

func sortLanguageCatalog(rows []localizationCatalogEntry) {
	slices.SortFunc(rows, func(a, b localizationCatalogEntry) int {
		for _, pair := range [][2]string{{a.Name, b.Name}, {a.ID, b.ID}, {a.Key, b.Key}} {
			if n := strings.Compare(strings.ToLower(pair[0]), strings.ToLower(pair[1])); n != 0 {
				return n
			}
		}
		return 0
	})
}
