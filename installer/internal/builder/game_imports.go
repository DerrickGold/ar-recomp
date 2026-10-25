package builder

import (
	"io"
	"net/http"

	"github.com/DerrickGold/ar-recomp/installer/internal/gameimports"
)

func (app *application) serveGameImports(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		writeJSON(w, http.StatusOK, map[string]any{"imports": gameimports.List(app.options.ProjectRoot)})
		return
	}
	if r.Method != http.MethodPost {
		http.NotFound(w, r)
		return
	}
	limit := gameimports.MaximumROMBytes()
	r.Body = http.MaxBytesReader(w, r.Body, limit+(64<<10))
	if err := r.ParseMultipartForm(limit + (64 << 10)); err != nil {
		writeJSONError(w, 400, "cannot read the game ROM")
		return
	}
	if r.MultipartForm == nil {
		writeJSONError(w, 400, "select one game ROM")
		return
	}
	defer r.MultipartForm.RemoveAll()
	if len(r.MultipartForm.File) != 1 || len(r.MultipartForm.File["rom"]) != 1 || len(r.MultipartForm.Value) != 1 || len(r.MultipartForm.Value["content"]) != 1 {
		writeJSONError(w, 400, "select one game ROM")
		return
	}
	file, err := r.MultipartForm.File["rom"][0].Open()
	if err != nil {
		writeJSONError(w, 400, err.Error())
		return
	}
	rom, err := io.ReadAll(io.LimitReader(file, limit+1))
	file.Close()
	if err != nil || int64(len(rom)) > limit {
		writeJSONError(w, 400, "game ROM exceeds the supported size")
		return
	}
	app.gameImportMu.Lock()
	changed, err := gameimports.Import(app.options.ProjectRoot, r.MultipartForm.Value["content"][0], rom)
	app.gameImportMu.Unlock()
	if err != nil {
		writeJSONError(w, 400, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"status": "installed", "changed": changed})
}
