package builder

import (
	"fmt"
	"net/http"
	"path/filepath"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

type localizationProjectRequest struct {
	ProjectID     string          `json:"projectID"`
	Revision      string          `json:"revision"`
	ID            string          `json:"id"`
	NewID         string          `json:"newID"`
	Metadata      lk.PackMetadata `json:"metadata"`
	Directory     string          `json:"directory"`
	Replace       bool            `json:"replace"`
	Expected      string          `json:"expected"`
	PreviewImport bool            `json:"previewImport"`
	ImportToken   string          `json:"importToken"`
}

func (work *localizationWork) manageLocalizationProject(w *localizationReply, r *http.Request, endpoint string, q localizationProjectRequest) error {
	if endpoint == "accept-import" {
		if work.pendingImport == nil || q.ImportToken != work.pendingImport.token {
			return fmt.Errorf("%w: import preview expired; select the pack again", lk.ErrProjectConflict)
		}
		if err := work.acceptLocalizationImport(work.pendingImport.project, q.NewID, q.Replace, q.Expected); err != nil {
			return err
		}
		work.pendingImport = nil
		w.json(200, work.localizationState())
		return nil
	}
	if endpoint == "open" || endpoint == "reference" {
		if endpoint == "reference" {
			if err := work.checkLocalizationIdentity(q.ProjectID, q.Revision); err != nil {
				return err
			}
			if q.ID == "" {
				work.reference = nil // Automatic Native US fallback.
				w.json(200, work.localizationState())
				return nil
			}
		}
		p, err := work.store.Open(q.ID)
		if err != nil {
			return err
		}
		if endpoint == "open" {
			work.current = p
		} else {
			work.reference = p
		}
		w.json(200, work.localizationState())
		return nil
	}
	if endpoint == "directory" {
		if !filepath.IsAbs(q.Directory) {
			return fmt.Errorf("choose an absolute pack directory")
		}
		p, err := lk.OpenAuthorProjectDirectory(q.Directory)
		if err != nil {
			return err
		}
		if q.PreviewImport {
			return work.previewLocalizationImport(w, p)
		}
		if err = work.acceptLocalizationImport(p, q.NewID, q.Replace, q.Expected); err != nil {
			return err
		}
		w.json(200, work.localizationState())
		return nil
	}
	if endpoint == "create" {
		var source *lk.AuthorPack
		if work.current != nil && work.current.Origin() == "native-source" && work.current.Pack().Manifest().Metadata().SourceProfile == "us" {
			source = work.current.Pack()
		} else {
			var err error
			source, err = lk.OpenAuthorPack(filepath.Join(work.root, "native-us"))
			if err != nil {
				return fmt.Errorf("extract the US source or build the game first: %w", err)
			}
		}
		m := q.Metadata
		m.SourceProfile, m.Target, m.Coverage, m.Fallback = "us", "us-runtime", "partial", "native-us"
		p, err := lk.NewTranslationProject(source, m)
		if err != nil {
			return err
		}
		if err = work.saveLocalization(p, ""); err != nil {
			return err
		}
		work.reference, _ = lk.NewSourceProject(source)
		w.json(200, work.localizationState())
		return nil
	}
	if err := work.checkLocalizationIdentity(q.ProjectID, q.Revision); err != nil {
		return err
	}
	if err := r.Context().Err(); err != nil {
		return err
	}
	p := work.current
	var next *lk.AuthorProject
	var err error
	switch endpoint {
	case "upgrade-v2":
		next, report, err := p.UpgradeV2(q.NewID)
		if err != nil {
			return err
		}
		if err = work.saveLocalization(next, ""); err != nil {
			return err
		}
		w.json(200, map[string]any{"state": work.localizationState(), "report": report})
		return nil
	case "clone":
		if p.Origin() == "native-source" {
			return fmt.Errorf("create a translation from the US source instead of cloning a read-only source")
		}
		m := p.Pack().Manifest().Metadata()
		m.ID = q.NewID
		if q.Metadata.Name != "" {
			m.Name = q.Metadata.Name
		}
		next, err = p.WithMetadata(m)
		if err != nil {
			return err
		}
		if err = work.saveLocalization(next, ""); err != nil {
			return err
		}
		w.json(200, work.localizationState())
		return nil
	}
	return fmt.Errorf("unknown project action")
}
