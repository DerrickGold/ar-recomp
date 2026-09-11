package host

import (
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

func TestRendererRequiresPrivatePrefixAndPreservesHTTPBodies(t *testing.T) {
	address, stop, err := StartRenderer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/upload" || r.Method != "POST" || r.URL.Query().Get("id") != "sample" {
			t.Errorf("unexpected request: %s %s", r.Method, r.URL)
		}
		body, _ := io.ReadAll(r.Body)
		w.Write(body)
	}))
	if err != nil {
		t.Fatal(err)
	}
	defer stop()
	u, err := ValidateAddress(1, address)
	if err != nil {
		t.Fatal(err)
	}
	client := &http.Client{Timeout: time.Second, Transport: &http.Transport{Proxy: nil}}
	response, err := client.Post(address+"upload?id=sample", "application/octet-stream", strings.NewReader("binary\x00body"))
	if err != nil {
		t.Fatal(err)
	}
	data, _ := io.ReadAll(response.Body)
	response.Body.Close()
	if response.StatusCode != 200 || string(data) != "binary\x00body" {
		t.Fatalf("%d: %q", response.StatusCode, data)
	}
	for _, path := range []string{"/", "/upload", strings.TrimSuffix(u.Path, "/") + "other/upload"} {
		response, err = client.Get("http://" + u.Host + path)
		if err != nil {
			t.Fatal(err)
		}
		response.Body.Close()
		if response.StatusCode != 404 {
			t.Fatalf("unguarded route %s: %d", path, response.StatusCode)
		}
	}
}

func TestWarmNativeBootstrapNeverServesWorkshopOnCustomScheme(t *testing.T) {
	bridge := &Bridge{}
	backend, _ := url.Parse("http://127.0.0.1:1/" + strings.Repeat("a", 36) + "/")
	bridge.Connect(backend)
	bridge.SetRendererURL("http://127.0.0.1:2/" + strings.Repeat("b", 36) + "/")
	w := httptest.NewRecorder()
	bridge.Bootstrap(w, httptest.NewRequest("GET", "wails://wails/", nil))
	if w.Code != 200 || !strings.Contains(w.Body.String(), "__shell/start.js") {
		t.Fatal(w.Body.String())
	}
	w = httptest.NewRecorder()
	bridge.Bootstrap(w, httptest.NewRequest("GET", "wails://wails/__shell/start.js", nil))
	if !strings.Contains(w.Body.String(), "location.replace(s.url)") {
		t.Fatal(w.Body.String())
	}
}
