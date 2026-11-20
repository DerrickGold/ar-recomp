package builder

import (
	"context"
	"path/filepath"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"github.com/DerrickGold/ar-recomp/installer/internal/modconfig"
)

type localizationInstallationUpdate struct {
	Installed   bool   `json:"installed"`
	Updated     bool   `json:"updated"`
	Enabled     bool   `json:"enabled"`
	Error       string `json:"error,omitempty"`
	ErrorCode   string `json:"errorCode,omitempty"`
	RecoveryKey string `json:"recoveryKey,omitempty"`
}

// The author archive is already saved. An installation failure must report that
// distinction, so the editor can adopt the saved revision without losing work or
// claiming that the game copy was refreshed.
func (work *localizationWork) refreshSavedLocalization(ctx context.Context, p *lk.AuthorProject) (result localizationInstallationUpdate) {
	if result, handled := work.updateSavedLanguageMod(ctx, p); handled {
		return result
	}
	root := filepath.Join(work.root, "packs")
	installed, err := lk.FindInstalledPack(root, p.Pack().Manifest().Metadata().ID)
	if err == nil && installed != nil && !installed.Archive {
		result.Installed, result.Enabled = true, installed.Enabled
		var prepared *lk.AuthorProject
		prepared, _, err = p.Installation()
		if err == nil {
			err = work.requireFontCoverage(ctx, prepared)
		}
		if err == nil {
			_, err = lk.UpdateInstalledProject(root, prepared, *installed)
		}
		if err == nil {
			result.Updated = true
		}
	}
	if err != nil {
		failure, _ := describeLocalizationError(err)
		result.Error, result.ErrorCode, result.RecoveryKey = failure.Error, failure.ErrorCode, failure.RecoveryKey
	}
	return result
}

// Private installs follow project saves only while the installed archive still
// matches the last copy written from this project.
func (work *localizationWork) updateSavedLanguageMod(ctx context.Context, p *lk.AuthorProject) (localizationInstallationUpdate, bool) {
	var update localizationInstallationUpdate
	if work.modRoot == "" {
		return update, false
	}
	state, err := modconfig.Read(filepath.Join(work.modRoot, "game-assets", modOverlayName))
	group := lk.LanguageModID(p.Pack().Manifest().Metadata().ID)
	if err != nil || state.LanguageProjects[group] == "" {
		return update, false
	}
	store, unlock, err := openModStore(work.modRoot, work.modDirs)
	if err == nil {
		defer unlock()
		defer store.saveIndex()
		var result languageProjectInstall
		result, err = store.installLanguageProject(p, false, false, func(project *lk.AuthorProject) (*lk.AuthorProject, error) {
			prepared, _, err := project.Installation()
			if err == nil {
				err = work.requireFontCoverage(ctx, prepared)
			}
			return prepared, err
		})
		if err == nil && result.path == "" {
			return update, false
		}
		update.Enabled = result.enabled
	}
	update.Installed = true
	update.Updated = err == nil
	if err != nil {
		failure, _ := describeLocalizationError(err)
		update.Error, update.ErrorCode, update.RecoveryKey = failure.Error, failure.ErrorCode, failure.RecoveryKey
	}
	return update, true
}
