package host

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

var tokenPath = regexp.MustCompile(`^/[0-9a-f]{36}/$`)

type Backend struct {
	URL        *url.URL
	Done       chan struct{}
	command    *exec.Cmd
	sessionDir string
	log        *os.File
	mu         sync.Mutex
	err        error
}

func StartBackend(ctx context.Context, workspace, outputDir string, jobs int, artifactDirectory ...string) (*Backend, error) {
	private, err := os.MkdirTemp("", "actraiser-builder-session-")
	if err != nil {
		return nil, err
	}
	b := &Backend{Done: make(chan struct{}), sessionDir: private}
	failed := true
	defer func() {
		if failed {
			b.Stop()
		}
	}()
	if err = os.MkdirAll(filepath.Join(workspace, "logs"), 0700); err != nil {
		return nil, err
	}
	b.log, err = os.CreateTemp(filepath.Join(workspace, "logs"), "builder-*.log")
	if err != nil {
		return nil, err
	}
	root := filepath.Join(workspace, "utils")
	ready := filepath.Join(private, "ready.json")
	b.command = exec.CommandContext(ctx, filepath.Join(root, "tools", executableName("actraiser-builder", runtime.GOOS)), "gui",
		"--root", root, "--output-dir", outputDir, "--standalone-output", "--snesbuild", filepath.Join(root, "tools", executableName("snesbuild", runtime.GOOS)), "--no-open", "--ready-file", ready, "--allow-stubs")
	configureCommand(b.command)
	if len(artifactDirectory) != 0 {
		b.command.Args = append(b.command.Args, "--import-search-dir", artifactDirectory[0])
	}
	if jobs > 0 {
		b.command.Args = append(b.command.Args, "--jobs", strconv.Itoa(jobs))
	}
	b.command.Dir = workspace
	b.command.Stdout = b.log
	b.command.Stderr = b.log
	b.command.WaitDelay = 2 * time.Second
	// GUI processes from an AppImage inherit webview library overrides. Do not
	// leak those into the headless build driver or the generated game's loader.
	b.command.Env = backendEnvironment(os.Environ())
	if err = b.command.Start(); err != nil {
		return nil, err
	}
	go func() { err := b.command.Wait(); b.mu.Lock(); b.err = err; b.mu.Unlock(); close(b.Done) }()
	ticker := time.NewTicker(25 * time.Millisecond)
	defer ticker.Stop()
	deadline := time.NewTimer(30 * time.Second)
	defer deadline.Stop()
	for {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-deadline.C:
			return nil, fmt.Errorf("Builder startup timed out; see %s", b.log.Name())
		case <-b.Done:
			return nil, fmt.Errorf("Builder stopped during startup; see %s", b.log.Name())
		case <-ticker.C:
			data, err := os.ReadFile(ready)
			if os.IsNotExist(err) {
				continue
			}
			if err != nil {
				return nil, err
			}
			var descriptor struct {
				Schema int    `json:"schema"`
				URL    string `json:"url"`
			}
			if json.Unmarshal(data, &descriptor) != nil {
				continue
			} // Small write may still be finishing.
			b.URL, err = ValidateAddress(descriptor.Schema, descriptor.URL)
			if err != nil {
				return nil, err
			}
			failed = false
			return b, nil
		}
	}
}

func backendEnvironment(environment []string) []string {
	var result []string
	var savedDataDirs, dataDirsMode string
	for _, entry := range environment {
		key, value, _ := strings.Cut(entry, "=")
		if key == "AR_BUILDER_HOST_XDG_DATA_DIRS" {
			savedDataDirs = value
		}
		if key == "AR_BUILDER_HOST_XDG_DATA_DIRS_MODE" {
			dataDirsMode = value
		}
	}
	for _, entry := range environment {
		key, _, _ := strings.Cut(entry, "=")
		if strings.HasPrefix(strings.ToUpper(key), "WEBVIEW2_") {
			continue
		}
		switch key {
		case "APPIMAGE", "APPDIR", "OWD", "LD_LIBRARY_PATH", "LD_PRELOAD", "GIO_EXTRA_MODULES", "GIO_MODULE_DIR", "GSETTINGS_SCHEMA_DIR", "GSETTINGS_BACKEND", "GTK_PATH", "GTK_IM_MODULE_FILE", "GDK_PIXBUF_MODULE_FILE", "GDK_PIXBUF_MODULEDIR", "WEBKIT_EXEC_PATH", "WEBKIT_INJECTED_BUNDLE_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0", "GST_PLUGIN_PATH_1_0", "GST_PLUGIN_SCANNER", "GST_REGISTRY_1_0", "AR_BUILDER_HOST_XDG_DATA_DIRS", "AR_BUILDER_HOST_XDG_DATA_DIRS_MODE":
			continue
		}
		if key == "XDG_DATA_DIRS" && dataDirsMode != "" {
			continue
		}
		result = append(result, entry)
	}
	if dataDirsMode == "set" {
		result = append(result, "XDG_DATA_DIRS="+savedDataDirs)
	}
	return result
}

func ValidateAddress(schema int, address string) (*url.URL, error) {
	u, err := url.Parse(address)
	if err != nil || schema != 1 || u.Scheme != "http" || u.Hostname() != "127.0.0.1" || u.Port() == "" || u.User != nil || u.RawQuery != "" || u.Fragment != "" || !tokenPath.MatchString(u.Path) || u.RawPath != "" {
		return nil, errors.New("invalid local Builder session descriptor")
	}
	return u, nil
}

func (b *Backend) RequestClose() error {
	select {
	case <-b.Done:
		return nil
	default:
	}
	client := &http.Client{Timeout: 3 * time.Second, Transport: &http.Transport{Proxy: nil}}
	response, err := client.Post(b.URL.String()+"close", "application/json", nil)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		data, _ := io.ReadAll(io.LimitReader(response.Body, 4096))
		return fmt.Errorf("Builder cannot close yet: %s", data)
	}
	return nil
}

func (b *Backend) Stop() {
	if b.command != nil && b.command.Process != nil {
		select {
		case <-b.Done:
		default:
			if b.URL != nil {
				_ = b.RequestClose()
			}
			select {
			case <-b.Done:
			case <-time.After(3 * time.Second):
				_ = b.command.Process.Kill()
				<-b.Done
			}
		}
	}
	if b.log != nil {
		_ = b.log.Close()
	}
	if b.sessionDir != "" {
		_ = os.RemoveAll(b.sessionDir)
	} // Exact private directory made by StartBackend.
}

// Bridge keeps the renderer on its app origin. It can only reach the single
// authenticated loopback backend chosen at startup; it is not a generic proxy.
type Bridge struct {
	mu          sync.RWMutex
	proxy       *httputil.ReverseProxy
	message     string
	failed      bool
	rendererURL string
}

func (h *Bridge) SetRendererURL(address string) { h.mu.Lock(); h.rendererURL = address; h.mu.Unlock() }

func (h *Bridge) Progress(message string) {
	fmt.Fprintln(os.Stderr, "Builder startup:", message)
	h.mu.Lock()
	h.message = message
	h.mu.Unlock()
}
func (h *Bridge) Fail(err error) {
	fmt.Fprintln(os.Stderr, "Builder startup failed:", err)
	h.mu.Lock()
	h.message = err.Error()
	h.failed = true
	h.mu.Unlock()
}
func (h *Bridge) Connect(address *url.URL) {
	p := httputil.NewSingleHostReverseProxy(address)
	p.Transport = &http.Transport{Proxy: nil}
	p.ErrorHandler = func(w http.ResponseWriter, r *http.Request, err error) {
		http.Error(w, "The local Builder stopped. Close and reopen the application.", http.StatusBadGateway)
	}
	h.mu.Lock()
	h.proxy = p
	h.mu.Unlock()
}
func (h *Bridge) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	h.serveHTTP(w, r, true)
}

// Bootstrap never serves the Workshop on the native custom protocol, even if
// a warm-start backend becomes ready before the first navigation.
func (h *Bridge) Bootstrap(w http.ResponseWriter, r *http.Request) {
	h.serveHTTP(w, r, false)
}

func (h *Bridge) serveHTTP(w http.ResponseWriter, r *http.Request, allowProxy bool) {
	h.mu.RLock()
	proxy, message, failed, rendererURL := h.proxy, h.message, h.failed, h.rendererURL
	h.mu.RUnlock()
	if r.URL.Path == "/__shell/status" {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_ = json.NewEncoder(w).Encode(map[string]any{"ready": proxy != nil, "message": message, "failed": failed, "url": rendererURL})
		return
	}
	if r.URL.Path == "/__shell/start.js" {
		w.Header().Set("Content-Type", "text/javascript")
		io.WriteString(w, `async function poll(){try{const s=await(await fetch('__shell/status',{cache:'no-store'})).json();document.getElementById('status').textContent=s.message||'Starting…';if(s.ready){if(s.url)location.replace(s.url);else location.reload();return;}if(s.failed)return;}catch(e){}setTimeout(poll,300);}poll();`)
		return
	}
	if proxy != nil && allowProxy {
		proxy.ServeHTTP(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	io.WriteString(w, `<!doctype html><html><head><meta charset="utf-8"><title>ActRaiser Recomp Builder</title></head><body style="background:#181b24;color:#eef0f6;font:18px system-ui;padding:48px"><h1>ActRaiser Recomp Builder</h1><p id="status">Preparing your workspace…</p><script src="__shell/start.js"></script></body></html>`)
}
