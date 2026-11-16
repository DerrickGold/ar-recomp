package builder

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSaveBuildReportIncludesVisibleErrorAndLog(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "local-session")
	app.state = "failed"
	app.errorMessage = "compiler failed"
	app.progress = progress{Percent: 63, PhaseLabel: "Compiling"}
	app.log.WriteString("last compiler output")
	request := httptest.NewRequest(http.MethodPost, app.prefix+"build-report", strings.NewReader(`{"message":"Build could not finish"}`))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	app.ServeHTTP(response, request)
	if response.Code != http.StatusCreated {
		t.Fatalf("save report: %d %s", response.Code, response.Body.String())
	}
	var result struct{ Path string }
	if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if filepath.Dir(filepath.FromSlash(result.Path)) != "logs" {
		t.Fatalf("path = %q", result.Path)
	}
	body, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(result.Path)))
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"Progress 63% Compiling", "Build could not finish", "compiler failed", "last compiler output"} {
		if !strings.Contains(string(body), want) {
			t.Errorf("report missing %q", want)
		}
	}
}
