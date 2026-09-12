package host

import (
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"
)

func closeTestBackend(t *testing.T, status *atomic.Int32, requests *atomic.Int32) *Backend {
	t.Helper()
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/0123456789abcdef0123456789abcdef0123/close" {
			t.Errorf("unexpected close request: %s %s", r.Method, r.URL.Path)
		}
		requests.Add(1)
		if code := int(status.Load()); code != http.StatusOK {
			http.Error(w, "wait for the current build to finish", code)
		}
	}))
	t.Cleanup(server.Close)
	u, err := url.Parse(server.URL + "/0123456789abcdef0123456789abcdef0123/")
	if err != nil {
		t.Fatal(err)
	}
	return &Backend{URL: u, Done: make(chan struct{})}
}

func TestCloseUsesNativeYesNoResponses(t *testing.T) {
	for _, answer := range []string{CloseAffirmative, CloseNegative, "Cancel", "", "Close", "OK"} {
		t.Run(answer, func(t *testing.T) {
			var status, requests atomic.Int32
			status.Store(http.StatusOK)
			b := closeTestBackend(t, &status, &requests)
			var guard CloseGuard
			prevent := guard.BeforeClose(b, func() (string, error) { return answer, nil }, func(err error) { t.Fatal(err) })
			wantClose := answer == "Yes"
			if prevent == wantClose || (requests.Load() == 1) != wantClose {
				t.Fatalf("answer %q: prevent=%v, close requests=%d", answer, prevent, requests.Load())
			}
		})
	}
}

func TestCloseBusyBackendCanRetry(t *testing.T) {
	var status, requests atomic.Int32
	status.Store(http.StatusConflict)
	b := closeTestBackend(t, &status, &requests)
	var guard CloseGuard
	var reported error
	confirm := func() (string, error) { return CloseAffirmative, nil }
	report := func(err error) { reported = err }
	if !guard.BeforeClose(b, confirm, report) || reported == nil || !strings.Contains(reported.Error(), "current build") {
		t.Fatalf("busy close must be vetoed and explained: %v", reported)
	}
	status.Store(http.StatusOK)
	reported = nil
	if guard.BeforeClose(b, confirm, report) || reported != nil || requests.Load() != 2 {
		t.Fatalf("retry failed: %v, requests=%d", reported, requests.Load())
	}
}

func TestCloseSerializesDialogsAndRemembersApproval(t *testing.T) {
	var status, requests atomic.Int32
	status.Store(http.StatusOK)
	b := closeTestBackend(t, &status, &requests)
	var guard CloseGuard
	entered, answer, finished := make(chan struct{}), make(chan string), make(chan bool, 1)
	unwanted := func() (string, error) { t.Error("opened another dialog"); return "", nil }
	report := func(err error) { t.Error(err) }
	go func() {
		finished <- guard.BeforeClose(b, func() (string, error) { close(entered); return <-answer, nil }, report)
	}()
	<-entered
	if !guard.BeforeClose(b, unwanted, report) {
		t.Error("duplicate close bypassed the pending confirmation")
	}
	answer <- CloseNegative
	if !<-finished {
		t.Fatal("No did not veto close")
	}
	if guard.BeforeClose(b, func() (string, error) { return CloseAffirmative, nil }, report) {
		t.Fatal("Yes after No did not close")
	}
	if guard.BeforeClose(b, unwanted, report) || requests.Load() != 1 {
		t.Fatal("an approved close opened another confirmation/request")
	}
}

func TestCloseAllowsStartupAndExitedBackend(t *testing.T) {
	b := &Backend{Done: make(chan struct{})}
	close(b.Done)
	for _, backend := range []*Backend{nil, b} {
		var guard CloseGuard
		if guard.BeforeClose(backend, func() (string, error) { t.Error("unexpected dialog"); return "", nil }, func(err error) { t.Error(err) }) {
			t.Fatal("closing was vetoed without a running backend")
		}
	}
}

func TestCloseReportsDialogAndTransportErrors(t *testing.T) {
	for _, failure := range []string{"dialog", "transport"} {
		t.Run(failure, func(t *testing.T) {
			var guard CloseGuard
			// Invalid protocol fails locally without depending on a refused TCP port.
			b := &Backend{URL: &url.URL{Scheme: "invalid", Host: "127.0.0.1"}, Done: make(chan struct{})}
			var reported error
			prevent := guard.BeforeClose(b, func() (string, error) {
				if failure == "dialog" {
					return "", errors.New("dialog unavailable")
				}
				return CloseAffirmative, nil
			}, func(err error) { reported = err })
			if !prevent || reported == nil {
				t.Fatalf("failure must be reported without silently discarding edits: prevent=%v, error=%v", prevent, reported)
			}
		})
	}
}

func TestCloseDoesNotLoseBackendExitDuringDialogs(t *testing.T) {
	for _, dialog := range []string{"confirmation", "warning"} {
		t.Run(dialog, func(t *testing.T) {
			var guard CloseGuard
			b := &Backend{Done: make(chan struct{})}
			exitWhilePending := func() {
				close(b.Done)
				// Simulate the backend-exit watcher's concurrent Quit callback.
				if !guard.BeforeClose(b, nil, nil) {
					t.Error("pending guard was bypassed")
				}
			}
			prevent := guard.BeforeClose(b, func() (string, error) {
				if dialog == "confirmation" {
					exitWhilePending()
					return CloseNegative, nil
				}
				return "", errors.New("dialog failed")
			}, func(error) { exitWhilePending() })
			if prevent {
				t.Fatal("backend-exit request was lost behind a dialog")
			}
		})
	}
}
