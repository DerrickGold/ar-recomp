package buildgui

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestBuilderAcceptsROMAndReportsSuccess(t *testing.T) {
	root := t.TempDir()
	built := make(chan string, 1)
	options := Options{
		ProjectRoot: root,
		Build: func(_ context.Context, romPath string, output io.Writer) (Result, error) {
			data, err := os.ReadFile(romPath)
			if err != nil {
				return Result{}, err
			}
			if string(data) != "test-rom" {
				t.Fatalf("stored ROM = %q", data)
			}
			_, _ = io.WriteString(output, "build log\n")
			built <- romPath
			return Result{Message: "done", OutputPath: "run-game"}, nil
		},
	}
	app := newApplication(context.Background(), options, "test-token")

	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	part, err := writer.CreateFormFile("rom", "ActRaiser.SFC")
	if err != nil {
		t.Fatal(err)
	}
	_, _ = io.WriteString(part, "test-rom")
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/test-token/build", &body)
	request.Header.Set("Content-Type", writer.FormDataContentType())
	response := httptest.NewRecorder()
	app.ServeHTTP(response, request)
	if response.Code != http.StatusAccepted {
		t.Fatalf("build status = %d", response.Code)
	}

	select {
	case romPath := <-built:
		if romPath != filepath.Join(root, "user-rom.sfc") {
			t.Fatalf("ROM path = %s", romPath)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("build callback did not run")
	}

	deadline := time.Now().Add(2 * time.Second)
	for {
		statusResponse := httptest.NewRecorder()
		app.ServeHTTP(statusResponse,
			httptest.NewRequest(http.MethodGet, "/test-token/status", nil))
		var current status
		if err := json.NewDecoder(statusResponse.Body).Decode(&current); err != nil {
			t.Fatal(err)
		}
		if current.State == "succeeded" {
			if current.Log != "build log\n" || current.Message != "done" {
				t.Fatalf("unexpected status: %+v", current)
			}
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("build did not finish: %+v", current)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestBuilderRejectsWrongExtension(t *testing.T) {
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		Build: func(context.Context, string, io.Writer) (Result, error) {
			t.Fatal("build callback should not run")
			return Result{}, nil
		},
	}, "test-token")

	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	part, _ := writer.CreateFormFile("rom", "notes.txt")
	_, _ = io.WriteString(part, "not a rom")
	_ = writer.Close()
	request := httptest.NewRequest(http.MethodPost, "/test-token/build", &body)
	request.Header.Set("Content-Type", writer.FormDataContentType())
	response := httptest.NewRecorder()
	app.ServeHTTP(response, request)
	if response.Code != http.StatusBadRequest ||
		!strings.Contains(response.Body.String(), ".sfc or .smc") {
		t.Fatalf("unexpected response: %d %s", response.Code, response.Body.String())
	}
}

func TestBuilderTokenHidesEndpoints(t *testing.T) {
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		Build: func(context.Context, string, io.Writer) (Result, error) {
			return Result{}, nil
		},
	}, "secret")
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/status", nil))
	if response.Code != http.StatusNotFound {
		t.Fatalf("status without token = %d", response.Code)
	}
}

func TestBuilderPageEscapesTitleAndSetsLocalSecurityHeaders(t *testing.T) {
	app := newApplication(context.Background(), Options{
		Title:       `<unsafe>`,
		ProjectRoot: t.TempDir(),
	}, "secret")
	response := httptest.NewRecorder()
	app.ServeHTTP(response,
		httptest.NewRequest(http.MethodGet, "/secret/", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("page status = %d", response.Code)
	}
	if strings.Contains(response.Body.String(), "<title><unsafe>") ||
		!strings.Contains(response.Body.String(), "<title>&lt;unsafe&gt;</title>") {
		t.Fatalf("title was not escaped: %s", response.Body.String())
	}
	for _, header := range []string{
		"Content-Security-Policy", "Referrer-Policy", "X-Content-Type-Options",
	} {
		if response.Header().Get(header) == "" {
			t.Errorf("missing %s header", header)
		}
	}
	if policy := response.Header().Get("Content-Security-Policy"); !strings.Contains(policy, "media-src 'self' blob:") {
		t.Errorf("audio policy does not admit local WAV/blob playback: %s", policy)
	}
}

func TestLockedLogWriterKeepsBoundedTail(t *testing.T) {
	app := newApplication(context.Background(), Options{}, "secret")
	writer := &lockedLogWriter{app: app}
	payload := append(bytes.Repeat([]byte("a"), maxLogBytes+32), []byte("tail")...)
	written, err := writer.Write(payload)
	if err != nil {
		t.Fatal(err)
	}
	if written != len(payload) {
		t.Fatalf("Write returned %d, want %d", written, len(payload))
	}
	if app.log.Len() != maxLogBytes ||
		!strings.HasSuffix(app.log.String(), "tail") {
		t.Fatalf("log length = %d, suffix retained = %t",
			app.log.Len(), strings.HasSuffix(app.log.String(), "tail"))
	}
}
