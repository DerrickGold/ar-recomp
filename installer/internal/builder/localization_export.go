package builder

import (
	"bytes"
	"net/http"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

type localizationExportRequest struct {
	ProjectID       string `json:"projectID"`
	Revision        string `json:"revision"`
	ConfirmRights   bool   `json:"confirmRights"`
	IncludeWIP      bool   `json:"includeWIP"`
	PrepareDownload bool   `json:"prepareDownload"`
}

func (work *localizationWork) exportLocalization(w *localizationReply, r *http.Request, endpoint string, q localizationExportRequest) error {
	if err := work.checkLocalizationIdentity(q.ProjectID, q.Revision); err != nil {
		return err
	}
	if err := r.Context().Err(); err != nil {
		return err
	}
	p := work.current
	var err error
	kind, extension := "backup", ".arproject"
	var report lk.PublicationReport
	if endpoint != "backup" {
		p, report, err = p.Publication(lk.PublicationOptions{ConfirmRights: q.ConfirmRights, IncludeWIP: q.IncludeWIP})
		if err != nil {
			return err
		}
		if err := work.requireFontCoverage(r.Context(), p); err != nil {
			return err
		}
		kind, extension = "publication", ".arlang"
	}
	if endpoint == "publication-check" {
		w.json(200, report)
		return nil
	}
	var archive bytes.Buffer
	if err := p.WriteArchive(&archive, kind); err != nil {
		return err
	}
	if q.PrepareDownload {
		token, err := randomToken()
		if err != nil {
			return err
		}
		work.export = &localizationExport{token: token, name: p.Pack().Manifest().Metadata().ID + extension, data: archive.Bytes()}
		w.json(200, map[string]string{"url": "localization/download/" + token, "name": work.export.name})
		return nil
	}
	w.archive = &localizationExport{name: p.Pack().Manifest().Metadata().ID + extension, data: archive.Bytes()}
	return nil
}
