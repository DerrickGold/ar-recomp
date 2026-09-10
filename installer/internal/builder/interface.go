package builder

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"

	"github.com/DerrickGold/ar-recomp/installer/internal/interfacecatalog"
)

type interfacePreferences struct {
	Language string `json:"language"`
}

// Interface catalogs are trusted executable resources, never imported game
// pack data. Cache their validation/encoding once, independently of sessions.
var interfaceMessages = sync.OnceValues(func() (json.RawMessage, error) {
	entries, err := interfacecatalog.Entries()
	if err != nil {
		return nil, err
	}
	messages := make(map[string][]string)
	for _, entry := range entries {
		if strings.HasPrefix(entry.Key, "builder.") || strings.HasPrefix(entry.Key, "common.") || strings.HasPrefix(entry.Key, "interface.") {
			messages[entry.Key] = entry.Text
		}
	}
	return json.Marshal(messages)
})

func (app *application) interfacePreferencesPath() string {
	return filepath.Join(app.options.ProjectRoot, "game-assets", "workshop-settings.json")
}

func (app *application) serveInterfaceFont(w http.ResponseWriter, r *http.Request) {
	// Fixed, shipped host asset only. This is not a general filesystem or
	// imported-pack font endpoint. System fonts still work if it is absent.
	path := filepath.Join(app.options.ProjectRoot, "game-assets", "fonts", "noto", "NotoSansJP-Bold.otf")
	f, err := os.Open(path)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil || !info.Mode().IsRegular() {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "font/otf")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Cache-Control", "private, max-age=3600")
	http.ServeContent(w, r, "japanese.otf", info.ModTime(), f)
}

func decodeInterfacePreferences(reader io.Reader) (interfacePreferences, error) {
	var value interfacePreferences
	d := json.NewDecoder(reader)
	d.DisallowUnknownFields()
	if err := d.Decode(&value); err != nil {
		return value, err
	}
	var extra any
	if err := d.Decode(&extra); err != io.EOF {
		return value, errors.New("unexpected preference data")
	}
	if !slices.Contains(interfacecatalog.Locales(), value.Language) {
		return value, errors.New("unsupported interface language")
	}
	return value, nil
}

func (app *application) readInterfacePreferences() (interfacePreferences, error) {
	app.interfaceMu.Lock()
	defer app.interfaceMu.Unlock()
	fallback := interfacePreferences{Language: "en"}
	f, err := os.Open(app.interfacePreferencesPath())
	if os.IsNotExist(err) {
		return fallback, nil
	}
	if err != nil {
		return fallback, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, 1025))
	if err != nil {
		return fallback, err
	}
	if len(data) > 1024 {
		return fallback, errors.New("interface preferences too large")
	}
	prefs, err := decodeInterfacePreferences(bytes.NewReader(data))
	if err != nil {
		return fallback, err
	}
	return prefs, nil
}

// This is a non-executable application/json element. json.Marshal escapes HTML
// delimiters, so even future text containing </script> cannot exit the element.
func (app *application) interfaceBootstrap() (string, error) {
	messages, err := interfaceMessages()
	if err != nil {
		return "", err
	}
	prefs, preferenceErr := app.readInterfacePreferences()
	value := struct {
		Locale          string          `json:"locale"`
		Locales         []string        `json:"locales"`
		Messages        json.RawMessage `json:"messages"`
		PreferenceError bool            `json:"preferenceError"`
	}{prefs.Language, interfacecatalog.Locales(), messages, preferenceErr != nil}
	encoded, err := json.Marshal(value)
	return string(encoded), err
}

func (app *application) serveInterfacePreferences(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	if r.Method != http.MethodPost {
		http.NotFound(w, r)
		return
	}
	defer r.Body.Close()
	prefs, err := decodeInterfacePreferences(http.MaxBytesReader(w, r.Body, 1024))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "Choose a supported interface language.", "errorCode": "builder.interface.invalid"})
		return
	}
	encoded, err := json.Marshal(prefs)
	if err == nil {
		// Only this tiny preference transaction is serialized. No pack editor,
		// build-session, asset or response-transfer lock is held here.
		app.interfaceMu.Lock()
		path := app.interfacePreferencesPath()
		// Do not let the Windows replacement fallback rename a directory or
		// symlink out of the way. This path belongs only to this preference file.
		if info, statErr := os.Lstat(path); statErr == nil && !info.Mode().IsRegular() {
			err = errors.New("interface preference target is not a regular file")
		} else if statErr != nil && !os.IsNotExist(statErr) {
			err = statErr
		} else {
			err = writeAtomicFile(path, append(encoded, '\n'))
		}
		app.interfaceMu.Unlock()
	}
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": "Could not save the interface language.", "errorCode": "builder.interface.save_failed", "detail": fmt.Sprint(err)})
		return
	}
	writeJSON(w, http.StatusOK, prefs)
}
