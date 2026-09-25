package builder

import (
	"context"
	"path/filepath"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
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
	root := filepath.Join(work.root, "packs")
	installed, err := lk.FindInstalledPack(root, p.Pack().Manifest().Metadata().ID)
	if err == nil && installed != nil {
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
