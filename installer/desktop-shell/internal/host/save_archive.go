package host

import (
	"encoding/json"
	"fmt"
	"io"
	"mime"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

const maxExportBytes = 1 << 30

var preparedArchivePath = regexp.MustCompile(`^(mods|localization)/download/[0-9a-f]{36}$`)

func (h *Bridge) SetSavePicker(picker func(name, title string) (string, error)) {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.savePicker = picker
}

func (h *Bridge) serveSaveArchive(w http.ResponseWriter, r *http.Request) {
	h.mu.RLock()
	base, picker, proxy := h.backendURL, h.savePicker, h.proxy
	h.mu.RUnlock()
	if base == nil || picker == nil {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	fail := func(status int, err error) {
		outputJSON(w, status, map[string]string{"error": err.Error()})
	}
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	if r.Header.Get("Content-Type") != "application/json" {
		w.WriteHeader(http.StatusUnsupportedMediaType)
		return
	}
	var export struct {
		URL   string `json:"url"`
		Name  string `json:"name"`
		Title string `json:"title"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096)).Decode(&export); err != nil {
		fail(http.StatusBadRequest, fmt.Errorf("invalid archive save request"))
		return
	}
	extension := strings.ToLower(filepath.Ext(export.Name))
	validName := filepath.IsLocal(export.Name) && !strings.ContainsAny(export.Name, "/\\\r\n\x00") &&
		!strings.HasPrefix(export.Name, ".") && (extension == ".o2r" || extension == ".arproject")
	installed := extension == ".o2r" && export.URL == "mods/archive/"+url.PathEscape(export.Name)
	if !validName || (!installed && !preparedArchivePath.MatchString(export.URL)) {
		fail(http.StatusBadRequest, fmt.Errorf("select a Builder archive to save"))
		return
	}
	if !h.saving.TryLock() {
		fail(http.StatusConflict, fmt.Errorf("finish the open save dialog first"))
		return
	}
	defer h.saving.Unlock()

	destination, err := picker(export.Name, export.Title)
	if err != nil {
		fail(http.StatusInternalServerError, err)
		return
	}
	if destination == "" {
		outputJSON(w, http.StatusOK, map[string]bool{"saved": false})
		return
	}
	if !strings.EqualFold(filepath.Ext(destination), extension) {
		fail(http.StatusBadRequest, fmt.Errorf("save the archive with the %s extension", extension))
		return
	}

	relative, _ := url.Parse(export.URL) // Only the Builder archive routes validated above.
	request, err := http.NewRequestWithContext(r.Context(), http.MethodGet, base.ResolveReference(relative).String(), nil)
	if err != nil {
		fail(http.StatusBadRequest, err)
		return
	}
	response, err := proxy.Transport.RoundTrip(request)
	if err != nil {
		fail(http.StatusBadGateway, fmt.Errorf("could not read the archive from the Builder: %w", err))
		return
	}
	defer response.Body.Close()
	_, parameters, _ := mime.ParseMediaType(response.Header.Get("Content-Disposition"))
	if response.StatusCode != http.StatusOK || response.Header.Get("Content-Type") != "application/zip" || parameters["filename"] != export.Name {
		fail(http.StatusConflict, fmt.Errorf("the archive is no longer available; prepare it again"))
		return
	}
	if err := saveArchiveFile(destination, response.Body, response.ContentLength); err != nil {
		fail(http.StatusInternalServerError, err)
		return
	}
	outputJSON(w, http.StatusOK, map[string]bool{"saved": true})
}

func saveArchiveFile(destination string, input io.Reader, size int64) error {
	if size <= 0 || size > maxExportBytes {
		return fmt.Errorf("invalid archive size")
	}
	file, err := os.CreateTemp(filepath.Dir(destination), ".actraiser-export-*")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	written, err := io.Copy(file, io.LimitReader(input, size+1))
	closeErr := file.Close()
	if err != nil {
		return err
	}
	if written != size {
		return fmt.Errorf("the archive transfer was incomplete")
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(file.Name(), destination)
}
