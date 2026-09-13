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

	"github.com/DerrickGold/ar-recomp/installer/internal/workshopui"
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
	stopOnce   sync.Once
}

// StartBundledBackend runs tools directly from the verified payload, with a
// separate writable workspace for logs and derived build products.
func StartBundledBackend(ctx context.Context, workspace, payload, inputID, outputDir string, jobs int, artifactDirectory ...string) (*Backend, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	root := filepath.Join(payload, "utils")
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
	ready := filepath.Join(private, "ready.json")
	// ctx cancels startup only. After readiness, Stop owns the backend lifetime.
	// CommandContext would kill it as soon as OnShutdown cancels initialization,
	// before the backend can drain requests and stop its own helper processes.
	b.command = exec.Command(filepath.Join(root, "tools", executableName("actraiser-builder", runtime.GOOS)), "gui",
		"--root", root, "--output-dir", outputDir, "--standalone-output", "--snesbuild", filepath.Join(root, "tools", executableName("snesbuild", runtime.GOOS)), "--no-open", "--ready-file", ready, "--allow-stubs")
	configureCommand(b.command)
	if inputID != "" {
		b.command.Args = append(b.command.Args, "--build-workspace", workspace, "--input-id", inputID)
	}
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
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	closeTree, err := startBackendCommand(b.command)
	if err != nil {
		return nil, err
	}
	go func() {
		err := b.command.Wait()
		closeTree()
		b.mu.Lock()
		b.err = err
		b.mu.Unlock()
		close(b.Done)
	}()
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
	if b.exited() {
		return nil
	}
	if b.URL == nil {
		return errors.New("Builder has not published its session address yet")
	}
	transport := &http.Transport{Proxy: nil}
	defer transport.CloseIdleConnections()
	client := &http.Client{Timeout: 3 * time.Second, Transport: transport}
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
	b.stopOnce.Do(b.stop)
}

func (b *Backend) exited() bool {
	select {
	case <-b.Done:
		return true
	default:
		return false
	}
}

func (b *Backend) stop() {
	if b.command != nil && b.command.Process != nil {
		select {
		case <-b.Done:
		default:
			if b.URL != nil {
				_ = b.RequestClose()
				// The backend gives active HTTP handlers five seconds to drain.
				// Allow that grace period before using the last-resort kill.
				select {
				case <-b.Done:
				case <-time.After(7 * time.Second):
					_ = b.command.Process.Kill()
				}
			} else {
				// Startup failed/cancelled before a close endpoint was available.
				_ = b.command.Process.Kill()
			}
			select {
			case <-b.Done:
			case <-time.After(2 * time.Second):
				fmt.Fprintln(os.Stderr, "Builder shutdown: timed out waiting for the terminated backend")
			}
		}
	}
	if b.log != nil {
		_ = b.log.Close()
	}
	if b.sessionDir != "" {
		_ = os.RemoveAll(b.sessionDir)
	} // Exact private directory made by StartBundledBackend.
}

// Bridge keeps the renderer on its app origin. It can only reach the single
// authenticated loopback backend chosen at startup; it is not a generic proxy.
type Bridge struct {
	mu          sync.RWMutex
	proxy       *httputil.ReverseProxy
	message     string
	failed      bool
	rendererURL string
	output      *OutputSelection
}

func (h *Bridge) SetOutputSelection(selection *OutputSelection) {
	h.mu.Lock()
	h.output = selection
	h.mu.Unlock()
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
	proxy, message, failed, rendererURL, output := h.proxy, h.message, h.failed, h.rendererURL, h.output
	h.mu.RUnlock()
	if r.Method == "GET" && (r.URL.Path == "/__shell/feedback.js" || r.URL.Path == "/__shell/feedback.css") {
		name := strings.TrimPrefix(r.URL.Path, "/__shell/")
		data, _ := workshopui.Assets.ReadFile(name)
		w.Header().Set("Content-Type", "text/javascript; charset=utf-8")
		if strings.HasSuffix(name, ".css") {
			w.Header().Set("Content-Type", "text/css; charset=utf-8")
		}
		w.Write(data)
		return
	}
	if strings.HasPrefix(r.URL.Path, "/__shell/output/") {
		if !allowProxy || output == nil {
			http.NotFound(w, r)
			return
		}
		output.ServeHTTP(w, r)
		return
	}
	if r.URL.Path == "/__shell/status" {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_ = json.NewEncoder(w).Encode(map[string]any{"ready": proxy != nil, "message": message, "failed": failed, "url": rendererURL, "chooseOutput": output != nil && output.NeedsChoice()})
		return
	}
	if r.URL.Path == "/__shell/start.js" {
		w.Header().Set("Content-Type", "text/javascript")
		io.WriteString(w, `let failures=0;async function poll(){const status=document.getElementById('status');try{const s=await(await fetch('__shell/status',{cache:'no-store'})).json();failures=0;window.workshopFeedback?.clear(status);status.textContent=s.message||'Starting…';if(s.url&&!location.href.startsWith(s.url)){location.replace(s.url);return;}if(s.chooseOutput){location.replace('__shell/output/');return;}if(s.ready){if(s.url)location.replace(s.url);else location.reload();return;}if(s.failed){status.textContent='The Builder could not start.';window.workshopFeedback?.show(status,new Error(s.message),{operation:'Builder startup'});return;}}catch(error){if(++failures>=3){status.textContent='The Builder is not responding.';window.workshopFeedback?.show(status,error,{operation:'Builder startup',retry:()=>{failures=0;return poll();}});return;}}setTimeout(poll,300);}poll();`)
		return
	}
	if proxy != nil && allowProxy {
		proxy.ServeHTTP(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	io.WriteString(w, `<!doctype html><html><head><meta charset="utf-8"><title>ActRaiser Recomp Builder</title><link rel="stylesheet" href="__shell/feedback.css"></head><body style="background:#181b24;color:#eef0f6;font:18px system-ui;padding:32px;max-width:850px;margin:auto"><h1>ActRaiser Recomp Builder</h1><p id="status" role="status">Preparing your workspace…</p><script src="__shell/feedback.js"></script><script src="__shell/start.js"></script></body></html>`)
}
