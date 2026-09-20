package builder

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
)

func decodeLocalizationRequest[T any](w http.ResponseWriter, r *http.Request) (T, error) {
	var q T
	r.Body = http.MaxBytesReader(w, r.Body, 20<<20)
	if !strings.HasPrefix(r.Header.Get("Content-Type"), "application/json") {
		return q, fmt.Errorf("expected application/json")
	}
	return decodeLocalizationJSON[T](r.Body)
}

func decodeLocalizationJSON[T any](input io.Reader) (T, error) {
	var q T
	decoder := json.NewDecoder(input)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&q); err != nil {
		return q, err
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF {
		return q, fmt.Errorf("expected one JSON request")
	}
	return q, nil
}

// A command owns decoded request data, never a live session pointer. The HTTP
// adapter performs I/O before the writer lock and runs it on a detached snapshot.
type localizationCommand func(*localizationWork, *localizationReply) error

func localizationJSONCommand[T any](w http.ResponseWriter, r *http.Request, endpoint string,
	handle func(*localizationWork, *localizationReply, *http.Request, string, T) error) (localizationCommand, error) {
	q, err := decodeLocalizationRequest[T](w, r)
	if err != nil {
		return nil, err
	}
	return func(work *localizationWork, reply *localizationReply) error {
		return handle(work, reply, r, endpoint, q)
	}, nil
}

func (app *application) prepareLocalizationCommand(w http.ResponseWriter, r *http.Request, endpoint string) (localizationCommand, func(), error) {
	if endpoint == "save" || endpoint == "preview" || endpoint == "playback" {
		var q localizationDraftRequest
		var cleanup func()
		var err error
		if strings.HasPrefix(r.Header.Get("Content-Type"), "multipart/form-data") {
			q, cleanup, err = app.prepareLocalizationFontSave(w, r)
		} else {
			q, err = decodeLocalizationRequest[localizationDraftRequest](w, r)
		}
		return func(work *localizationWork, reply *localizationReply) error {
			return work.editLocalization(reply, r, endpoint, q)
		}, cleanup, err
	}
	var command localizationCommand
	var err error
	switch endpoint {
	case "edit", "metadata", "notice", "materialize":
		command, err = localizationJSONCommand(w, r, endpoint, (*localizationWork).editLocalization)
	case "open", "reference", "directory", "accept-import", "create", "clone", "upgrade-v2":
		command, err = localizationJSONCommand(w, r, endpoint, (*localizationWork).manageLocalizationProject)
	case "installation", "installation-check", "install", "set-enabled", "uninstall":
		command, err = localizationJSONCommand(w, r, endpoint, (*localizationWork).manageLocalizationInstallation)
	case "backup", "publish", "publication-check":
		command, err = localizationJSONCommand(w, r, endpoint, (*localizationWork).exportLocalization)
	case "font-coverage":
		command, err = localizationJSONCommand(w, r, endpoint, (*localizationWork).inspectLocalizationFonts)
	default:
		err = fmt.Errorf("unknown localization action")
	}
	return command, nil, err
}
