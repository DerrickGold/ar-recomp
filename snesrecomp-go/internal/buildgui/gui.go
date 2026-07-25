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
	State      string `json:"state"`
	Log        string `json:"log"`
	Error      string `json:"error,omitempty"`
	Message    string `json:"message,omitempty"`
	OutputPath string `json:"outputPath,omitempty"`
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
		response.Header().Set("Content-Security-Policy",
			"default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "+
				"connect-src 'self'; img-src 'none'; object-src 'none'; base-uri 'none'")
		response.Header().Set("Cache-Control", "no-store")
		response.Header().Set("Referrer-Policy", "no-referrer")
		response.Header().Set("X-Content-Type-Options", "nosniff")
		page := strings.Replace(pageHTML, "{{TITLE}}",
			html.EscapeString(app.options.Title), 1)
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
:root { color-scheme: dark; --ink:#f3efe2; --muted:#ada791; --panel:#181a1d; --line:#34363a; --gold:#d9ae59; --ok:#72c69a; --bad:#ef8176; }
* { box-sizing: border-box; }
body { margin:0; min-height:100vh; background:radial-gradient(circle at 75% 0,#363022 0,transparent 32rem),#0d0f11; color:var(--ink); font:16px/1.5 system-ui,-apple-system,sans-serif; }
main { width:min(850px,calc(100% - 32px)); margin:48px auto; }
h1 { margin:0; font-family:Georgia,serif; font-size:clamp(2rem,6vw,4rem); font-weight:500; letter-spacing:-.04em; }
.eyebrow { color:var(--gold); text-transform:uppercase; letter-spacing:.18em; font-size:.72rem; font-weight:700; }
.lede { max-width:650px; color:var(--muted); margin:10px 0 28px; }
.panel { background:color-mix(in srgb,var(--panel) 94%,transparent); border:1px solid var(--line); border-radius:18px; padding:24px; box-shadow:0 22px 70px #0008; }
label { display:block; font-weight:650; margin-bottom:8px; }
input[type=file] { width:100%; padding:14px; border:1px dashed #5b5d62; border-radius:10px; background:#111316; color:var(--muted); }
.actions { display:flex; flex-wrap:wrap; gap:10px; margin-top:18px; }
button { border:0; border-radius:999px; padding:11px 18px; background:var(--gold); color:#17130b; font:inherit; font-weight:750; cursor:pointer; }
button.secondary { color:var(--ink); background:#292b2f; border:1px solid #44474c; }
button:disabled { opacity:.4; cursor:not-allowed; }
#state { display:flex; align-items:center; gap:9px; margin:22px 0 8px; color:var(--muted); }
#state[data-kind=building]::before { content:""; width:13px; height:13px; border:2px solid #ffffff33; border-top-color:var(--gold); border-radius:50%; animation:spin .8s linear infinite; }
#state[data-kind=succeeded] { color:var(--ok); }
#state[data-kind=failed] { color:var(--bad); }
pre { margin:0; min-height:180px; max-height:360px; overflow:auto; white-space:pre-wrap; word-break:break-word; padding:16px; border-radius:10px; background:#090a0c; border:1px solid #292b2f; color:#c9c5b8; font:12px/1.55 ui-monospace,SFMono-Regular,Consolas,monospace; }
.privacy { color:var(--muted); font-size:.83rem; margin-top:18px; }
@keyframes spin { to { transform:rotate(360deg); } }
</style>
</head>
<body>
<main>
  <div class="eyebrow">Local, private, self-contained</div>
  <h1>Build your game.</h1>
  <p class="lede">Choose your legally obtained ROM. The build runs entirely on this computer with the toolchain packaged alongside this builder.</p>
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
    <div id="state" data-kind="idle">Ready to build</div>
    <pre id="log">Select your ROM to begin.</pre>
    <div class="privacy">Your ROM is copied only into this local folder and is never uploaded. This page communicates only with the builder at 127.0.0.1.</div>
  </section>
</main>
<script>
const form=document.querySelector("#build-form"), build=document.querySelector("#build"), launch=document.querySelector("#launch");
const state=document.querySelector("#state"), log=document.querySelector("#log"), closeButton=document.querySelector("#close");
let polling=false;
function show(kind,text){ state.dataset.kind=kind; state.textContent=text; }
async function responseJSON(response){ const body=await response.json(); if(!response.ok) throw new Error(body.error||"Request failed"); return body; }
async function refresh(){
  try {
    const data=await responseJSON(await fetch("status",{cache:"no-store"}));
    if(data.log){ log.textContent=data.log; log.scrollTop=log.scrollHeight; }
    if(data.state==="building"){ show("building","Building — this can take a few minutes"); build.disabled=true; launch.disabled=true; }
    if(data.state==="succeeded"){ show("succeeded",data.message||"Build complete"); build.disabled=false; launch.disabled=false; polling=false; }
    if(data.state==="failed"){ show("failed",data.error||"Build failed"); build.disabled=false; launch.disabled=true; polling=false; }
  } catch(error) { show("failed",error.message); polling=false; }
  if(polling) setTimeout(refresh,500);
}
form.addEventListener("submit",async event=>{
  event.preventDefault();
  if(!document.querySelector("#rom").files.length) return;
  build.disabled=true; launch.disabled=true; log.textContent="Preparing local ROM copy…"; show("building","Starting build");
  try { await responseJSON(await fetch("build",{method:"POST",body:new FormData(form)})); polling=true; refresh(); }
  catch(error){ show("failed",error.message); build.disabled=false; }
});
launch.addEventListener("click",async()=>{ try { await responseJSON(await fetch("launch",{method:"POST"})); show("succeeded","Game launched"); } catch(error){ show("failed",error.message); } });
closeButton.addEventListener("click",async()=>{ try { await responseJSON(await fetch("close",{method:"POST"})); show("idle","Builder closed — you can close this tab"); build.disabled=true; launch.disabled=true; closeButton.disabled=true; } catch(error){ show("failed",error.message); } });
</script>
</body>
</html>`
