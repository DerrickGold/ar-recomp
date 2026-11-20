package builder

import (
	"fmt"
	"net/http"
	"path/filepath"
	"strings"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

type localizationInstallRequest struct {
	ProjectID        string `json:"projectID"`
	Revision         string `json:"revision"`
	ID               string `json:"id"`
	Directory        string `json:"directory"`
	Replace          bool   `json:"replace"`
	Expected         string `json:"expected"`
	Enabled          bool   `json:"enabled"`
	AsMod            bool   `json:"asMod"`
	ConfirmUninstall bool   `json:"confirmUninstall"`
	ModID            string `json:"modId"`
	Release          string `json:"release"`
}

func (work *localizationWork) installedLanguageMod(id string) (*languageModGroup, error) {
	if !hasModSources(work.modRoot, work.modDirs) {
		return nil, nil
	}
	catalog, err := updateModCatalog(work.modRoot, work.modDirs, nil, nil)
	if err != nil {
		return nil, err
	}
	for _, group := range catalog.languageGroups {
		if strings.EqualFold(group.ID, id) {
			return &group, nil
		}
	}
	return nil, nil
}

func (work *localizationWork) manageLocalizationInstallation(w *localizationReply, r *http.Request, endpoint string, q localizationInstallRequest) error {
	if endpoint == "select-language-release" || (endpoint == "set-enabled" && q.ModID != "") {
		if q.ID == "" || lk.LanguageModID(q.ID) != q.ModID || work.modRoot == "" {
			return fmt.Errorf("invalid language mod identity")
		}
		action := "select"
		if endpoint == "set-enabled" {
			action = "disable"
			if q.Enabled {
				action = "enable"
			}
		}
		_, err := updateModCatalog(work.modRoot, work.modDirs, nil, &modChange{
			Action: action, Pack: q.ModID, Release: q.Release, LanguageOnly: true,
		})
		if err != nil {
			return err
		}
		w.json(200, map[string]bool{"enabled": q.Enabled})
		return nil
	}
	if endpoint == "set-enabled" {
		if err := lk.SetLanguagePackEnabled(filepath.Join(work.root, "packs"), q.Directory, q.ID, q.Expected, q.Enabled); err != nil {
			return err
		}
		w.json(200, map[string]any{"enabled": q.Enabled, "message": "Package availability saved. Restart the game to refresh its language selector."})
		return nil
	}
	if endpoint == "uninstall" {
		if !q.ConfirmUninstall {
			return fmt.Errorf("confirm removal of this installed package first")
		}
		backup, err := lk.UninstallLanguagePack(filepath.Join(work.root, "packs"), q.Directory, q.ID, q.Expected)
		if err != nil {
			return err
		}
		w.json(200, map[string]string{"backup": backup, "message": "Uninstalled from game discovery. Restart the game. Your workshop project and installed files are retained for recovery."})
		return nil
	}
	if err := work.checkLocalizationIdentity(q.ProjectID, q.Revision); err != nil {
		return err
	}
	if err := r.Context().Err(); err != nil {
		return err
	}
	p := work.current
	switch endpoint {
	case "installation":
		if q.AsMod {
			group, err := work.installedLanguageMod(p.Pack().Manifest().Metadata().ID)
			if err != nil {
				return err
			}
			if group != nil {
				var path string
				for _, choice := range group.Choices {
					if choice.Key == group.Active && choice.pack != nil {
						path = choice.pack.path
					}
				}
				w.json(200, map[string]any{"installed": true, "enabled": group.Enabled, "path": path})
				return nil
			}
		}
		path := filepath.Join(work.root, "packs", p.Pack().Manifest().Metadata().ID, "pack.ini")
		installed, statErr := lk.FindInstalledPack(filepath.Join(work.root, "packs"), p.Pack().Manifest().Metadata().ID)
		if statErr != nil {
			return statErr
		}
		enabled := installed != nil && installed.Enabled
		if installed != nil && installed.Archive {
			path = filepath.Join(work.root, "packs", installed.Key)
		}
		w.json(200, map[string]any{"installed": installed != nil, "enabled": enabled, "path": path})
		return nil
	case "install", "installation-check":
		prepared, report, err := p.Installation()
		if err != nil {
			return err
		}
		if err := work.requireFontCoverage(r.Context(), prepared); err != nil {
			return err
		}
		if endpoint == "installation-check" {
			w.json(200, report)
			return nil
		}
		if q.AsMod {
			if work.modRoot == "" {
				return fmt.Errorf("choose a game folder before installing language packs")
			}
			legacy, err := lk.FindInstalledPack(filepath.Join(work.root, "packs"), p.Pack().Manifest().Metadata().ID)
			if err != nil {
				return err
			}
			if legacy != nil && !q.Replace {
				return fmt.Errorf("%w: confirm replacing the installed language pack first", lk.ErrProjectConflict)
			}
			store, unlock, err := openModStore(work.modRoot, work.modDirs)
			if err != nil {
				return err
			}
			defer unlock()
			defer store.saveIndex()
			result, err := store.installLanguageProject(prepared, true, q.Replace, nil)
			if err != nil {
				return err
			}
			w.json(200, map[string]any{
				"path": result.path, "report": report, "enabled": result.enabled,
				"installedType": languageModType, "catalog": result.catalog.view(work.modRoot),
			})
			return nil
		}
		path, err := lk.InstallProjectInLibrary(filepath.Join(work.root, "packs"), prepared, q.Replace)
		if err != nil {
			return err
		}
		w.json(200, map[string]any{"path": path, "report": report, "enabled": filepath.Base(path) == "pack.ini"})
		return nil
	}
	return fmt.Errorf("unknown installation action")
}
