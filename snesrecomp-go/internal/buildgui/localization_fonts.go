package buildgui

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"

	lk "github.com/DerrickGold/snesrecomp-go/internal/localizationkit"
)

func (app *application) prepareLocalizationFontSave(w http.ResponseWriter, r *http.Request) (localizationRequest, func(), error) {
	var q localizationRequest
	r.Body = http.MaxBytesReader(w, r.Body, lk.MaxAuthorPackBytes+(20<<20))
	err := r.ParseMultipartForm(2 << 20)
	cleanup := func() {
		if r.MultipartForm != nil {
			r.MultipartForm.RemoveAll()
		}
	}
	if err != nil {
		return q, cleanup, err
	}
	if len(r.MultipartForm.Value) != 1 || len(r.MultipartForm.Value["request"]) != 1 {
		return q, cleanup, fmt.Errorf("font save requires one request field")
	}
	q, err = decodeLocalizationJSON(strings.NewReader(r.FormValue("request")))
	if err != nil {
		return q, cleanup, err
	}
	if err = app.checkLocalizationIdentity(q.ProjectID, q.Revision); err != nil {
		return q, cleanup, err
	}
	if !q.SaveFonts || len(q.FontPaths) > 9 || len(r.MultipartForm.File) != len(q.FontPaths) {
		return q, cleanup, fmt.Errorf("invalid font upload count")
	}
	q.fontUploads = make(map[string][]byte)
	remaining := lk.MaxAuthorPackBytes
	for i, path := range q.FontPaths {
		if !lk.PortablePackPath(path) || q.fontUploads[path] != nil {
			return q, cleanup, fmt.Errorf("invalid or duplicate font upload path")
		}
		parts := r.MultipartForm.File["font"+strconv.Itoa(i)]
		if len(parts) != 1 || parts[0].Size <= 0 || parts[0].Size > lk.MaxPackFontBytes || parts[0].Size > int64(remaining) {
			return q, cleanup, fmt.Errorf("font upload exceeds its file or aggregate limit")
		}
		file, err := parts[0].Open()
		if err != nil {
			return q, cleanup, err
		}
		data, err := io.ReadAll(io.LimitReader(file, parts[0].Size+1))
		closeErr := file.Close()
		if err != nil || closeErr != nil || int64(len(data)) != parts[0].Size {
			return q, cleanup, fmt.Errorf("incomplete font upload")
		}
		remaining -= len(data)
		q.fontUploads[path] = data
	}
	return q, cleanup, r.Context().Err()
}

func (work *localizationWork) checkFontCoverage(ctx context.Context, p *lk.AuthorProject, samples []string) (lk.FontCoverageReport, error) {
	var fallback *lk.AuthorPack
	if p.Origin() != "native-source" {
		source := work.nativeLocalizationSource()
		if source == nil {
			return lk.FontCoverageReport{}, fmt.Errorf("extract the native US source before checking this translation's font coverage")
		}
		fallback = source.Pack()
	}
	return p.Pack().CheckFontCoverageWithFallback(ctx, work.fontProbe, fallback, samples)
}

func (work *localizationWork) requireFontCoverage(ctx context.Context, p *lk.AuthorProject) error {
	report, err := work.checkFontCoverage(ctx, p, nil)
	if err != nil {
		return err
	}
	if !report.Complete {
		first := report.Missing[0]
		return fmt.Errorf("font stack is missing %d character(s), starting with %s (%s). Open Fonts → Check coverage for message locations; add a suitable fallback font before installing or exporting", report.MissingCount, first.Codepoint, first.Character)
	}
	return nil
}
