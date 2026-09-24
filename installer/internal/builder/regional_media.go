package builder

import (
	"bytes"
	"errors"
	"io"
	"net/http"

	"github.com/DerrickGold/ar-recomp/installer/internal/gamerom"
	"github.com/DerrickGold/ar-recomp/installer/internal/regionalmedia"
)

func (app *application) serveRegionalMedia(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		rows, err := regionalmedia.ListInstalled(app.options.ProjectRoot)
		if err != nil {
			writeJSONError(w, 500, err.Error())
			return
		}
		writeJSON(w, 200, map[string]any{"donors": rows})
		return
	}
	if r.Method != http.MethodPost {
		http.NotFound(w, r)
		return
	}
	// No ROM is copied into the installation. The same game-owned extractor
	// and parser used by the CLI determine identity, resource set and filename.
	r.Body = http.MaxBytesReader(w, r.Body, regionalmedia.MaximumBytes+(64<<10))
	if err := r.ParseMultipartForm(regionalmedia.MaximumBytes + (64 << 10)); err != nil {
		writeJSONError(w, 400, "cannot read the selected regional ROM or media package")
		return
	}
	if r.MultipartForm == nil {
		writeJSONError(w, 400, "select one regional ROM or media package")
		return
	}
	defer r.MultipartForm.RemoveAll()
	if len(r.MultipartForm.File) != 1 || len(r.MultipartForm.File["donor"]) != 1 {
		writeJSONError(w, 400, "select one regional ROM or media package")
		return
	}
	for key, values := range r.MultipartForm.Value {
		if key != "replace" || len(values) != 1 || (values[0] != "true" && values[0] != "false") {
			writeJSONError(w, 400, "invalid regional media option")
			return
		}
	}
	file, err := r.MultipartForm.File["donor"][0].Open()
	if err != nil {
		writeJSONError(w, 400, err.Error())
		return
	}
	data, err := io.ReadAll(io.LimitReader(file, regionalmedia.MaximumBytes+1))
	file.Close()
	if err != nil || len(data) > regionalmedia.MaximumBytes {
		writeJSONError(w, 400, "regional media exceeds the supported size")
		return
	}
	if !bytes.HasPrefix(data, []byte("ARMEDIA\x00")) && len(data) == gamerom.Size {
		extraction, extractErr := regionalmedia.Extract(data)
		if extractErr != nil {
			writeJSONError(w, 400, extractErr.Error())
			return
		}
		data, err = regionalmedia.Pack(extraction)
		if err != nil {
			writeJSONError(w, 400, err.Error())
			return
		}
	}
	app.regionalMediaMu.Lock()
	release, changed, err := regionalmedia.Install(app.options.ProjectRoot, data, r.FormValue("replace") == "true")
	app.regionalMediaMu.Unlock()
	if errors.Is(err, regionalmedia.ErrReplaceRequired) {
		writeJSON(w, http.StatusConflict, map[string]string{"code": "replace_required", "release": release})
		return
	}
	if err != nil {
		writeJSONError(w, 400, err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"release": release, "changed": changed})
}
