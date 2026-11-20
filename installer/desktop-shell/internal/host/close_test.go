package host

import (
	"bufio"
	"net/http"
	"net/http/httptest"
	"net/url"
	"sync/atomic"
	"testing"
	"time"
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

func TestNativeCloseWhileStarting(t *testing.T) {
	var bridge Bridge
	if bridge.BeforeClose(nil) {
		t.Fatal("startup without a backend should allow closing")
	}
	var status, requests atomic.Int32
	status.Store(http.StatusConflict)
	b := closeTestBackend(t, &status, &requests)
	if !bridge.BeforeClose(b) {
		t.Fatal("the backend refused to close while building")
	}
	status.Store(http.StatusOK)
	if bridge.BeforeClose(b) || requests.Load() != 2 {
		t.Fatal("the host should allow a successful startup close")
	}
}

func TestNativeCloseReachesRendererAndWaitsForBackend(t *testing.T) {
	var bridge Bridge
	var status, requests atomic.Int32
	status.Store(http.StatusOK)
	b := closeTestBackend(t, &status, &requests)
	bridge.Connect(b.URL)

	path := "/__shell/close-requests"
	native := httptest.NewRecorder()
	bridge.Bootstrap(native, httptest.NewRequest(http.MethodGet, path, nil))
	if native.Code != http.StatusNotFound {
		t.Fatalf("close stream exposed on the bootstrap origin: %d", native.Code)
	}

	address, stop, err := StartRenderer(&bridge)
	if err != nil {
		t.Fatal(err)
	}
	defer stop()
	client := &http.Client{Timeout: 3 * time.Second}
	connect := func() *http.Response {
		t.Helper()
		response, err := client.Get(address + path[1:])
		if err != nil {
			t.Fatal(err)
		}
		if response.StatusCode != http.StatusOK || response.Header.Get("Content-Type") != "text/event-stream" {
			response.Body.Close()
			t.Fatalf("close stream missing: %s (%s)", response.Status, response.Header.Get("Content-Type"))
		}
		t.Cleanup(func() { response.Body.Close() })
		return response
	}
	response := connect()
	defer response.Body.Close()
	stream := bufio.NewReader(response.Body)
	for i := 0; i < 2; i++ {
		// A second native close can arrive after the player cancels the first.
		if !bridge.BeforeClose(b) || requests.Load() != 0 {
			t.Fatal("native close bypassed the renderer's draft check")
		}
		for _, want := range []string{"event: close-request\n", "data: {}\n", "\n"} {
			line, err := stream.ReadString('\n')
			if err != nil || line != want {
				t.Fatalf("read %q, want %q: %v", line, want, err)
			}
		}
	}

	close(b.Done)
	if bridge.BeforeClose(b) {
		t.Fatal("the host must allow quitting after the backend exits")
	}
}
