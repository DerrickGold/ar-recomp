package builder

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestStructuredProgressIsMonotonic(t *testing.T) {
	current := initialProgress()
	for _, update := range []BuildProgress{
		{PhaseID: "localization", Completed: 1, Total: 1},
		{PhaseID: "regen", Completed: 0, Total: 1},
		{PhaseID: "compile", Completed: 10, Total: 100},
		{PhaseID: "compile", Completed: 90, Total: 100},
		{PhaseID: "regen", Completed: 1, Total: 1},
		{PhaseID: "install", Completed: 1, Total: 1},
	} {
		next := progressFromEvent(current, update)
		if next.Percent < current.Percent || next.PhaseIndex < current.PhaseIndex {
			t.Fatalf("progress went backwards: %#v -> %#v", current, next)
		}
		current = next
	}
	if current.PhaseID != "install" || current.Percent != 99 {
		t.Fatalf("final in-flight progress = %#v", current)
	}
}

func TestCompletedProgressReservesHundredForSuccess(t *testing.T) {
	if got := completedProgress(); got.Percent != 100 || len(got.Completed) != len(buildPhases) {
		t.Fatalf("completed progress = %#v", got)
	}
}

func TestPageEmbedsEveryPhaseAndLeavesNoPlaceholder(t *testing.T) {
	app := newApplication(context.Background(), Options{Title: "Builder", ProjectRoot: t.TempDir()}, "secret")
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/secret/", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("page status = %d", response.Code)
	}
	body := response.Body.String()
	for _, item := range buildPhases {
		if !strings.Contains(body, `data-step="`+item.id+`"`) {
			t.Errorf("page is missing phase %q", item.id)
		}
	}
}

func TestStatusAlwaysIncludesProgress(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "secret")
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/secret/status", nil))
	var payload struct {
		State    string   `json:"state"`
		Progress progress `json:"progress"`
	}
	if err := json.NewDecoder(response.Body).Decode(&payload); err != nil {
		t.Fatal(err)
	}
	if payload.State != "idle" || payload.Progress.Percent != 0 || payload.Progress.Completed == nil {
		t.Fatalf("idle status = %#v", payload)
	}
}
