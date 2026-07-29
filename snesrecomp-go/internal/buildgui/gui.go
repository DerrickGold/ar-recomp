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
	Message string `json:"message"`
	// OutputPath is the generated run-game script: the way to play WITHOUT this
	// GUI, which is its real purpose. Reported to the user so they know what to
	// double-click later.
	OutputPath string `json:"outputPath"`
	// BinaryPath is the game executable, and WorkingDir the directory it must
	// run from. The GUI launches these directly rather than shelling out to the
	// script: going through the script hands off to the OS ("open", "start"),
	// which reports success as soon as the HANDOFF works, so a missing or
	// unrunnable binary looked like a successful launch. Invoking it here means
	// a real failure reaches the user.
	//
	// Optional; with these empty the host falls back to the script.
	BinaryPath string `json:"binaryPath,omitempty"`
	WorkingDir string `json:"workingDir,omitempty"`
}

// Options configures one local GUI session.
type Options struct {
	Title       string
	ProjectRoot string
	OpenBrowser bool
	Stdout      io.Writer
	Build       func(context.Context, string, io.Writer) (Result, error)
	Launch      func(Result) error
	// Detect reports what this copy of the bundle can currently do: launch an
	// already-built game, run a rebuild, or reclaim space by removing the
	// build-only files. Called at session start and again on every status poll,
	// so the page always reflects the filesystem rather than only what happened
	// in this process.
	//
	// MUST BE CHEAP: this runs at the poll interval. Report CanSlim from the
	// PRESENCE of the build-only files, and leave SlimBytes zero -- sizing them
	// means walking the tree, which belongs in MeasureSlim below.
	//
	// Optional: with no Detect the GUI behaves as it always did, opening on a
	// ROM picker. That keeps every existing caller and test working unchanged.
	Detect func() InstallState
	// MeasureSlim sizes what the cleanup would reclaim. Split out from Detect
	// because it walks the whole build tree -- measured at 24ms for ~900 files
	// and 74ms for ~3600, which at a 500ms poll is 5-15% of a core spent
	// re-deriving a number that only changes when a build or a cleanup runs.
	// So it is called ONLY at those two moments, and its result is cached.
	//
	// Optional: without it the offer still appears, just with no size in it
	// (slimSummary already returns "" for an unknown size).
	MeasureSlim func() int64
	// Slim removes the build-only files, keeping everything the game needs to
	// run. Optional; the cleanup offer is not shown when it is absent.
	Slim         func(io.Writer) error
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
	// Install is what this copy can do, and Mode is the page shape derived from
	// it. Both are sent on every poll so the page never has to infer capability
	// from state transitions it may have missed.
	Install InstallState `json:"install"`
	Mode    string       `json:"mode"`
	// SlimSize is the human-readable reclaimable size ("612 MB"), empty when
	// unknown or when there is nothing to reclaim.
	SlimSize string `json:"slimSize,omitempty"`
	// SlimDone is set once a cleanup has completed in this session, so the page
	// can confirm it rather than silently dropping the offer.
	SlimDone bool `json:"slimDone,omitempty"`
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
	install      InstallState
	slimDone     bool
	slimming     bool
	// slimBytes caches MeasureSlim's walk. Refreshed only when the size can
	// have changed -- session start, after a build, after a cleanup -- because
	// Detect runs at the poll interval and must not walk the tree.
	slimBytes int64
}

func newApplication(ctx context.Context, options Options, token string) *application {
	app := &application{
		ctx: ctx, options: options, prefix: "/" + token + "/",
		closed: make(chan struct{}), state: "idle",
	}
	// Adopt an existing build as this session's result, which is what makes
	// Launch work without rebuilding first: the launch handler needs an
	// OutputPath, and a freshly opened process has no other source for one.
	app.install = app.detect()
	// One walk at startup so a bundle that is ALREADY slimmable shows its size
	// without waiting for a build that may never happen in this session.
	if app.install.CanSlim && app.options.MeasureSlim != nil {
		app.slimBytes = app.options.MeasureSlim()
		app.install.SlimBytes = app.slimBytes
	}
	if app.install.CanLaunch {
		app.result = app.install.Result
	}
	return app
}

// detect re-probes the filesystem. Safe with no Detect hook: the zero
// InstallState means "cannot launch, cannot rebuild", and refreshState below
// keeps the legacy ROM-picker behaviour in that case.
func (app *application) detect() InstallState {
	if app.options.Detect == nil {
		return InstallState{}
	}
	return app.options.Detect()
}

// remeasureSlim refreshes the cached cleanup size. Separate from refreshState
// because it walks the build tree: call it only where the size can actually
// have changed, never from the status poll. Caller must NOT hold the mutex.
func (app *application) remeasureSlim() {
	var size int64
	if app.options.MeasureSlim != nil {
		size = app.options.MeasureSlim()
	}
	app.mu.Lock()
	app.slimBytes = size
	app.install.SlimBytes = size
	app.mu.Unlock()
}

// refreshState recomputes install capability after something changed on disk.
// Caller must NOT hold the mutex: Detect touches the filesystem.
//
// Deliberately does NOT re-measure the cleanup size -- see remeasureSlim. The
// cached figure is carried over so a poll cannot blank a size the page is
// already showing, and is dropped when there is nothing left to clean.
func (app *application) refreshState() {
	state := app.detect()
	app.mu.Lock()
	defer app.mu.Unlock()
	if state.CanSlim && state.SlimBytes == 0 {
		state.SlimBytes = app.slimBytes
	}
	if !state.CanSlim {
		app.slimBytes = 0
	}
	app.install = state
	if state.CanLaunch {
		// Adopt the detected artifacts unless THIS session built something,
		// whose paths are authoritative. Keyed on BinaryPath rather than
		// OutputPath: the run-game script is optional now, so a launchable
		// install may legitimately have no script path at all.
		if app.state != "succeeded" || app.result.BinaryPath == "" {
			app.result = state.Result
		}
		return
	}
	// The game went away (deleted, moved, or a folder renamed underneath us).
	// Drop the stale result so Launch reports "nothing to launch" rather than
	// failing against a path that no longer exists -- and so the page stops
	// offering Play. A build that succeeded in THIS session keeps its message,
	// since the log and its outcome are still what the user is reading.
	if app.state != "succeeded" {
		app.result = Result{}
	}
}

// pageMode is the shape the page should take. Without a Detect hook every
// session looks like the original builder, so existing callers are unaffected.
func (app *application) pageMode(install InstallState) string {
	if app.options.Detect == nil {
		return "buildable"
	}
	return install.mode()
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
		// The sky, clouds and columns are CSS; the cover art and the manual
		// are served from this same origin, so 'self' covers everything and
		// no remote origin is reachable. `frame-src 'self'` admits the
		// manual's <iframe> (the browser's own PDF viewer) without opening
		// `object-src`, which stays 'none'.
		response.Header().Set("Content-Security-Policy",
			"default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "+
				"connect-src 'self'; img-src 'self'; frame-src 'self'; "+
				"object-src 'none'; base-uri 'none'")
		response.Header().Set("Cache-Control", "no-store")
		response.Header().Set("Referrer-Policy", "no-referrer")
		response.Header().Set("X-Content-Type-Options", "nosniff")
		page := strings.Replace(pageHTML, "{{TITLE}}",
			html.EscapeString(app.options.Title), 1)
		page = strings.Replace(page, "{{STEPS}}", renderStepList(), 1)
		_, _ = io.WriteString(response, page)
	case endpoint == "boxart.webp" && request.Method == http.MethodGet:
		serveBoxArt(response, request)
	case endpoint == "manual.pdf" && request.Method == http.MethodGet:
		serveManual(response, request)
	case endpoint == "status" && request.Method == http.MethodGet:
		app.writeStatus(response)
	case endpoint == "build" && request.Method == http.MethodPost:
		app.startBuild(response, request)
	case endpoint == "launch" && request.Method == http.MethodPost:
		app.launch(response)
	case endpoint == "slim" && request.Method == http.MethodPost:
		app.slim(response)
	case endpoint == "close" && request.Method == http.MethodPost:
		app.close(response)
	default:
		http.NotFound(response, request)
	}
}

func (app *application) writeStatus(response http.ResponseWriter) {
	// Re-probe unless a build is running. The install state can change from
	// OUTSIDE this process -- someone deletes the game, moves the folder, or
	// cleans up by hand -- and a cached answer would leave the page offering a
	// Play button for a game that is gone. Skipped mid-build because the tree is
	// churning by definition and the probe would be noise; the post-build
	// refresh covers that case.
	//
	// Cheap enough for a 500 ms poll: a bounded handful of stats. The directory
	// WALK that sizes the cleanup is deliberately not here -- it costs 24ms at
	// ~900 files and 74ms at ~3600, and is cached by remeasureSlim instead.
	app.mu.Lock()
	building := app.state == "building"
	app.mu.Unlock()
	if !building {
		app.refreshState()
	}

	app.mu.Lock()
	current := status{
		State: app.state, Log: app.log.String(), Error: app.errorMessage,
		Message: app.result.Message, OutputPath: app.result.OutputPath,
		Install: app.install, SlimDone: app.slimDone,
	}
	app.mu.Unlock()
	current.Mode = app.pageMode(current.Install)
	// Only offered while a cleanup is actually possible: no Slim hook means no
	// offer, and an already-lean install has nothing left to remove.
	if app.options.Slim != nil && current.Install.CanSlim {
		current.SlimSize = slimSummary(current.Install.SlimBytes)
	} else {
		current.Install.CanSlim = false
	}
	// Derived outside the lock: computeProgress is pure and can be slow
	// relative to a mutex hold (it scans the whole log tail on every poll).
	current.Progress = computeProgress(current.Log, current.State)
	writeJSON(response, http.StatusOK, current)
}

func (app *application) startBuild(response http.ResponseWriter, request *http.Request) {
	// A build must be refused when its inputs are gone. Re-probed here rather
	// than trusting the cached state, because a cleanup (or a manual deletion)
	// can happen between a page load and this request, and the failure mode
	// otherwise is a build that starts and then dies partway with a confusing
	// toolchain error. Enforced SERVER-side deliberately: hiding the button is
	// presentation, and presentation is not a guarantee.
	if app.options.Detect != nil {
		app.refreshState()
		app.mu.Lock()
		canRebuild := app.install.CanRebuild
		app.mu.Unlock()
		if !canRebuild {
			writeJSONError(response, http.StatusConflict,
				"this install no longer has the build tools — download the "+
					"package again from the repository to rebuild")
			return
		}
	}

	app.mu.Lock()
	if app.state == "building" {
		app.mu.Unlock()
		writeJSONError(response, http.StatusConflict, "a build is already running")
		return
	}
	if app.slimming {
		app.mu.Unlock()
		writeJSONError(response, http.StatusConflict,
			"wait for the cleanup to finish")
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
		if buildErr != nil {
			app.state = "failed"
			app.errorMessage = buildErr.Error()
			app.mu.Unlock()
			return
		}
		app.state = "succeeded"
		app.result = result
		app.mu.Unlock()
		// Re-probe so the cleanup offer appears with a current size: the build
		// just created most of what it would reclaim. This is one of the two
		// moments the size can change, so it is one of the two that walk.
		app.remeasureSlim()
		app.refreshState()
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
	result, state, install := app.result, app.state, app.install
	app.mu.Unlock()
	// NEVER mid-build. Broadening the guard below to accept a DETECTED build made
	// Play work during a rebuild, which launches the very binary the build is in
	// the middle of overwriting -- at best a crash, at worst a half-written
	// executable. Checked first and independently of CanLaunch, because the cached
	// install state legitimately still says "launchable" while a rebuild runs
	// (status deliberately skips re-probing mid-build, since the tree is churning).
	if state == "building" {
		writeJSONError(response, http.StatusConflict,
			"a build is running -- wait for it to finish before playing")
		return
	}
	// Launchable either because this session built it, or because a previous one
	// did and the artifacts are still on disk. The second case is the whole
	// point of detection -- it is what turns the builder into a launcher.
	if state != "succeeded" && !install.CanLaunch {
		writeJSONError(response, http.StatusConflict, "finish a successful build first")
		return
	}
	// Either path is enough to launch: the binary (which the host runs directly)
	// or the generated script (its fallback). Testing only OutputPath would
	// refuse a launch the host could perform, since the script is optional now
	// and a user may well have deleted it.
	if result.BinaryPath == "" && result.OutputPath == "" {
		writeJSONError(response, http.StatusConflict,
			"no built game was found to launch")
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

// slim removes the build-only files and re-probes. Synchronous: deleting a
// directory tree is fast next to a build, and the page's poll picks up the new
// state immediately afterwards.
func (app *application) slim(response http.ResponseWriter) {
	if app.options.Slim == nil {
		writeJSONError(response, http.StatusNotImplemented,
			"cleanup is unavailable on this host")
		return
	}
	app.mu.Lock()
	if app.state == "building" {
		app.mu.Unlock()
		writeJSONError(response, http.StatusConflict,
			"wait for the current build to finish")
		return
	}
	if app.slimming {
		app.mu.Unlock()
		writeJSONError(response, http.StatusConflict, "cleanup is already running")
		return
	}
	// Refuse when there is nothing to remove, so a double-click cannot delete
	// a second time against a changed filesystem.
	if !app.install.CanSlim {
		app.mu.Unlock()
		writeJSONError(response, http.StatusConflict,
			"this install has no build files left to remove")
		return
	}
	app.slimming = true
	app.mu.Unlock()

	err := app.options.Slim(&lockedLogWriter{app: app})

	app.mu.Lock()
	app.slimming = false
	if err == nil {
		app.slimDone = true
	}
	app.mu.Unlock()
	// The other moment the size changes: the cleanup just removed the trees.
	app.remeasureSlim()
	app.refreshState()

	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(response, http.StatusOK, map[string]string{"state": "slimmed"})
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
 * Every visual is CSS or inline SVG apart from the cover art, which is served
 * from this origin -- no other bitmap ships (the release bundle deliberately
 * carries no media; see packaging/CMakeLists.txt).
 *
 * Structure: a two-tab shell (Build / Manual) with a STICKY PROGRESS DOCK. The
 * build is not a full-height section -- once it starts, its progress detaches
 * into the dock so the reader can sit in the manual for the several minutes a
 * build takes and still see exactly where it is. */
:root {
  color-scheme: dark;
  --ink:#f6f1e3; --muted:#b9b39c; --gold:#e0b95f; --gold-deep:#a97f2c;
  --marble:#e9e3d2; --marble-shade:#a89d84; --marble-dark:#6d6552;
  --sky-high:#1d3f78; --sky-mid:#4b7bb5; --sky-low:#b8d4e6; --sky-haze:#e9d9b6;
  --panel:#10192bd9; --line:#31456b;
  --ok:#8fd6a6; --bad:#f0938a;
  --dock-h:64px;
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
  /* Leave room for the dock so it can never cover the last line of content. */
  padding-bottom:var(--dock-h);
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
  #bar.indeterminate { animation:none; }
}

/* Sky Palace colonnade. The shafts are ABSOLUTE, not fixed, and sized to the
 * document, so each column stands on a plinth at the page's base instead of
 * running off both edges of the viewport like a pipe -- and content no longer
 * slides behind a floating shaft while scrolling. Hidden on narrow viewports
 * where they would crowd the content rather than frame it. */
.colonnade { position:absolute; top:0; bottom:0; width:78px; z-index:1; pointer-events:none; }
.colonnade.left { left:max(0px,calc(50% - 610px)); }
.colonnade.right { right:max(0px,calc(50% - 610px)); }
.shaft {
  position:absolute; top:64px; bottom:64px; left:11px; right:11px;
  background:
    repeating-linear-gradient(90deg,
      var(--marble-shade) 0 1px, var(--marble) 1px 7px,
      #fffdf6 7px 9px, var(--marble) 9px 15px, var(--marble-shade) 15px 16px);
  box-shadow:inset 0 0 18px #6d655233, 0 0 26px #0a132480;
}
/* Capital (top) and plinth (bottom): each is a two-tier block, so the column
 * visibly terminates instead of being cropped. */
.cap, .base {
  position:absolute; left:0; right:0; height:30px;
  background:linear-gradient(180deg,#fffdf7,var(--marble) 55%,var(--marble-shade));
  box-shadow:0 2px 10px #0a132466;
}
.cap { top:34px; }
.cap::before {
  content:""; position:absolute; left:-6px; right:-6px; top:-34px; height:34px;
  background:linear-gradient(180deg,var(--marble),#fffdf7 42%,var(--marble-shade));
  border-radius:3px 3px 0 0;
}
.base { bottom:34px; background:linear-gradient(180deg,var(--marble-shade),var(--marble) 45%,#fffdf7); }
.base::after {
  content:""; position:absolute; left:-8px; right:-8px; bottom:-34px; height:34px;
  background:linear-gradient(180deg,var(--marble),var(--marble-shade) 55%,var(--marble-dark));
  border-radius:0 0 2px 2px;
  box-shadow:0 6px 18px #0a132466;
}
@media (max-width:1080px) { .colonnade { display:none; } }

main { position:relative; z-index:2; width:min(920px,calc(100% - 40px)); margin:0 auto; padding:38px 0 40px; }

/* Header: the cover art sits BESIDE the title rather than floating between the
 * lede and the panel, so it anchors the composition and stays visible on both
 * tabs without needing a section of its own. */
.masthead { display:flex; align-items:center; gap:26px; }
.masthead .cover {
  flex:none; width:210px; border:1px solid var(--gold-deep); border-radius:3px;
  overflow:hidden; background:#0d1b34;
  box-shadow:0 14px 38px #05091599, inset 0 0 0 1px #ffe9b422;
}
.masthead .cover img { display:block; width:100%; height:auto; }
.masthead .titles { min-width:0; }
.eyebrow { color:#f4e6c2; text-transform:uppercase; letter-spacing:.2em; font-size:.68rem; font-weight:700;
  font-family:system-ui,-apple-system,sans-serif; text-shadow:0 1px 3px #0a1324cc; }
h1 {
  margin:.12em 0 .1em; font-size:clamp(1.9rem,5vw,3.1rem); font-weight:600; letter-spacing:.01em;
  color:#fff8e6; text-shadow:0 2px 0 var(--gold-deep), 0 3px 14px #0a132480;
}
.tagline { margin:0; color:#efe7d0; font-style:italic; font-size:1rem;
  text-shadow:0 1px 4px #0a132466; }
.lede { max-width:62ch; color:#e8e2cf; margin:12px 0 0; font-size:.95rem;
  text-shadow:0 1px 4px #0a132466; }
@media (max-width:640px) {
  .masthead { flex-direction:column; align-items:flex-start; gap:16px; }
  .masthead .cover { width:min(260px,60%); }
}

/* Tab strip. Two tabs only: the thing you came to do, and the thing to do while
 * it runs. */
.tabs { display:flex; gap:4px; margin:26px 0 0; border-bottom:2px solid var(--gold-deep); }
.tab {
  appearance:none; border:1px solid transparent; border-bottom:0;
  border-radius:4px 4px 0 0; padding:10px 20px; margin-bottom:-2px;
  background:#0f1c3480; color:#cfc9b5; font:inherit; font-size:.95rem; font-weight:600;
  letter-spacing:.02em; cursor:pointer; position:relative; box-shadow:none;
}
.tab:hover { color:var(--ink); background:#16274580; }
.tab[aria-selected=true] {
  background:var(--panel); color:#fff8e6;
  border-color:var(--line); border-bottom:2px solid var(--panel);
  box-shadow:0 -2px 0 var(--gold) inset;
}
/* Badge: a build that finishes while the reader is in the manual announces
 * itself here rather than yanking the tab out from under them. */
.tab .badge {
  display:none; width:8px; height:8px; border-radius:50%; margin-left:8px;
  vertical-align:middle; background:var(--gold);
}
.tab[data-badge=ok] .badge { display:inline-block; background:var(--ok); }
.tab[data-badge=bad] .badge { display:inline-block; background:var(--bad); }

.panel {
  background:var(--panel); backdrop-filter:blur(7px);
  border:1px solid var(--line); border-top:0;
  border-radius:0 0 4px 4px; padding:24px; box-shadow:0 20px 60px #05091580;
}
[role=tabpanel][hidden] { display:none; }

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

/* PLAY BLOCK. In launcher mode this is the entire panel, so it has to carry the
 * page rather than read as one control among several: the button is oversized
 * and gold, and the heading states plainly that a game exists. */
#play-box[hidden], #slim-box[hidden], #build-box[hidden], #nobuild[hidden],
#slim-done[hidden] { display:none; }
.play-head { display:flex; align-items:center; gap:18px; flex-wrap:wrap; }
.play-title { font-size:1.15rem; color:#fff8e6; font-weight:600; letter-spacing:.01em; }
.play-sub { color:var(--muted); font-size:.83rem; margin-top:2px;
  font-family:system-ui,-apple-system,sans-serif; }
.play-btn { margin-left:auto; font-size:1.05rem; padding:14px 30px; }
/* A rebuild is secondary once a game exists, so the build block is separated
 * from the play block by a rule rather than competing with it. */
body[data-mode=ready] #build-box, body[data-mode=ready] #slim-box {
  margin-top:20px; padding-top:18px; border-top:1px solid var(--line);
}
body[data-mode=ready] #rom-label::before { content:"Rebuild \2014 "; color:var(--muted); }
/* Launcher mode: no build affordances at all, and the panel is quieter. */
body[data-mode=launcher] #steps-box, body[data-mode=launcher] #log-box,
body[data-mode=launcher] .privacy { display:none; }

/* Cleanup offer. Framed as an opportunity, not a warning -- nothing is wrong,
 * there is simply space to reclaim. */
.slim-title { font-weight:600; color:#fff3d6; font-size:1rem; }
.slim-copy { margin:6px 0 0; color:#e4dece; font-size:.88rem; max-width:64ch; }
.slim-done { margin-top:18px; color:var(--ok); font-size:.9rem; font-weight:600;
  font-family:system-ui,-apple-system,sans-serif; }
button.quiet { background:none; border-color:transparent; box-shadow:none;
  color:var(--muted); font-weight:600; }
button.quiet:hover:not(:disabled) { color:var(--ink); }
.notice { margin-top:18px; padding:13px 15px; border:1px solid var(--line);
  border-left:3px solid var(--gold-deep); border-radius:3px; background:#0c142759;
  color:#ded8c4; font-size:.86rem; font-family:system-ui,-apple-system,sans-serif; }
.notice strong { color:#fff3d6; display:block; margin-bottom:3px; }

/* ONE status line. The previous layout said the same thing three times -- a
 * phase label, a separate state line, and eight empty step circles -- before
 * the user had even chosen a file. */
#state { display:flex; align-items:center; gap:9px; margin:20px 0 0; color:#ded8c4;
  font-family:system-ui,-apple-system,sans-serif; font-size:.92rem; }
#state[data-kind=building]::before {
  content:""; width:13px; height:13px; border:2px solid #ffffff2e;
  border-top-color:var(--gold); border-radius:50%; animation:spin .8s linear infinite;
}
#state[data-kind=succeeded] { color:var(--ok); font-weight:600; }
#state[data-kind=failed] { color:var(--bad); font-weight:600; }
@keyframes spin { to { transform:rotate(360deg); } }

/* The step checklist is collapsed until a build starts: eight grey circles are
 * the largest block on the page and say nothing at all while idle. */
#steps-box { margin-top:16px; }
#steps-box[hidden] { display:none; }
.steps { list-style:none; margin:0; padding:0; display:grid; gap:5px;
  grid-template-columns:repeat(auto-fit,minmax(230px,1fr)); }
.step { display:flex; align-items:center; gap:10px; font-size:.86rem; color:#8f9ab4;
  font-family:system-ui,-apple-system,sans-serif; transition:color .2s; }
.tick { width:15px; height:15px; flex:none; border:1px solid #45577d; border-radius:50%;
  position:relative; transition:all .2s; }
.step[data-state=done] { color:var(--ok); }
.step[data-state=done] .tick { border-color:var(--ok); background:#8fd6a626; }
.step[data-state=done] .tick::after {
  content:""; position:absolute; left:4px; top:1px; width:5px; height:8px;
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

details { margin-top:16px; }
summary { cursor:pointer; color:var(--muted); font-size:.8rem; letter-spacing:.06em;
  text-transform:uppercase; font-family:system-ui,-apple-system,sans-serif; }
summary:hover { color:var(--gold); }
pre {
  margin:10px 0 0; min-height:140px; max-height:300px; overflow:auto;
  white-space:pre-wrap; word-break:break-word; padding:14px; border-radius:3px;
  background:#060b16; border:1px solid #26364f; color:#c3bda9;
  font:12px/1.55 ui-monospace,SFMono-Regular,Consolas,monospace;
}
.privacy { color:#cfc9b5; font-size:.78rem; margin:16px 0 0; line-height:1.5;
  font-family:system-ui,-apple-system,sans-serif; }

/* Manual tab. The reader gets the full panel: this is the whole reason the tab
 * exists, and a scanned page is unreadable in a short box. */
.manual-bar { display:flex; flex-wrap:wrap; align-items:baseline; justify-content:space-between;
  gap:12px; margin-bottom:12px; }
.manual-bar p { margin:0; color:var(--muted); font-size:.83rem;
  font-family:system-ui,-apple-system,sans-serif; }
.linkbtn { color:var(--gold); font-size:.83rem; text-decoration:none; border-bottom:1px solid #a97f2c66;
  font-family:system-ui,-apple-system,sans-serif; white-space:nowrap; }
.linkbtn:hover { color:#ffe9b8; border-bottom-color:var(--gold); }
#manual-frame {
  display:block; width:100%; height:min(76vh,900px);
  border:1px solid var(--gold-deep); border-radius:3px; background:#0d1b34;
}
.blurb { margin-top:22px; padding-top:18px; border-top:1px solid var(--line); }
.blurb h2 { margin:0 0 4px; font-size:1rem; color:#fff3d6; letter-spacing:.02em; }
.blurb p { margin:0; color:#e4dece; font-size:.9rem; }
.blurb .colophon { margin-top:12px; color:var(--muted); font-size:.75rem;
  font-family:system-ui,-apple-system,sans-serif; }

/* STICKY PROGRESS DOCK. Present on every tab once a build begins, so the build
 * no longer needs to own the viewport. Hidden entirely while idle -- an empty
 * bar pinned to the screen is just furniture. */
#dock {
  position:fixed; left:0; right:0; bottom:0; z-index:5;
  background:#0a1426f2; backdrop-filter:blur(9px);
  border-top:2px solid var(--gold-deep); box-shadow:0 -10px 34px #05091599;
  transform:translateY(100%); transition:transform .28s cubic-bezier(.4,0,.2,1);
}
#dock[data-open=true] { transform:translateY(0); }
.dock-inner {
  width:min(920px,calc(100% - 40px)); margin:0 auto; padding:10px 0 12px;
  display:flex; align-items:center; gap:16px;
}
.dock-text { min-width:0; flex:1; }
.dock-head { display:flex; align-items:baseline; gap:10px; }
#dock-phase { font-weight:600; font-size:.95rem; white-space:nowrap; overflow:hidden;
  text-overflow:ellipsis; }
#dock-pct { margin-left:auto; font-variant-numeric:tabular-nums; color:var(--gold);
  font-weight:700; font-family:system-ui,-apple-system,sans-serif; font-size:.9rem; }
#dock-detail { color:var(--muted); font-size:.78rem; min-height:1.1em;
  font-family:system-ui,-apple-system,sans-serif; }
#dock[data-kind=succeeded] #dock-phase { color:var(--ok); }
#dock[data-kind=failed] #dock-phase { color:var(--bad); }
#dock-launch { display:none; flex:none; padding:9px 18px; }
#dock[data-kind=succeeded] #dock-launch { display:inline-block; }

.track {
  height:12px; border:1px solid #40598c; border-radius:3px; overflow:hidden;
  background:linear-gradient(180deg,#091326,#0c1a33);
  background-color:#091326; box-shadow:inset 0 2px 6px #00000073; margin-top:6px;
}
#bar {
  height:100%; width:0; transition:width .45s cubic-bezier(.4,0,.2,1);
  background:linear-gradient(180deg,#ffeec0,var(--gold) 45%,var(--gold-deep));
  box-shadow:0 0 12px #e0b95f80;
}
#dock[data-kind=succeeded] #bar { background:linear-gradient(180deg,#cdf0da,var(--ok)); }
#dock[data-kind=failed] #bar { background:linear-gradient(180deg,#f8cfca,var(--bad)); }
#bar.indeterminate {
  width:100% !important;
  background:repeating-linear-gradient(115deg,var(--gold-deep) 0 12px,var(--gold) 12px 24px);
  background-size:200% 100%; animation:slide 1.1s linear infinite; opacity:.55;
}
@keyframes slide { to { background-position:-48px 0; } }
.sr { position:absolute; width:1px; height:1px; overflow:hidden; clip:rect(0 0 0 0); }
</style>
</head>
<body>
<div class="sky" aria-hidden="true"><div class="clouds far"></div><div class="clouds near"></div></div>
<div class="colonnade left" aria-hidden="true"><div class="cap"></div><div class="shaft"></div><div class="base"></div></div>
<div class="colonnade right" aria-hidden="true"><div class="cap"></div><div class="shaft"></div><div class="base"></div></div>

<main>
  <header class="masthead">
    <div class="cover">
      <img src="boxart.webp" width="760" height="555" alt="ActRaiser Super Nintendo box art: the game's logo above a lightning storm over pyramids">
    </div>
    <div class="titles">
      <div class="eyebrow">Local &middot; Private &middot; Self-contained</div>
      <h1>Build your game</h1>
      <p class="tagline">Create order from chaos</p>
      <p class="lede">Choose your legally obtained ROM. Everything runs on this computer
      with the toolchain packaged beside this builder &mdash; nothing is uploaded.</p>
    </div>
  </header>

  <div class="tabs" role="tablist" aria-label="Builder sections">
    <button class="tab" id="tab-build" role="tab" aria-selected="true"
            aria-controls="panel-build">Build<span class="badge" aria-hidden="true"></span></button>
    <button class="tab" id="tab-manual" role="tab" aria-selected="false"
            aria-controls="panel-manual" tabindex="-1">Manual</button>
  </div>

  <section class="panel" id="panel-build" role="tabpanel" aria-labelledby="tab-build">
    <!-- PLAY BLOCK. Shown whenever a built game is on disk, whether this
         session built it or a previous one did. In launcher mode it is the only
         thing here. -->
    <div id="play-box" hidden>
      <div class="play-head">
        <div>
          <div class="play-title" id="play-title">Your game is built and ready</div>
          <div class="play-sub" id="play-sub"></div>
        </div>
        <button id="play" type="button" class="play-btn">&#9654;&nbsp; Play</button>
      </div>
    </div>

    <!-- CLEANUP OFFER. Appears after a successful build, and on any install
         that still carries removable build files. -->
    <div id="slim-box" hidden>
      <div class="slim-title">Free up space?</div>
      <p class="slim-copy" id="slim-copy">
        The build tools are only needed for future rebuilds. You can remove them
        and keep just the game &mdash; and if you ever want to rebuild, download
        the package again from the repository.
      </p>
      <div class="actions">
        <button id="slim" type="button" class="secondary">Clean up build tools</button>
        <button id="slim-dismiss" type="button" class="quiet">Keep them</button>
      </div>
    </div>

    <div id="slim-done" hidden class="slim-done">
      Build tools removed &mdash; this install is now just the game.
    </div>

    <!-- BUILD BLOCK. Hidden entirely in launcher mode: with the inputs gone
         there is nothing a ROM picker could accomplish, and a disabled control
         invites the question "why?" without answering it. -->
    <div id="build-box">
      <form id="build-form">
        <label for="rom" id="rom-label">ActRaiser ROM (.sfc or .smc)</label>
        <input id="rom" name="rom" type="file" accept=".sfc,.smc" required>
        <div class="actions">
          <button id="build" type="submit">Build game</button>
          <button id="launch" class="secondary" type="button" disabled>Launch game</button>
        </div>
      </form>
    </div>

    <!-- Why building is unavailable. Only rendered in the modes where the
         build block is hidden, so it explains an absence rather than
         decorating a working page. -->
    <div id="nobuild" hidden class="notice">
      <strong>Rebuilding is unavailable in this copy.</strong>
      The build tools were removed to save space. Download the package again from
      the repository if you want to rebuild from a ROM.
    </div>

    <div class="actions" id="shell-actions">
      <button id="close" class="secondary" type="button">Close</button>
    </div>

    <div id="state" data-kind="idle">Ready to build &mdash; choose your ROM above</div>

    <div id="steps-box" hidden>
      <ol class="steps" id="steps">{{STEPS}}</ol>
    </div>

    <details id="log-box">
      <summary>Build log</summary>
      <pre id="log">Select your ROM to begin.</pre>
    </details>
    <p class="privacy">Your ROM is copied only into this local folder and is never uploaded.
    This page talks only to the builder on 127.0.0.1.</p>
  </section>

  <section class="panel" id="panel-manual" role="tabpanel" aria-labelledby="tab-manual" hidden>
    <div class="manual-bar">
      <p>The original 40-page instruction booklet &mdash; your browser's own reader
      handles paging, zoom and search.</p>
      <a class="linkbtn" href="manual.pdf" target="_blank" rel="noreferrer">Open in a new tab</a>
    </div>
    <iframe id="manual-frame" title="ActRaiser instruction booklet"></iframe>
    <div class="blurb">
      <h2>Action &amp; Simulation</h2>
      <p>Pulse-stopping action sequences combined with a Simulation Mode that lets you
      forge a new civilization &mdash; a game built to use the Super NES to the full.
      Long ago you and your people built a peaceful land; since then the evil Tanzra
      and his Guardians have made it a breeding ground for monsters. Punish Tanzra and
      give your people back the world they knew.</p>
      <p class="colophon">Text and artwork &copy; 1990&ndash;1992 Quintet / Enix.
      Reproduced from the retail packaging and manual for reference. This project ships
      no game code or data &mdash; that is generated on this machine from your own ROM.</p>
    </div>
  </section>
</main>

<div id="dock" data-open="false" data-kind="idle" aria-live="polite">
  <div class="dock-inner">
    <div class="dock-text">
      <div class="dock-head">
        <span id="dock-phase">Starting build</span>
        <span id="dock-pct" aria-hidden="true">0%</span>
      </div>
      <div class="track" role="progressbar" aria-labelledby="dock-phase"
           aria-valuemin="0" aria-valuemax="100" id="track"><div id="bar"></div></div>
      <div id="dock-detail"></div>
    </div>
    <button id="dock-launch" type="button">Launch game</button>
  </div>
</div>

<script>
const form=document.querySelector("#build-form"), build=document.querySelector("#build"), launch=document.querySelector("#launch");
const state=document.querySelector("#state"), log=document.querySelector("#log"), closeButton=document.querySelector("#close");
const bar=document.querySelector("#bar"), track=document.querySelector("#track"), logBox=document.querySelector("#log-box");
const dock=document.querySelector("#dock"), dockPhase=document.querySelector("#dock-phase");
const dockPct=document.querySelector("#dock-pct"), dockDetail=document.querySelector("#dock-detail");
const dockLaunch=document.querySelector("#dock-launch"), stepsBox=document.querySelector("#steps-box");
const steps=[...document.querySelectorAll(".step")];
const tabs=[...document.querySelectorAll(".tab")];
const buildTab=document.querySelector("#tab-build");
let polling=false;

/* Tabs. The manual's <iframe> is HIDDEN rather than removed when its tab is not
 * showing: unmounting it would refetch 8 MB and lose the reader's page. It is
 * still created empty and only given its src on first open, so a user who never
 * opens the manual never pays for it. */
function selectTab(tab){
  for(const t of tabs){
    const on=t===tab;
    t.setAttribute("aria-selected",String(on));
    t.tabIndex=on?0:-1;
    document.querySelector("#"+t.getAttribute("aria-controls")).hidden=!on;
  }
  if(tab===document.querySelector("#tab-manual")){
    const frame=document.querySelector("#manual-frame");
    if(!frame.getAttribute("src")) frame.setAttribute("src","manual.pdf");
  }
  if(tab===buildTab) buildTab.removeAttribute("data-badge");
  tab.focus({preventScroll:true});
}
tabs.forEach(t=>t.addEventListener("click",()=>selectTab(t)));
/* Left/right arrows move between tabs, per the ARIA tabs pattern. */
document.querySelector(".tabs").addEventListener("keydown",event=>{
  const i=tabs.indexOf(document.activeElement);
  if(i<0) return;
  if(event.key==="ArrowRight"||event.key==="ArrowLeft"){
    event.preventDefault();
    selectTab(tabs[(i+(event.key==="ArrowRight"?1:tabs.length-1))%tabs.length]);
  }
});

function show(kind,text){ state.dataset.kind=kind; state.textContent=text; }

/* MODE. The server decides which shape the page takes (buildgui/install.go), so
 * the page cannot disagree with what the server will actually permit -- the same
 * reason the phase model lives server-side. Applied on every poll because a
 * cleanup or a finished build changes capability mid-session. */
const playBox=document.querySelector("#play-box"), playButton=document.querySelector("#play");
const playTitle=document.querySelector("#play-title"), playSub=document.querySelector("#play-sub");
const buildBox=document.querySelector("#build-box"), noBuild=document.querySelector("#nobuild");
const slimBox=document.querySelector("#slim-box"), slimButton=document.querySelector("#slim");
const slimCopy=document.querySelector("#slim-copy"), slimDone=document.querySelector("#slim-done");
const slimDismiss=document.querySelector("#slim-dismiss");
let slimDismissed=false, lastMode="";

function applyMode(data){
  const mode=data.mode||"buildable", install=data.install||{};
  document.body.dataset.mode=mode;
  /* Play is hidden while a build runs: the binary it would launch is being
     overwritten. The server refuses it too (launch() checks state first) -- this
     is only so the button does not sit there inviting the click. */
  const building=(data.state==="building");
  playBox.hidden=!install.canLaunch||building;
  playButton.disabled=building;
  buildBox.hidden=(mode==="launcher");
  noBuild.hidden=(mode!=="launcher");
  slimDone.hidden=!data.slimDone;
  /* Offered only when the server says there is something to remove, and only
   * once the game is actually playable -- reclaiming space before there is a
   * working build would be the wrong trade. */
  slimBox.hidden=!(install.canSlim && install.canLaunch && !slimDismissed && !data.slimDone);
  if(data.slimSize) slimCopy.dataset.size=data.slimSize;
  slimButton.textContent=data.slimSize
    ? "Clean up build tools ("+data.slimSize+")" : "Clean up build tools";
  if(mode==="launcher"){
    playTitle.textContent="Ready to play";
    playSub.textContent="This copy contains just the game.";
  } else if(install.canLaunch && data.state!=="building"){
    playTitle.textContent="Your game is built and ready";
    playSub.textContent=install.result&&install.result.outputPath?install.result.outputPath:"";
  }
  if(mode==="unusable" && lastMode!==mode)
    show("failed","This copy has neither a built game nor the tools to build one — download the package again.");
  /* The masthead's call to action is wrong once nothing needs building. */
  if(mode==="launcher" && lastMode!==mode){
    document.querySelector("h1").textContent="ActRaiser";
    document.querySelector(".lede").textContent=
      "Your game is built and ready to play. The build tools are not part of this copy.";
    document.querySelector("#tab-build").firstChild.textContent="Play";
  }
  lastMode=mode;
}

/* The phase list and percentage come from the server (buildgui/progress.go), so
 * the page never parses the build log itself -- one definition of the phase
 * model, and the dock cannot disagree with the step list. */
function paint(progress,kind){
  if(!progress) return;
  const done=new Set(progress.completed||[]);
  steps.forEach(step=>{
    const id=step.dataset.step;
    if(done.has(id)) step.dataset.state="done";
    else if(id===progress.phaseId) step.dataset.state=(kind==="failed"?"failed":"active");
    else step.removeAttribute("data-state");
  });
  const percent=Math.max(0,Math.min(100,progress.percent|0));
  dockPhase.textContent=progress.phaseLabel||"";
  dockPct.textContent=percent+"%";
  dockDetail.textContent=progress.detail||"";
  bar.style.width=percent+"%";
  bar.classList.remove("indeterminate");
  track.setAttribute("aria-valuenow",percent);
}

/* A finished build must be unmissable without hijacking the reader's tab: the
 * dock announces it (and offers Launch inline), and the Build tab gets a dot. */
function announce(kind,message){
  dock.dataset.kind=kind;
  dock.dataset.open="true";
  if(message) dockPhase.textContent=message;
  if(document.querySelector("#panel-build").hidden)
    buildTab.dataset.badge=(kind==="failed"?"bad":"ok");
}

async function responseJSON(response){ const body=await response.json(); if(!response.ok) throw new Error(body.error||"Request failed"); return body; }

async function refresh(){
  try {
    const data=await responseJSON(await fetch("status",{cache:"no-store"}));
    if(data.log){ log.textContent=data.log; log.scrollTop=log.scrollHeight; }
    applyMode(data);
    paint(data.progress,data.state);
    if(data.state==="building"){
      show("building","Building — this can take a few minutes");
      build.disabled=true; launch.disabled=true;
      dock.dataset.kind="building"; dock.dataset.open="true";
    }
    if(data.state==="succeeded"){
      show("succeeded",data.message||"Build complete");
      build.disabled=false; launch.disabled=false; polling=false;
      dockPct.textContent="100%"; bar.style.width="100%";
      announce("succeeded",data.message||"Build complete");
    }
    if(data.state==="failed"){
      show("failed",data.error||"Build failed"); build.disabled=false; launch.disabled=true; polling=false;
      logBox.open=true;  /* a failure is the one time the log matters unprompted */
      announce("failed",data.error||"Build failed");
    }
    if(data.state==="idle"){
      /* The original copy assumed nothing was built. Say what is actually
       * true, so a returning user is not told to choose a ROM they do not
       * need. */
      const install=data.install||{};
      if(data.mode==="launcher") show("idle","Ready to play");
      else if(install.canLaunch){ show("idle","A built game is ready — Play, or rebuild below"); launch.disabled=false; }
      else if(install.canRebuild) show("idle","Ready to build — choose your ROM above");
    }
  } catch(error) { show("failed",error.message); polling=false; announce("failed",error.message); }
  if(polling) setTimeout(refresh,500);
}

form.addEventListener("submit",async event=>{
  event.preventDefault();
  if(!document.querySelector("#rom").files.length) return;
  build.disabled=true; launch.disabled=true; log.textContent="Preparing local ROM copy…";
  show("building","Starting build");
  steps.forEach(step=>step.removeAttribute("data-state"));
  stepsBox.hidden=false;   /* only worth showing once there is progress to show */
  buildTab.removeAttribute("data-badge");
  dock.dataset.kind="building"; dock.dataset.open="true";
  dockPhase.textContent="Preparing your ROM"; dockPct.textContent="0%"; dockDetail.textContent="";
  bar.classList.add("indeterminate");
  try { await responseJSON(await fetch("build",{method:"POST",body:new FormData(form)})); polling=true; refresh(); }
  catch(error){ show("failed",error.message); build.disabled=false; announce("failed",error.message); }
});

async function doLaunch(){
  try { await responseJSON(await fetch("launch",{method:"POST"})); show("succeeded","Game launched"); dockPhase.textContent="Game launched"; }
  catch(error){ show("failed",error.message); }
}
launch.addEventListener("click",doLaunch);
dockLaunch.addEventListener("click",doLaunch);
playButton.addEventListener("click",doLaunch);

slimDismiss.addEventListener("click",()=>{ slimDismissed=true; slimBox.hidden=true; });
slimButton.addEventListener("click",async()=>{
  slimButton.disabled=true; slimDismiss.disabled=true;
  show("building","Removing build tools…");
  try {
    await responseJSON(await fetch("slim",{method:"POST"}));
    /* One poll settles everything: the server has re-probed, so the offer
     * disappears, the confirmation appears, and the page drops into launcher
     * mode without the page having to guess any of it. */
    await refresh();
    show("succeeded","Build tools removed");
  } catch(error){ show("failed",error.message); }
  slimButton.disabled=false; slimDismiss.disabled=false;
});

/* First paint: ask the server what this copy can do before showing anything, so
 * a returning user never sees the build form flash past on the way to a
 * launcher. */
refresh();
closeButton.addEventListener("click",async()=>{
  try {
    await responseJSON(await fetch("close",{method:"POST"}));
    show("idle","Builder closed — you can close this tab");
    build.disabled=true; launch.disabled=true; closeButton.disabled=true;
    dock.dataset.open="false";
  } catch(error){ show("failed",error.message); }
});
</script>
</body>
</html>`
