package builder

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

func (app *application) saveBuildReport(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("Content-Type") != "application/json" {
		writeJSONError(w, http.StatusUnsupportedMediaType, "expected a JSON report request")
		return
	}
	var request struct {
		// Captures errors shown by the frontend, including failures before a
		// build starts and the server has a build error or log to report.
		Message   string `json:"message"`
		Operation string `json:"operation"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 32<<10)).Decode(&request); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid build report request")
		return
	}

	app.mu.Lock()
	state, buildError := app.state, app.errorMessage
	phase := app.progress
	tail := app.log.String()
	app.mu.Unlock()

	dir := filepath.Join(app.options.ProjectRoot, "logs")
	if err := os.MkdirAll(dir, 0700); err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	file, err := os.CreateTemp(dir, "builder-report-*.txt")
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	report := fmt.Sprintf("ActRaiser Recomp Builder %s\nRecorded %s\n", app.options.Version, time.Now().Format(time.RFC3339))
	if request.Operation == "launch" {
		report += "Launch error\n" + request.Message
	} else {
		report += fmt.Sprintf("State %s\nProgress %d%% %s\n%s\n%s\n\nBuild log\n%s",
			state, phase.Percent, phase.PhaseLabel, request.Message, buildError, tail)
	}
	_, err = file.WriteString(report)
	closeErr := file.Close()
	if err == nil {
		err = closeErr
	}
	if err != nil {
		_ = os.Remove(file.Name())
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusCreated, map[string]string{"path": "logs/" + filepath.Base(file.Name())})
}
