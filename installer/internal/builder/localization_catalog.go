package builder

import (
	"path/filepath"
	"slices"
	"strings"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

type localizationCatalogEntry struct {
	ID                string `json:"id"`
	Name              string `json:"name"`
	Locale            string `json:"locale"`
	Project           bool   `json:"project"`
	Installed         bool   `json:"installed"`
	Enabled           bool   `json:"enabled"`
	InstalledName     string `json:"installedName,omitempty"`
	Key               string `json:"key,omitempty"`
	InstalledRevision string `json:"installedRevision,omitempty"`
	Error             string `json:"error,omitempty"`
	ReadOnly          bool   `json:"readOnly"`
}

func (work *localizationWork) localizationCatalog() ([]localizationCatalogEntry, error) {
	projects, err := work.store.List()
	if err != nil {
		return nil, err
	}
	installed, err := lk.ListInstalledPacks(filepath.Join(work.root, "packs"))
	if err != nil {
		return nil, err
	}
	byID := make(map[string]lk.AuthorProjectSummary, len(projects))
	for _, p := range projects {
		byID[p.ID] = p
	}
	seen := map[string]bool{}
	rows := []localizationCatalogEntry{}
	for _, pack := range installed {
		m := pack.Metadata
		row := localizationCatalogEntry{ID: m.ID, Name: m.Name, Locale: m.Locale, Installed: true, Enabled: pack.Enabled, InstalledName: m.Name, Key: pack.Key, InstalledRevision: pack.Revision, Error: pack.Error}
		if row.Name == "" {
			row.Name = pack.Key
		}
		if p, ok := byID[m.ID]; ok && p.Error == "" {
			row.Project, row.Name, row.Locale = true, p.Name, p.Locale
			row.ReadOnly = p.Origin == "native-source"
			seen[m.ID] = true
		}
		rows = append(rows, row)
	}
	for _, p := range projects {
		if !seen[p.ID] {
			rows = append(rows, localizationCatalogEntry{ID: p.ID, Name: p.Name, Locale: p.Locale, Project: p.Error == "", Error: p.Error, ReadOnly: p.Origin == "native-source"})
		}
	}
	slices.SortFunc(rows, func(a, b localizationCatalogEntry) int {
		for _, pair := range [][2]string{{a.Name, b.Name}, {a.ID, b.ID}, {a.Key, b.Key}} {
			if n := strings.Compare(strings.ToLower(pair[0]), strings.ToLower(pair[1])); n != 0 {
				return n
			}
		}
		return 0
	})
	return rows, nil
}
