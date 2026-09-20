package builder

import (
	"fmt"
	"net/http"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"github.com/DerrickGold/ar-recomp/installer/internal/textpreview"
)

type localizationDraftRequest struct {
	ProjectID      string               `json:"projectID"`
	Revision       string               `json:"revision"`
	ID             string               `json:"id"`
	Body           string               `json:"body"`
	Status         lk.TranslationStatus `json:"status"`
	Metadata       lk.PackMetadata      `json:"metadata"`
	Notes          string               `json:"notes"`
	NoticeName     string               `json:"noticeName"`
	NoticeText     string               `json:"noticeText"`
	SaveMessage    bool                 `json:"saveMessage"`
	SaveDetails    bool                 `json:"saveDetails"`
	SaveNotice     bool                 `json:"saveNotice"`
	SaveFonts      bool                 `json:"saveFonts"`
	Fonts          lk.PackFonts         `json:"fonts"`
	FontPaths      []string             `json:"fontPaths"`
	fontUploads    map[string][]byte
	Scenario       *textpreview.Scenario `json:"scenario"`
	SourceScenario *textpreview.Scenario `json:"sourceScenario"`
}

func (work *localizationWork) editLocalization(w *localizationReply, r *http.Request, endpoint string, q localizationDraftRequest) error {
	if err := work.checkLocalizationIdentity(q.ProjectID, q.Revision); err != nil {
		return err
	}
	if err := r.Context().Err(); err != nil {
		return err
	}
	p := work.current
	if endpoint == "playback" {
		result, err := work.playback(r.Context(), q)
		if err == nil {
			w.json(200, result)
		}
		return err
	}
	var next *lk.AuthorProject
	var err error
	switch endpoint {
	case "save":
		if p.Origin() == "native-source" {
			return fmt.Errorf("native sources are read-only; create a translation first")
		}
		// One immutable edit chain and one archive replacement: invalid text or
		// metadata must not leave a partially saved set of progress/details.
		if !q.SaveMessage && !q.SaveDetails && !q.SaveNotice && !q.SaveFonts {
			return fmt.Errorf("no changes to save")
		}
		next = p
		if q.SaveDetails {
			m := p.Pack().Manifest().Metadata()
			m.Name, m.Locale, m.Autonym, m.Author, m.License, m.Direction = q.Metadata.Name, q.Metadata.Locale, q.Metadata.Autonym, q.Metadata.Author, q.Metadata.License, q.Metadata.Direction
			next, err = next.WithMetadata(m)
			if err == nil {
				next, err = next.WithNotes(q.Notes)
			}
		}
		if err == nil && q.SaveNotice {
			next, err = next.WithNotice(q.NoticeName, q.NoticeText)
		}
		if err == nil {
			switch {
			case q.SaveMessage && q.SaveFonts:
				next, err = next.EditMessageAndFonts(q.ID, q.Body, q.Status, q.Fonts, q.fontUploads)
			case q.SaveMessage:
				next, err = next.EditMessage(q.ID, q.Body, q.Status)
			case q.SaveFonts:
				next, err = next.WithFonts(q.Fonts, q.fontUploads)
			}
		}
	case "edit":
		next, err = p.EditMessage(q.ID, q.Body, q.Status)
	case "preview":
		p, err = previewLocalizationDraft(p, q)
		if err != nil {
			return err
		}

		ops, err := p.Pack().MessageOperations(q.ID)
		if err != nil {
			return err
		}
		w.json(200, ops)
		return nil
	case "materialize":
		message, err := p.Pack().ResolvedMessage(q.ID)
		if err != nil {
			return err
		}
		script, err := lk.EmitAuthorScriptVersion([]lk.AuthorMessage{message}, "preview.artext", p.Pack().Manifest().Version(), p.Pack().Treatments()...)
		if err != nil {
			return err
		}
		body, _ := script.Body(q.ID)
		w.json(200, map[string]string{"body": body})
		return nil
	case "metadata":
		m := p.Pack().Manifest().Metadata()
		m.Name, m.Locale, m.Autonym, m.Author, m.License, m.Direction = q.Metadata.Name, q.Metadata.Locale, q.Metadata.Autonym, q.Metadata.Author, q.Metadata.License, q.Metadata.Direction
		next, err = p.WithMetadata(m)
		if err == nil {
			next, err = next.WithNotes(q.Notes)
		}
	case "notice":
		next, err = p.WithNotice(q.NoticeName, q.NoticeText)
	default:
		return fmt.Errorf("unknown localization action")
	}
	if err != nil {
		return err
	}
	if err = work.saveLocalization(next, p.ProjectRevision()); err != nil {
		return err
	}
	state := work.localizationState()
	if endpoint == "save" {
		state["installationUpdate"] = work.refreshSavedLocalization(r.Context(), next)
	}
	w.json(200, state)
	return nil
}
