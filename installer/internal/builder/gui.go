// Package builder serves the dependency-free local browser interface used by
// snesbuild. The server is deliberately loopback-only and hides every endpoint
// behind an unguessable per-process path token.
package builder

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

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
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
	Slim func(io.Writer) error
	// AudioPreviewCacheDir overrides the per-user cache used for ROM-derived
	// WAV previews. It is primarily useful to embedders and tests. Empty uses
	// the operating system cache directory, never game-assets or the manifest.
	AudioPreviewCacheDir string
	openURL              func(string) error
	pickDirectory        func(context.Context) (string, error)
	fontCoverageProbe    lk.FontCoverageProbe
	sessionToken         string
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
	// The manual is already embedded in this executable for the builder's reader.
	// Materialize the same bytes at the game's runtime path, including in launcher
	// mode, without adding a duplicate PDF to the distribution archive.
	if err := materializeBundledManual(root); err != nil {
		return fmt.Errorf("prepare bundled manual: %w", err)
	}
	// Same handoff for the manifest: an install with none gets the template,
	// and one that already has a manifest keeps every entry in it.
	if err := materializeAssetManifest(root); err != nil {
		return fmt.Errorf("prepare asset manifest: %w", err)
	}
	options.ProjectRoot = root
	if options.Title == "" {
		options.Title = "ActRaiser Builder"
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
	// Independent of compilation: extraction publishes the source first.
	LocalizationReady bool   `json:"localizationReady"`
	LocalizationError string `json:"localizationError,omitempty"`
}

type application struct {
	ctx     context.Context
	options Options
	prefix  string
	closed  chan struct{}
	// Asset saves are synchronous, but two browser tabs can still submit at the
	// same time. Serializing the read/merge/install transaction prevents the
	// second request from rebuilding a manifest from stale text.
	assetMu           sync.Mutex
	directoryPickerMu sync.Mutex
	interfaceMu       sync.Mutex
	localization      localizationSession
	previewMu         sync.Mutex
	preview           audioPreviewStatus
	previewPaths      map[string]string
	scenery           scenerySession

	mu           sync.Mutex
	state        string
	log          bytes.Buffer
	errorMessage string
	result       Result
	progress     progress
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
		progress:     initialProgress(),
		preview:      audioPreviewStatus{State: "idle", Total: len(assetTracks)},
		previewPaths: make(map[string]string),
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
	if endpoint == "interface/preferences" {
		app.serveInterfacePreferences(response, request)
		return
	}
	if endpoint == "interface/japanese.otf" && request.Method == http.MethodGet {
		app.serveInterfaceFont(response, request)
		return
	}
	if strings.HasPrefix(endpoint, "builder/") {
		serveFrontend(response, request, endpoint)
		return
	}
	if strings.HasPrefix(endpoint, "localization/") {
		app.serveLocalization(response, request, strings.TrimPrefix(endpoint, "localization/"))
		return
	}
	switch {
	case endpoint == "" && request.Method == http.MethodGet:
		interfaceData, err := app.interfaceBootstrap()
		if err != nil {
			http.Error(response, "interface catalog unavailable", http.StatusInternalServerError)
			return
		}
		response.Header().Set("Content-Type", "text/html; charset=utf-8")
		// The sky, clouds and columns are CSS; the cover art and the manual
		// are served from this same origin, so 'self' covers everything and
		// no remote origin is reachable. `frame-src 'self'` admits the
		// manual's <iframe> (the browser's own PDF viewer) without opening
		// `object-src`, which stays 'none'.
		response.Header().Set("Content-Security-Policy",
			"default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self'; "+
				"connect-src 'self'; img-src 'self'; media-src 'self' blob:; frame-src 'self'; "+
				"object-src 'none'; base-uri 'none'")
		response.Header().Set("Cache-Control", "no-store")
		response.Header().Set("Referrer-Policy", "no-referrer")
		response.Header().Set("X-Content-Type-Options", "nosniff")
		// Replace once over the template, never rescan inserted title/catalog
		// text for another marker that happens to look like template syntax.
		page := strings.NewReplacer(
			"{{TITLE}}", html.EscapeString(app.options.Title),
			"{{STEPS}}", renderStepList(),
			"{{ASSET_TRACKS}}", renderAssetTrackRows(),
			"{{ASSET_ROW_PROTOTYPE}}", renderAssetRowPrototype(),
			"{{LOCALIZATION}}", localizationHTML,
			"{{UI_CATALOG}}", interfaceData).Replace(pageHTML)
		_, _ = io.WriteString(response, page)
	case endpoint == "boxart.webp" && request.Method == http.MethodGet:
		serveBoxArt(response, request)
	case endpoint == "title-logo.png" && request.Method == http.MethodGet:
		serveTitleLogo(response, request)
	case endpoint == "manual.pdf" && request.Method == http.MethodGet:
		serveManual(response, request)
	case endpoint == "scene-assets" && request.Method == http.MethodGet:
		app.serveScenery(response, request, false)
	case endpoint == "scene-atlas.png" && request.Method == http.MethodGet:
		app.serveScenery(response, request, true)
	case endpoint == "assets" && request.Method == http.MethodGet:
		app.writeAssets(response)
	case endpoint == "assets" && request.Method == http.MethodPost:
		app.saveAssets(response, request)
	case endpoint == "audio-previews" && request.Method == http.MethodGet:
		app.writeAudioPreviewStatus(response)
	case endpoint == "audio-previews" && request.Method == http.MethodPost:
		app.startAudioPreviews(response, request)
	case strings.HasPrefix(endpoint, "asset-audio/") && request.Method == http.MethodGet:
		app.serveInstalledAudio(response, request,
			strings.TrimPrefix(endpoint, "asset-audio/"))
	case strings.HasPrefix(endpoint, "audio-preview/") && request.Method == http.MethodGet:
		app.serveAudioPreview(response, request,
			strings.TrimPrefix(endpoint, "audio-preview/"))
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
	current.Progress = app.progress
	if app.state == "succeeded" {
		current.Progress = completedProgress()
	}
	app.mu.Unlock()
	current.Mode = app.pageMode(current.Install)
	current.LocalizationReady, current.LocalizationError = app.localizationAvailability()
	// Only offered while a cleanup is actually possible: no Slim hook means no
	// offer, and an already-lean install has nothing left to remove.
	if app.options.Slim != nil && current.Install.CanSlim {
		current.SlimSize = slimSummary(current.Install.SlimBytes)
	} else {
		current.Install.CanSlim = false
	}
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
	app.progress = initialProgress()
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
		logWriter := &lockedLogWriter{app: app}
		result, buildErr := app.options.Build(app.ctx, romPath, logWriter)
		if buildErr != nil {
			// The failure text goes to the LOG, in full. The status line and
			// the dock get only its first line: an error carrying a compiler's
			// output is many lines long, and both of those are single-line
			// elements that would show a fragment of it and hide the rest with
			// no way to read the remainder anywhere on the page.
			fmt.Fprintf(logWriter, "\n=== Build failed ===\n%s\n", buildErr.Error())
		}
		app.mu.Lock()
		if buildErr != nil {
			app.state = "failed"
			app.errorMessage = firstLine(buildErr.Error())
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

// firstLine is what a one-line element can honestly show of a multi-line error.
// The full text is always in the build log; this is the headline that tells the
// reader which step died, so they know what they are looking at down there.
func firstLine(text string) string {
	trimmed := strings.TrimSpace(text)
	if index := strings.IndexByte(trimmed, '\n'); index >= 0 {
		return strings.TrimSpace(trimmed[:index]) + " — see the build log below"
	}
	return trimmed
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

func (writer *lockedLogWriter) ReportBuildProgress(update BuildProgress) {
	app := writer.app
	app.mu.Lock()
	app.progress = progressFromEvent(app.progress, update)
	app.mu.Unlock()
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
