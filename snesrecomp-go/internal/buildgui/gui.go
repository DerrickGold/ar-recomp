// Package buildgui serves the dependency-free local browser interface used by
// snesbuild. The server is deliberately loopback-only and hides every endpoint
// behind an unguessable per-process path token.
package buildgui

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"html"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"
)

const (
	maxROMBytes = 16 << 20
	maxLogBytes = 2 << 20
)

// Result describes the playable artifact produced by a successful build.
type Result struct {
	Message    string `json:"message"`
	OutputPath string `json:"outputPath"`
}

// Options configures one local GUI session.
type Options struct {
	Title        string
	ProjectRoot  string
	OpenBrowser  bool
	Stdout       io.Writer
	Build        func(context.Context, string, io.Writer) (Result, error)
	Launch       func(Result) error
	openURL      func(string) error
	sessionToken string
}

// Run starts a loopback-only server, opens the system browser when requested,
// and blocks until the user closes the builder or the context is cancelled.
func Run(ctx context.Context, options Options) error {
	if options.Build == nil {
		return errors.New("builder GUI requires a build function")
	}
	root, err := filepath.Abs(options.ProjectRoot)
	if err != nil {
		return fmt.Errorf("resolve GUI project root: %w", err)
	}
	if info, statErr := os.Stat(root); statErr != nil || !info.IsDir() {
		if statErr == nil {
			statErr = errors.New("not a directory")
		}
		return fmt.Errorf("GUI project root %s is unavailable: %w", root, statErr)
	}
	options.ProjectRoot = root
	if options.Title == "" {
		options.Title = "snesbuild"
	}
	if options.Stdout == nil {
		options.Stdout = io.Discard
	}
	if options.openURL == nil {
		options.openURL = openBrowser
	}
	token := options.sessionToken
	if token == "" {
		token, err = randomToken()
		if err != nil {
			return fmt.Errorf("create GUI session token: %w", err)
		}
	}

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return fmt.Errorf("start builder GUI: %w", err)
	}
	defer listener.Close()

	sessionCtx, cancelSession := context.WithCancel(ctx)
	defer cancelSession()
	app := newApplication(sessionCtx, options, token)
	server := &http.Server{
		Handler:           app,
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       2 * time.Minute,
		IdleTimeout:       30 * time.Second,
	}
	address := "http://" + listener.Addr().String() + "/" + token + "/"
	fmt.Fprintf(options.Stdout, "builder GUI: %s\n", address)
	if options.OpenBrowser {
		if openErr := options.openURL(address); openErr != nil {
			fmt.Fprintf(options.Stdout,
				"builder GUI: could not open a browser (%v); open the URL above manually\n",
				openErr)
		}
	}

	serveError := make(chan error, 1)
	go func() {
		serveError <- server.Serve(listener)
	}()

	var runError error
	select {
	case <-ctx.Done():
		runError = ctx.Err()
	case <-app.closed:
	case err := <-serveError:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			runError = err
		}
	}
	cancelSession()
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer shutdownCancel()
	if err := server.Shutdown(shutdownCtx); err != nil && runError == nil {
		runError = err
	}
	return runError
}

func randomToken() (string, error) {
	data := make([]byte, 18)
	if _, err := rand.Read(data); err != nil {
		return "", err
	}
	return hex.EncodeToString(data), nil
}

func openBrowser(url string) error {
	var command *exec.Cmd
	switch runtime.GOOS {
	case "darwin":
		command = exec.Command("open", url)
	case "windows":
		command = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	default:
		command = exec.Command("xdg-open", url)
	}
	return command.Start()
}

type status struct {
	State      string   `json:"state"`
	Log        string   `json:"log"`
	Error      string   `json:"error,omitempty"`
	Message    string   `json:"message,omitempty"`
	OutputPath string   `json:"outputPath,omitempty"`
	Progress   progress `json:"progress"`
}

type application struct {
	ctx     context.Context
	options Options
	prefix  string
	closed  chan struct{}

	mu           sync.Mutex
	state        string
	log          bytes.Buffer
	errorMessage string
	result       Result
	closeOnce    sync.Once
}

func newApplication(ctx context.Context, options Options, token string) *application {
	return &application{
		ctx: ctx, options: options, prefix: "/" + token + "/",
		closed: make(chan struct{}), state: "idle",
	}
}

func (app *application) ServeHTTP(response http.ResponseWriter, request *http.Request) {
	if !strings.HasPrefix(request.URL.Path, app.prefix) {
		http.NotFound(response, request)
		return
	}
	endpoint := strings.TrimPrefix(request.URL.Path, app.prefix)
	switch {
	case endpoint == "" && request.Method == http.MethodGet:
		response.Header().Set("Content-Type", "text/html; charset=utf-8")
		// The themed page draws its art with inline CSS gradients and inline
		// SVG (data: URLs), so no bitmap ever ships or loads: `img-src` stays
		// as tight as the artwork allows and no remote origin is reachable.
		response.Header().Set("Content-Security-Policy",
			"default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "+
				"connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'")
		response.Header().Set("Cache-Control", "no-store")
		response.Header().Set("Referrer-Policy", "no-referrer")
		response.Header().Set("X-Content-Type-Options", "nosniff")
		page := strings.Replace(pageHTML, "{{TITLE}}",
			html.EscapeString(app.options.Title), 1)
		page = strings.Replace(page, "{{STEPS}}", renderStepList(), 1)
		_, _ = io.WriteString(response, page)
	case endpoint == "status" && request.Method == http.MethodGet:
		app.writeStatus(response)
	case endpoint == "build" && request.Method == http.MethodPost:
		app.startBuild(response, request)
	case endpoint == "launch" && request.Method == http.MethodPost:
		app.launch(response)
	case endpoint == "close" && request.Method == http.MethodPost:
		app.close(response)
	default:
		http.NotFound(response, request)
	}
}

func (app *application) writeStatus(response http.ResponseWriter) {
	app.mu.Lock()
	current := status{
		State: app.state, Log: app.log.String(), Error: app.errorMessage,
		Message: app.result.Message, OutputPath: app.result.OutputPath,
	}
	app.mu.Unlock()
	// Derived outside the lock: computeProgress is pure and can be slow
	// relative to a mutex hold (it scans the whole log tail on every poll).
	current.Progress = computeProgress(current.Log, current.State)
	writeJSON(response, http.StatusOK, current)
}

func (app *application) startBuild(response http.ResponseWriter, request *http.Request) {
	app.mu.Lock()
	if app.state == "building" {
		app.mu.Unlock()
		writeJSONError(response, http.StatusConflict, "a build is already running")
		return
	}
	app.state = "building"
	app.log.Reset()
	app.errorMessage = ""
	app.result = Result{}
	app.mu.Unlock()

	request.Body = http.MaxBytesReader(response, request.Body, maxROMBytes+(1<<20))
	if err := request.ParseMultipartForm(maxROMBytes + (1 << 20)); err != nil {
		app.rejectBuild(response, "could not read the selected ROM")
		return
	}
	input, header, err := request.FormFile("rom")
	if err != nil {
		app.rejectBuild(response, "select a .sfc or .smc ROM")
		return
	}
	defer input.Close()
	if !validROMName(header.Filename) {
		app.rejectBuild(response, "the selected file must end in .sfc or .smc")
		return
	}
	romPath, err := storeROM(app.options.ProjectRoot, input)
	if err != nil {
		app.rejectBuild(response, err.Error())
		return
	}

	writeJSON(response, http.StatusAccepted, map[string]string{"state": "building"})

	go func() {
		result, buildErr := app.options.Build(app.ctx, romPath, &lockedLogWriter{app: app})
		app.mu.Lock()
		defer app.mu.Unlock()
		if buildErr != nil {
			app.state = "failed"
			app.errorMessage = buildErr.Error()
			return
		}
		app.state = "succeeded"
		app.result = result
	}()
}

func (app *application) rejectBuild(response http.ResponseWriter, message string) {
	app.mu.Lock()
	app.state = "idle"
	app.mu.Unlock()
	writeJSONError(response, http.StatusBadRequest, message)
}

func validROMName(name string) bool {
	extension := strings.ToLower(filepath.Ext(filepath.Base(name)))
	return extension == ".sfc" || extension == ".smc"
}

func storeROM(root string, source io.Reader) (destination string, resultErr error) {
	temporary, err := os.CreateTemp(root, ".snesbuild-rom-*")
	if err != nil {
		return "", fmt.Errorf("create local ROM copy: %w", err)
	}
	temporaryPath := temporary.Name()
	defer func() {
		_ = temporary.Close()
		if resultErr != nil {
			_ = os.Remove(temporaryPath)
		}
	}()

	written, err := io.Copy(temporary, io.LimitReader(source, maxROMBytes+1))
	if err != nil {
		return "", fmt.Errorf("copy selected ROM: %w", err)
	}
	if written == 0 {
		return "", errors.New("the selected ROM is empty")
	}
	if written > maxROMBytes {
		return "", fmt.Errorf("the selected ROM exceeds the %d MiB safety limit", maxROMBytes>>20)
	}
	if err := temporary.Sync(); err != nil {
		return "", fmt.Errorf("flush local ROM copy: %w", err)
	}
	if err := temporary.Close(); err != nil {
		return "", fmt.Errorf("close local ROM copy: %w", err)
	}

	destination = filepath.Join(root, "user-rom.sfc")
	if err := os.Rename(temporaryPath, destination); err != nil {
		// Windows cannot atomically replace an existing file. This path is
		// exclusively managed by the GUI, so removing its previous copy is
		// safe before the second rename.
		if removeErr := os.Remove(destination); removeErr != nil && !os.IsNotExist(removeErr) {
			return "", fmt.Errorf("replace local ROM copy: %w", err)
		}
		if err := os.Rename(temporaryPath, destination); err != nil {
			return "", fmt.Errorf("store local ROM copy: %w", err)
		}
	}
	if err := os.Chmod(destination, 0o600); err != nil {
		return "", fmt.Errorf("protect local ROM copy: %w", err)
	}
	return destination, nil
}

type lockedLogWriter struct {
	app *application
}

func (writer *lockedLogWriter) Write(data []byte) (int, error) {
	originalLength := len(data)
	if len(data) > maxLogBytes {
		data = data[len(data)-maxLogBytes:]
	}
	app := writer.app
	app.mu.Lock()
	defer app.mu.Unlock()
	if app.log.Len()+len(data) > maxLogBytes {
		excess := app.log.Len() + len(data) - maxLogBytes
		existing := app.log.Bytes()
		if excess >= len(existing) {
			app.log.Reset()
		} else {
			kept := append([]byte(nil), existing[excess:]...)
			app.log.Reset()
			_, _ = app.log.Write(kept)
		}
	}
	_, _ = app.log.Write(data)
	return originalLength, nil
}

func (app *application) launch(response http.ResponseWriter) {
	app.mu.Lock()
	result, state := app.result, app.state
	app.mu.Unlock()
	if state != "succeeded" {
		writeJSONError(response, http.StatusConflict, "finish a successful build first")
		return
	}
	if app.options.Launch == nil {
		writeJSONError(response, http.StatusNotImplemented, "launching is unavailable on this host")
		return
	}
	if err := app.options.Launch(result); err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(response, http.StatusOK, map[string]string{"state": "launched"})
}

func (app *application) close(response http.ResponseWriter) {
	app.mu.Lock()
	building := app.state == "building"
	app.mu.Unlock()
	if building {
		writeJSONError(response, http.StatusConflict, "wait for the current build to finish")
		return
	}
	app.closeOnce.Do(func() { close(app.closed) })
	writeJSON(response, http.StatusOK, map[string]string{"state": "closed"})
}

func writeJSON(response http.ResponseWriter, code int, value any) {
	response.Header().Set("Content-Type", "application/json")
	response.Header().Set("Cache-Control", "no-store")
	response.WriteHeader(code)
	_ = json.NewEncoder(response).Encode(value)
}

func writeJSONError(response http.ResponseWriter, code int, message string) {
	writeJSON(response, code, map[string]string{"error": message})
}

const pageHTML = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{TITLE}}</title>
<style>
/* The look borrows from the game's own presentation: a high, pale sky over a
 * hazy horizon (the Sky Palace's outlook), the Palace's fluted marble columns
 * framing the content, and the manual/box palette of warm gold on deep blue.
 * Every visual is CSS or inline SVG -- no bitmap ships in the release bundle,
 * which deliberately carries no media (see packaging/CMakeLists.txt). */
:root {
  color-scheme: dark;
  --ink:#f6f1e3; --muted:#b9b39c; --gold:#e0b95f; --gold-deep:#a97f2c;
  --marble:#e9e3d2; --marble-shade:#a89d84; --marble-dark:#6d6552;
  --sky-high:#1d3f78; --sky-mid:#4b7bb5; --sky-low:#b8d4e6; --sky-haze:#e9d9b6;
  --panel:#10192bd9; --line:#31456b;
  --ok:#8fd6a6; --bad:#f0938a;
}
* { box-sizing:border-box; }
html,body { height:100%; }
body {
  margin:0; color:var(--ink);
  font:16px/1.55 "Iowan Old Style",Palatino,Georgia,serif;
  /* Sky: deep at the zenith, warm haze at the horizon. */
  background:
    linear-gradient(180deg,
      var(--sky-high) 0%, #2b5590 26%, var(--sky-mid) 52%,
      var(--sky-low) 74%, var(--sky-haze) 92%, #cbb388 100%);
  background-color:var(--sky-high);
  background-attachment:fixed;
}
/* Clouds: layered soft radial gradients that drift very slowly. Two bands at
 * different speeds/scales give parallax without any image or canvas. */
.sky { position:fixed; inset:0; z-index:0; overflow:hidden; pointer-events:none; }
.clouds {
  position:absolute; left:-50%; width:200%; height:100%;
  background-repeat:repeat-x; opacity:.85;
}
.clouds.far {
  top:8%; height:42%;
  background-image:
    radial-gradient(60px 22px at 12% 60%, #ffffffcc 0, #ffffff00 70%),
    radial-gradient(90px 30px at 26% 48%, #ffffffd9 0, #ffffff00 72%),
    radial-gradient(52px 18px at 38% 66%, #ffffffbf 0, #ffffff00 70%),
    radial-gradient(110px 34px at 58% 52%, #ffffffe6 0, #ffffff00 74%),
    radial-gradient(70px 24px at 78% 62%, #ffffffcc 0, #ffffff00 70%),
    radial-gradient(96px 30px at 92% 50%, #ffffffd9 0, #ffffff00 72%);
  background-size:1400px 100%;
  animation:drift 210s linear infinite;
}
.clouds.near {
  top:34%; height:44%; opacity:.7;
  background-image:
    radial-gradient(150px 40px at 18% 56%, #fffffff2 0, #ffffff00 76%),
    radial-gradient(120px 32px at 46% 66%, #ffffffe6 0, #ffffff00 74%),
    radial-gradient(180px 46px at 72% 54%, #fffffff2 0, #ffffff00 78%),
    radial-gradient(130px 34px at 96% 64%, #ffffffe6 0, #ffffff00 74%);
  background-size:1900px 100%;
  animation:drift 130s linear infinite reverse;
}
@keyframes drift { to { transform:translateX(-1400px); } }
@media (prefers-reduced-motion:reduce) {
  .clouds { animation:none; }
  #state[data-kind=building]::before { animation:none; }
}

/* Sky Palace columns frame the page. Fluted shaft via repeating-linear-
 * gradient, with a capital and base block. Hidden on narrow viewports where
 * they would crowd the content instead of framing it. */
.colonnade { position:fixed; top:0; bottom:0; width:74px; z-index:1; pointer-events:none; }
.colonnade.left { left:max(0px,calc(50% - 560px)); }
.colonnade.right { right:max(0px,calc(50% - 560px)); }
.shaft {
  position:absolute; top:52px; bottom:52px; left:9px; right:9px;
  background:
    repeating-linear-gradient(90deg,
      var(--marble-shade) 0 1px, var(--marble) 1px 7px,
      #fffdf6 7px 9px, var(--marble) 9px 15px, var(--marble-shade) 15px 16px);
  box-shadow:inset 0 0 18px #6d655233, 0 0 26px #0a132480;
}
.cap, .base {
  position:absolute; left:0; right:0; height:26px;
  background:linear-gradient(180deg,#fffdf7,var(--marble) 55%,var(--marble-shade));
  box-shadow:0 2px 10px #0a132466;
}
.cap { top:26px; }
.cap::before {
  content:""; position:absolute; left:-5px; right:-5px; top:-26px; height:26px;
  background:linear-gradient(180deg,var(--marble),#fffdf7 40%,var(--marble-shade));
  border-radius:3px 3px 0 0;
}
.base { bottom:26px; }
.base::after {
  content:""; position:absolute; left:-5px; right:-5px; bottom:-26px; height:26px;
  background:linear-gradient(180deg,var(--marble-shade),var(--marble) 60%,var(--marble-dark));
  border-radius:0 0 3px 3px;
}
@media (max-width:1000px) { .colonnade { display:none; } }

main { position:relative; z-index:2; width:min(880px,calc(100% - 40px)); margin:0 auto; padding:44px 0 56px; }

.crest { display:flex; align-items:center; gap:16px; margin-bottom:6px; }
.crest svg { width:44px; height:44px; flex:none; filter:drop-shadow(0 2px 6px #0a132499); }
.eyebrow { color:#f4e6c2; text-transform:uppercase; letter-spacing:.2em; font-size:.7rem; font-weight:700;
  font-family:system-ui,-apple-system,sans-serif; text-shadow:0 1px 3px #0a1324cc; }
h1 {
  margin:.1em 0 0; font-size:clamp(2.1rem,6vw,3.6rem); font-weight:600; letter-spacing:.01em;
  color:#fff8e6; text-shadow:0 2px 0 var(--gold-deep), 0 3px 14px #0a132480;
}
.rule { height:3px; margin:14px 0 16px; border-radius:2px;
  background:linear-gradient(90deg,transparent,var(--gold) 12%,#fff2cd 50%,var(--gold) 88%,transparent); }
.lede { max-width:60ch; color:#e8e2cf; margin:0 0 26px; text-shadow:0 1px 4px #0a132466; }

.layout { display:grid; grid-template-columns:minmax(0,1fr) 224px; gap:22px; align-items:start; }
@media (max-width:760px) { .layout { grid-template-columns:minmax(0,1fr); } }

.panel {
  background:var(--panel); backdrop-filter:blur(7px);
  border:1px solid var(--line); border-top:2px solid var(--gold);
  border-radius:4px; padding:22px; box-shadow:0 20px 60px #05091580;
}

/* Box art: an original stylised plate in the game's cover idiom (a winged
 * figure against the sky over a temple silhouette), drawn as inline SVG so it
 * ships as text and scales cleanly. Not a reproduction of the retail cover. */
.boxart { display:flex; flex-direction:column; gap:9px; }
.boxart .frame {
  border:1px solid var(--gold-deep); border-radius:3px; overflow:hidden;
  box-shadow:0 12px 34px #05091599, inset 0 0 0 1px #ffe9b422; background:#0d1b34;
}
.boxart svg { display:block; width:100%; height:auto; }
.boxart figcaption { color:#efe7d0; font-size:.74rem; letter-spacing:.1em; text-transform:uppercase;
  font-family:system-ui,-apple-system,sans-serif; text-align:center; text-shadow:0 1px 3px #0a1324; }

label { display:block; font-weight:600; margin-bottom:8px; font-size:.95rem; letter-spacing:.02em; }
input[type=file] {
  width:100%; padding:13px; border:1px dashed #6f83ab; border-radius:3px;
  background:#0c142759; color:#ded8c4;
  font:14px/1.4 system-ui,-apple-system,sans-serif;
}
input[type=file]::file-selector-button {
  margin-right:12px; padding:7px 13px; border:1px solid var(--gold-deep); border-radius:2px;
  background:linear-gradient(180deg,#f0d795,var(--gold)); color:#3a2a08;
  font:inherit; font-weight:700; cursor:pointer;
}
.actions { display:flex; flex-wrap:wrap; gap:10px; margin-top:18px; }
button {
  border:1px solid var(--gold-deep); border-radius:3px; padding:11px 20px;
  background:linear-gradient(180deg,#f2db9d,var(--gold) 60%,#cfa544);
  color:#3a2a08; font:inherit; font-weight:700; letter-spacing:.02em; cursor:pointer;
  box-shadow:0 2px 0 #7d5c1e, 0 6px 16px #05091566;
}
button:hover:not(:disabled) { filter:brightness(1.07); }
button:active:not(:disabled) { transform:translateY(1px); box-shadow:0 1px 0 #7d5c1e; }
button.secondary {
  color:var(--ink); background:linear-gradient(180deg,#22304d,#16223a);
  border-color:#3d5480; box-shadow:0 2px 0 #0d1526;
}
button:disabled { opacity:.42; cursor:not-allowed; box-shadow:none; }

/* Progress: a determinate bar plus the step list, so the current stage and the
 * overall position are both visible without opening the console. */
.progress { margin:24px 0 4px; }
.progress-head { display:flex; justify-content:space-between; align-items:baseline; gap:12px; margin-bottom:8px; }
#phase { font-weight:600; letter-spacing:.01em; }
#pct { font-variant-numeric:tabular-nums; color:var(--gold); font-weight:700;
  font-family:system-ui,-apple-system,sans-serif; }
.track {
  height:15px; border:1px solid #40598c; border-radius:3px; overflow:hidden;
  background:linear-gradient(180deg,#091326,#0c1a33); box-shadow:inset 0 2px 6px #00000073;
}
#bar {
  height:100%; width:0; transition:width .45s cubic-bezier(.4,0,.2,1);
  background:linear-gradient(180deg,#ffeec0,var(--gold) 45%,var(--gold-deep));
  box-shadow:0 0 12px #e0b95f80;
}
#bar.indeterminate {
  width:100% !important;
  background:repeating-linear-gradient(115deg,var(--gold-deep) 0 12px,var(--gold) 12px 24px);
  background-size:200% 100%; animation:slide 1.1s linear infinite; opacity:.55;
}
@keyframes slide { to { background-position:-48px 0; } }
#detail { margin-top:7px; min-height:1.3em; color:var(--muted); font-size:.83rem;
  font-family:system-ui,-apple-system,sans-serif; }

.steps { list-style:none; margin:18px 0 0; padding:0; display:grid; gap:5px; }
.step { display:flex; align-items:center; gap:10px; font-size:.88rem; color:#8f9ab4;
  font-family:system-ui,-apple-system,sans-serif; transition:color .2s; }
.tick { width:16px; height:16px; flex:none; border:1px solid #45577d; border-radius:50%;
  position:relative; transition:all .2s; }
.step[data-state=done] { color:var(--ok); }
.step[data-state=done] .tick { border-color:var(--ok); background:#8fd6a626; }
.step[data-state=done] .tick::after {
  content:""; position:absolute; left:4px; top:1px; width:5px; height:9px;
  border-right:2px solid var(--ok); border-bottom:2px solid var(--ok); transform:rotate(38deg);
}
.step[data-state=active] { color:#fff5df; font-weight:600; }
.step[data-state=active] .tick { border-color:var(--gold); box-shadow:0 0 0 3px #e0b95f2e; }
.step[data-state=active] .tick::after {
  content:""; position:absolute; inset:3px; border-radius:50%; background:var(--gold);
  animation:pulse 1.3s ease-in-out infinite;
}
@keyframes pulse { 50% { opacity:.35; } }
.step[data-state=failed] { color:var(--bad); }
.step[data-state=failed] .tick { border-color:var(--bad); }

#state { display:flex; align-items:center; gap:9px; margin:20px 0 8px; color:#ded8c4;
  font-family:system-ui,-apple-system,sans-serif; font-size:.92rem; }
#state[data-kind=building]::before {
  content:""; width:13px; height:13px; border:2px solid #ffffff2e;
  border-top-color:var(--gold); border-radius:50%; animation:spin .8s linear infinite;
}
#state[data-kind=succeeded] { color:var(--ok); font-weight:600; }
#state[data-kind=failed] { color:var(--bad); font-weight:600; }
@keyframes spin { to { transform:rotate(360deg); } }

details { margin-top:14px; }
summary { cursor:pointer; color:var(--muted); font-size:.83rem; letter-spacing:.06em;
  text-transform:uppercase; font-family:system-ui,-apple-system,sans-serif; }
summary:hover { color:var(--gold); }
pre {
  margin:10px 0 0; min-height:150px; max-height:320px; overflow:auto;
  white-space:pre-wrap; word-break:break-word; padding:14px; border-radius:3px;
  background:#060b16; border:1px solid #26364f; color:#c3bda9;
  font:12px/1.55 ui-monospace,SFMono-Regular,Consolas,monospace;
}
.privacy { color:#cfc9b5; font-size:.8rem; margin-top:16px; line-height:1.5;
  font-family:system-ui,-apple-system,sans-serif; }
.sr { position:absolute; width:1px; height:1px; overflow:hidden; clip:rect(0 0 0 0); }
</style>
</head>
<body>
<div class="sky" aria-hidden="true"><div class="clouds far"></div><div class="clouds near"></div></div>
<div class="colonnade left" aria-hidden="true"><div class="cap"></div><div class="shaft"></div><div class="base"></div></div>
<div class="colonnade right" aria-hidden="true"><div class="cap"></div><div class="shaft"></div><div class="base"></div></div>

<main>
  <div class="crest">
    <svg viewBox="0 0 48 48" aria-hidden="true">
      <defs><linearGradient id="cg" x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="#fff3d0"/><stop offset="1" stop-color="#c9992f"/>
      </linearGradient></defs>
      <path fill="url(#cg)" d="M24 3l3.4 7.6L35 8.9l-2.2 7.3 6.6 3.6-5.6 5.2 4 6.6-7.6.3-1.2 7.5L24 35.6l-5 3.8-1.2-7.5-7.6-.3 4-6.6-5.6-5.2 6.6-3.6L13 8.9l7.6 1.7z"/>
      <circle cx="24" cy="22" r="5.2" fill="#0d1b34" opacity=".55"/>
    </svg>
    <div>
      <div class="eyebrow">Local &middot; Private &middot; Self-contained</div>
      <h1>Build your game</h1>
    </div>
  </div>
  <div class="rule"></div>
  <p class="lede">Choose your legally obtained ROM. Everything runs on this computer with the
  toolchain packaged beside this builder &mdash; nothing is uploaded.</p>

  <div class="layout">
    <section class="panel">
      <form id="build-form">
        <label for="rom">ActRaiser ROM (.sfc or .smc)</label>
        <input id="rom" name="rom" type="file" accept=".sfc,.smc" required>
        <div class="actions">
          <button id="build" type="submit">Build game</button>
          <button id="launch" class="secondary" type="button" disabled>Launch game</button>
          <button id="close" class="secondary" type="button">Close builder</button>
        </div>
      </form>

      <div class="progress">
        <div class="progress-head">
          <span id="phase">Waiting for your ROM</span>
          <span id="pct" aria-hidden="true">&mdash;</span>
        </div>
        <div class="track" role="progressbar" aria-labelledby="phase"
             aria-valuemin="0" aria-valuemax="100" id="track">
          <div id="bar"></div>
        </div>
        <div id="detail"></div>
        <ol class="steps" id="steps">{{STEPS}}</ol>
      </div>

      <div id="state" data-kind="idle">Ready to build</div>
      <details id="log-box">
        <summary>Build log</summary>
        <pre id="log">Select your ROM to begin.</pre>
      </details>
      <p class="privacy">Your ROM is copied only into this local folder and is never uploaded.
      This page talks only to the builder on 127.0.0.1.</p>
    </section>

    <figure class="boxart">
      <div class="frame">
        <svg viewBox="0 0 200 280" role="img" aria-labelledby="art-title">
          <title id="art-title">Stylised cover plate: a winged figure above a temple at dawn</title>
          <defs>
            <linearGradient id="bsky" x1="0" y1="0" x2="0" y2="1">
              <stop offset="0" stop-color="#13305f"/><stop offset=".45" stop-color="#3f6ea8"/>
              <stop offset=".78" stop-color="#c2d8e8"/><stop offset="1" stop-color="#efdcb2"/>
            </linearGradient>
            <linearGradient id="bstone" x1="0" y1="0" x2="0" y2="1">
              <stop offset="0" stop-color="#f3ecd9"/><stop offset="1" stop-color="#8d8368"/>
            </linearGradient>
            <linearGradient id="bwing" x1="0" y1="0" x2="1" y2="1">
              <stop offset="0" stop-color="#fff6dd"/><stop offset="1" stop-color="#d9b661"/>
            </linearGradient>
            <radialGradient id="bglow" cx=".5" cy=".42" r=".5">
              <stop offset="0" stop-color="#ffe9b8" stop-opacity=".95"/>
              <stop offset="1" stop-color="#ffe9b8" stop-opacity="0"/>
            </radialGradient>
          </defs>
          <rect width="200" height="280" fill="url(#bsky)"/>
          <circle cx="100" cy="118" r="62" fill="url(#bglow)"/>
          <g fill="#ffffff" opacity=".5">
            <ellipse cx="42" cy="74" rx="26" ry="7"/><ellipse cx="150" cy="58" rx="30" ry="8"/>
            <ellipse cx="168" cy="98" rx="20" ry="6"/><ellipse cx="30" cy="112" rx="18" ry="5"/>
          </g>
          <!-- temple silhouette on the horizon -->
          <g fill="#2c4670" opacity=".62">
            <rect x="58" y="196" width="84" height="10"/>
            <rect x="62" y="206" width="76" height="6"/>
            <g>
              <rect x="68" y="168" width="7" height="28"/><rect x="82" y="168" width="7" height="28"/>
              <rect x="96" y="168" width="7" height="28"/><rect x="110" y="168" width="7" height="28"/>
              <rect x="124" y="168" width="7" height="28"/>
            </g>
            <path d="M60 168h80l-40-22z"/>
          </g>
          <!-- winged figure -->
          <g transform="translate(100 132)">
            <path fill="url(#bwing)" d="M-6-10c-16-14-40-20-58-16 14 4 24 12 30 22-12-2-22 0-30 6 14 2 26 8 34 18 8-10 16-18 24-22z"/>
            <path fill="url(#bwing)" d="M6-10c16-14 40-20 58-16-14 4-24 12-30 22 12-2 22 0 30 6-14 2-26 8-34 18-8-10-16-18-24-22z"/>
            <path fill="url(#bstone)" d="M0-30c5 0 8 4 8 9s-3 9-8 9-8-4-8-9 3-9 8-9z"/>
            <path fill="url(#bstone)" d="M-9-9h18l5 40-9 30h-10l-9-30z"/>
            <rect x="-2" y="18" width="4" height="52" fill="#f6efdb"/>
            <rect x="-11" y="26" width="22" height="4" fill="#e6c97a"/>
          </g>
          <rect x="6" y="6" width="188" height="268" fill="none" stroke="#e0b95f" stroke-opacity=".55"/>
          <text x="100" y="252" text-anchor="middle" font-family="Georgia,serif" font-size="21"
                fill="#fff6e0" letter-spacing="1.5">ACTRAISER</text>
          <text x="100" y="266" text-anchor="middle" font-family="system-ui,sans-serif" font-size="7"
                fill="#f0e3c2" letter-spacing="3.4">RECOMPILED</text>
        </svg>
      </div>
      <figcaption>Static recompilation</figcaption>
    </figure>
  </div>
</main>

<script>
const form=document.querySelector("#build-form"), build=document.querySelector("#build"), launch=document.querySelector("#launch");
const state=document.querySelector("#state"), log=document.querySelector("#log"), closeButton=document.querySelector("#close");
const phase=document.querySelector("#phase"), pct=document.querySelector("#pct"), bar=document.querySelector("#bar");
const detail=document.querySelector("#detail"), track=document.querySelector("#track"), logBox=document.querySelector("#log-box");
const steps=[...document.querySelectorAll(".step")];
let polling=false;

function show(kind,text){ state.dataset.kind=kind; state.textContent=text; }

/* The phase list and percentage come from the server (buildgui/progress.go), so
 * the page never parses the build log itself -- one definition of the phase
 * model, and the bar cannot disagree with the step list. */
function paint(progress,kind){
  if(!progress) return;
  const done=new Set(progress.completed||[]);
  steps.forEach(step=>{
    const id=step.dataset.step;
    if(done.has(id)) step.dataset.state="done";
    else if(id===progress.phaseId) step.dataset.state=(kind==="failed"?"failed":"active");
    else step.removeAttribute("data-state");
  });
  phase.textContent=progress.phaseLabel||"";
  const percent=Math.max(0,Math.min(100,progress.percent|0));
  bar.style.width=percent+"%";
  pct.textContent=percent+"%";
  track.setAttribute("aria-valuenow",percent);
  /* Only the compile phase can measure itself; elsewhere the bar's width still
   * reflects completed phases, so keep it determinate and just say less. */
  detail.textContent=progress.detail||"";
  bar.classList.remove("indeterminate");
}

function resetProgress(){
  steps.forEach(step=>step.removeAttribute("data-state"));
  bar.style.width="0%"; bar.classList.remove("indeterminate");
  pct.textContent="—"; phase.textContent="Waiting for your ROM"; detail.textContent="";
  track.removeAttribute("aria-valuenow");
}

async function responseJSON(response){ const body=await response.json(); if(!response.ok) throw new Error(body.error||"Request failed"); return body; }

async function refresh(){
  try {
    const data=await responseJSON(await fetch("status",{cache:"no-store"}));
    if(data.log){ log.textContent=data.log; log.scrollTop=log.scrollHeight; }
    paint(data.progress,data.state);
    if(data.state==="building"){ show("building","Building — this can take a few minutes"); build.disabled=true; launch.disabled=true; }
    if(data.state==="succeeded"){ show("succeeded",data.message||"Build complete"); build.disabled=false; launch.disabled=false; polling=false; }
    if(data.state==="failed"){
      show("failed",data.error||"Build failed"); build.disabled=false; launch.disabled=true; polling=false;
      logBox.open=true;  /* a failure is the one time the log matters unprompted */
    }
  } catch(error) { show("failed",error.message); polling=false; }
  if(polling) setTimeout(refresh,500);
}

form.addEventListener("submit",async event=>{
  event.preventDefault();
  if(!document.querySelector("#rom").files.length) return;
  build.disabled=true; launch.disabled=true; log.textContent="Preparing local ROM copy…";
  resetProgress(); show("building","Starting build");
  phase.textContent="Preparing your ROM"; bar.classList.add("indeterminate");
  try { await responseJSON(await fetch("build",{method:"POST",body:new FormData(form)})); polling=true; refresh(); }
  catch(error){ show("failed",error.message); build.disabled=false; bar.classList.remove("indeterminate"); }
});
launch.addEventListener("click",async()=>{ try { await responseJSON(await fetch("launch",{method:"POST"})); show("succeeded","Game launched"); } catch(error){ show("failed",error.message); } });
closeButton.addEventListener("click",async()=>{ try { await responseJSON(await fetch("close",{method:"POST"})); show("idle","Builder closed — you can close this tab"); build.disabled=true; launch.disabled=true; closeButton.disabled=true; } catch(error){ show("failed",error.message); } });
</script>
</body>
</html>`
