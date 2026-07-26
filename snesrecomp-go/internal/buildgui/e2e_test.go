package buildgui

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// Drive the real endpoints with a build that emits the real log lines, and
// assert the bar advances and finishes. This is the closest thing to watching
// the page without a ROM or a toolchain: it exercises the whole chain the
// browser sees -- multipart upload, the log writer, the status poll, and the
// derived progress -- rather than computeProgress in isolation.
func TestEndToEndProgressAdvances(t *testing.T) {
	release := make(chan struct{})
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		Build: func(_ context.Context, _ string, out io.Writer) (Result, error) {
			for _, chunk := range []string{
				logRegen, logFuncs, logMetadata, logRTS, logStubs, logDone,
				logUnits, strings.Repeat("  cc src/present.c\n", 256),
				logLinking, logBuilt, "Playable game installed at ./ActRaiserRecomp\n",
			} {
				io.WriteString(out, chunk)
			}
			<-release
			return Result{Message: "done", OutputPath: "run-game"}, nil
		},
	}, "tok")

	var body bytes.Buffer
	w := multipart.NewWriter(&body)
	part, _ := w.CreateFormFile("rom", "ar.sfc")
	io.WriteString(part, "rom")
	w.Close()
	req := httptest.NewRequest(http.MethodPost, "/tok/build", &body)
	req.Header.Set("Content-Type", w.FormDataContentType())
	rec := httptest.NewRecorder()
	app.ServeHTTP(rec, req)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("build = %d", rec.Code)
	}

	poll := func() status {
		r := httptest.NewRecorder()
		app.ServeHTTP(r, httptest.NewRequest(http.MethodGet, "/tok/status", nil))
		var s status
		json.NewDecoder(r.Body).Decode(&s)
		return s
	}
	// Wait for the log to be fully written, then check the mid-build reading.
	deadline := time.Now().Add(2 * time.Second)
	var mid status
	for time.Now().Before(deadline) {
		mid = poll()
		if strings.Contains(mid.Log, "Playable game installed") {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Logf("mid-build: state=%s phase=%s pct=%d detail=%q",
		mid.State, mid.Progress.PhaseID, mid.Progress.Percent, mid.Progress.Detail)
	if mid.State != "building" {
		t.Fatalf("state = %q, want building", mid.State)
	}
	if mid.Progress.Percent >= 100 {
		t.Errorf("pct = %d mid-build", mid.Progress.Percent)
	}
	if mid.Progress.PhaseID != "install" {
		t.Errorf("phase = %q", mid.Progress.PhaseID)
	}

	close(release)
	deadline = time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		final := poll()
		if final.State == "succeeded" {
			t.Logf("final: pct=%d completed=%d", final.Progress.Percent, len(final.Progress.Completed))
			if final.Progress.Percent != 100 {
				t.Errorf("final pct = %d", final.Progress.Percent)
			}
			if len(final.Progress.Completed) != len(buildPhases) {
				t.Errorf("completed=%d", len(final.Progress.Completed))
			}
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("build never succeeded")
}
