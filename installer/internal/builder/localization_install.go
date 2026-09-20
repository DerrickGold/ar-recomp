package builder

import (
	"fmt"
	"net/http"
	"path/filepath"

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
	ConfirmUninstall bool   `json:"confirmUninstall"`
}

func (work *localizationWork) manageLocalizationInstallation(w *localizationReply, r *http.Request, endpoint string, q localizationInstallRequest) error {
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
		path, err := lk.InstallProjectInLibrary(filepath.Join(work.root, "packs"), prepared, q.Replace)
		if err != nil {
			return err
		}
		w.json(200, map[string]any{"path": path, "report": report, "enabled": filepath.Base(path) == "pack.ini"})
		return nil
	}
	return fmt.Errorf("unknown installation action")
}
