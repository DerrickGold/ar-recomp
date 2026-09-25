package builder

import (
	"bytes"
	_ "embed"
	"fmt"
	"io"
	"net/http"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// The GUI is an adapter over localization, not another script parser or ROM
// decoder. Immutable project snapshots make disk-save failure transactional.
//
//go:embed localization.html
var localizationHTML string

//go:embed localization.css
var localizationCSS string

//go:embed localization.js
var localizationJS string

//go:embed localization_font_editor.js
var localizationFontEditorJS string

//go:embed localization_import_editor.js
var localizationImportEditorJS string

//go:embed localization_library.js
var localizationLibraryJS string

//go:embed localization_styling.js
var localizationStylingJS string

//go:embed localization_playback.js
var localizationPlaybackJS string

type localizationSession struct {
	mu     sync.Mutex
	editMu sync.Mutex
	initMu sync.Mutex
	localizationStateData
}

type localizationExport struct {
	token, name string
	data        []byte
}

func (app *application) localizationRoot() string {
	return filepath.Join(app.options.ProjectRoot, "game-assets", "languages")
}

func (app *application) serveLocalization(w http.ResponseWriter, r *http.Request, endpoint string) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	if endpoint == "choose-directory" && r.Method == http.MethodPost {
		app.chooseLocalizationDirectory(w, r)
		return
	}
	if r.Method == http.MethodGet && (endpoint == "editor.js" || endpoint == "editor.css" || endpoint == "playback.js") {
		if endpoint == "playback.js" {
			w.Header().Set("Content-Type", "text/javascript; charset=utf-8")
			io.WriteString(w, localizationPlaybackJS)
		} else if endpoint == "editor.js" {
			w.Header().Set("Content-Type", "text/javascript; charset=utf-8")
			io.WriteString(w, localizationStylingJS)
			io.WriteString(w, localizationFontEditorJS)
			io.WriteString(w, localizationLibraryJS)
			io.WriteString(w, localizationImportEditorJS)
			io.WriteString(w, localizationJS)
		} else {
			w.Header().Set("Content-Type", "text/css; charset=utf-8")
			io.WriteString(w, localizationCSS)
		}
		return
	}
	if r.Method != http.MethodGet && r.Method != http.MethodPost {
		w.Header().Set("Allow", "GET, POST")
		writeJSONError(w, 405, "method not allowed")
		return
	}
	s := &app.localization
	// Everything that waits on the client -- reading a request body, writing an
	// archive back -- and the ROM decode that follows an upload happen off the
	// session lock. The same lock answers the build page's 500 ms status poll,
	// so a stalled transfer must not be able to hold it.
	if r.Method == http.MethodGet && strings.HasPrefix(endpoint, "download/") {
		app.serveLocalizationDownload(w, r, endpoint)
		return
	}
	var upload *localizationUpload
	var command localizationCommand
	var err error
	if r.Method == http.MethodPost {
		if endpoint == "extract" || endpoint == "import" {
			upload, err = app.prepareLocalizationUpload(w, r, endpoint)
			if upload != nil && upload.cleanup != nil {
				defer upload.cleanup()
			}
		} else {
			var cleanup func()
			command, cleanup, err = app.prepareLocalizationCommand(w, r, endpoint)
			if cleanup != nil {
				defer cleanup()
			}
		}
		if err != nil {
			writeLocalizationError(w, err)
			return
		}
	}
	// A writer owns its detached working snapshot. Other readers/status polls
	// keep using the previous immutable project until publication. The separate
	// writer mutex orders mutations without holding the state lock over I/O.
	reply := &localizationReply{}
	err = func() error {
		if r.Method == http.MethodPost && endpoint != "font-coverage" && endpoint != "playback" && endpoint != "preview" {
			s.editMu.Lock()
			defer s.editMu.Unlock()
		}
		if err := r.Context().Err(); err != nil {
			return err
		}
		work, err := app.localizationSnapshot()
		if err != nil {
			return err
		}
		if r.Method == http.MethodGet {
			return work.readLocalization(reply, r, endpoint)
		}
		if endpoint == "playback" || endpoint == "font-coverage" || endpoint == "preview" {
			return command(work, reply)
		}
		if upload != nil {
			err = work.commitLocalizationUpload(reply, r, upload)
		} else {
			err = command(work, reply)
		}
		s.mu.Lock()
		s.localizationStateData = work.localizationStateData
		s.mu.Unlock()
		return err
	}()
	if err != nil {
		writeLocalizationError(w, err)
	} else {
		reply.write(w, r)
	}
}

// The prepared archive is immutable once published, so it is served from a
// snapshot taken under a momentary lock rather than for the whole transfer.
func (app *application) serveLocalizationDownload(w http.ResponseWriter, r *http.Request, endpoint string) {
	app.localization.mu.Lock()
	export := app.localization.export
	app.localization.mu.Unlock()
	if export == nil || endpoint != "download/"+export.token {
		writeJSONError(w, http.StatusBadRequest, "download expired; prepare the archive again")
		return
	}
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Content-Disposition", fmt.Sprintf(`attachment; filename="%s"`, export.name))
	http.ServeContent(w, r, export.name, time.Time{}, bytes.NewReader(export.data))
}
