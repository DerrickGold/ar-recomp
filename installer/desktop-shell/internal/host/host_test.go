package host

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func fixture(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	for _, p := range requiredFiles(runtime.GOOS) {
		path := filepath.Join(root, p)
		if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("fixture"), 0755); err != nil {
			t.Fatal(err)
		}
	}
	if err := WriteManifest(root, runtime.GOOS, runtime.GOARCH); err != nil {
		t.Fatal(err)
	}
	return root
}
func TestRejectPrivateFilesAndEscapingPaths(t *testing.T) {
	for _, leaf := range []string{"utils/user-rom.sfc", "utils/ar.smc", "utils/settings.ini", "utils/src/gen/bank00.c", "utils/game-assets/manifest.ini", "._utils", "utils/.DS_Store"} {
		t.Run(leaf, func(t *testing.T) {
			root := fixture(t)
			path := filepath.Join(root, leaf)
			os.MkdirAll(filepath.Dir(path), 0700)
			os.WriteFile(path, []byte("private"), 0600)
			if err := WriteManifest(root, runtime.GOOS, runtime.GOARCH); err == nil {
				t.Fatal("private input accepted")
			}
		})
	}
	for _, path := range []string{"../escape", "/absolute", "a/../../escape", "a/./b", "a\\b"} {
		if localPath(path) {
			t.Fatal(path)
		}
	}
	root := fixture(t)
	data, _ := os.ReadFile(filepath.Join(root, manifestName))
	var m Manifest
	json.Unmarshal(data, &m)
	m.Files = append(m.Files, File{Path: "../escape", SHA256: fmt.Sprintf("%x", sha256.Sum256(nil)), Mode: 0600})
	if validateManifest(m) == nil {
		t.Fatal("escape accepted")
	}
}
func TestWorkspaceLock(t *testing.T) {
	work := t.TempDir()
	release, err := Lock(work)
	if err != nil {
		t.Fatal(err)
	}
	defer release()
	if second, err := Lock(work); err == nil {
		second()
		t.Fatal("concurrent session accepted")
	}
}

func TestWorkspaceNeverWritesInsideApplication(t *testing.T) {
	app := t.TempDir()
	alias := filepath.Join(t.TempDir(), "alias")
	if err := os.Symlink(app, alias); err != nil {
		t.Fatal(err)
	}
	for _, work := range []string{app, filepath.Join(app, "Contents", "new", "work"), filepath.Join(alias, "new", "work")} {
		if err := ValidateWorkspace(work, app); err == nil {
			t.Fatalf("accepted workspace inside app: %s", work)
		}
	}
	entries, err := os.ReadDir(app)
	if err != nil || len(entries) != 0 {
		t.Fatalf("validation mutated app: %v %v", entries, err)
	}
	if err := ValidateWorkspace(filepath.Join(t.TempDir(), "new", "work"), app); err != nil {
		t.Fatal(err)
	}
	if err := ValidateWorkspace("relative", app); err == nil {
		t.Fatal("accepted relative workspace")
	}
}

func TestPortableAndGlobalWorkspace(t *testing.T) {
	base := t.TempDir()
	t.Setenv("HOME", base)
	t.Setenv("XDG_DATA_HOME", filepath.Join(base, "data"))
	t.Setenv("LOCALAPPDATA", filepath.Join(base, "local"))
	artifact := filepath.Join(base, Name+".app")
	global, err := DefaultWorkspace(artifact, "")
	if err != nil {
		t.Fatal(err)
	}
	want := filepath.Join(base, "Library", "Application Support", "ActRaiserRecomp", "installer", "workspace")
	if runtime.GOOS == "linux" {
		want = filepath.Join(base, "data", "ActRaiserRecomp", "installer", "workspace")
	} else if runtime.GOOS == "windows" {
		want = filepath.Join(base, "local", "ActRaiserRecomp", "installer", "workspace")
	}
	if global != want {
		t.Fatalf("global workspace: %s, want %s", global, want)
	}
	if got := AuxiliaryDirectory(global, "runtime"); got != filepath.Join(filepath.Dir(global), "runtime") {
		t.Fatalf("global runtime outside installer namespace: %s", got)
	}
	marker := artifact + ".portable"
	if err = os.WriteFile(marker, []byte("BuilderData\n"), 0600); err != nil {
		t.Fatal(err)
	}
	portable, err := DefaultWorkspace(artifact, "")
	if err != nil || portable != filepath.Join(base, "BuilderData") {
		t.Fatalf("portable workspace: %s, %v", portable, err)
	}
	if got := AuxiliaryDirectory(portable, "webview"); got != portable+"-webview" {
		t.Fatalf("portable profile layout changed: %s", got)
	}
	for _, invalid := range []string{"", ".", "../escape", "/absolute"} {
		if err = os.WriteFile(marker, []byte(invalid), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err = DefaultWorkspace(artifact, ""); err == nil {
			t.Fatalf("accepted invalid sidecar %q", invalid)
		}
	}
	if actual, err := DefaultWorkspace(artifact, filepath.Join(base, "explicit")); err != nil || actual != filepath.Join(base, "explicit") {
		t.Fatalf("explicit workspace: %s, %v", actual, err)
	}
}
func TestBridgePreservesRelativeRequestsAndHidesBackendToken(t *testing.T) {
	token := "/" + strings.Repeat("a", 36) + "/"
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != token+"status" || r.Method != "POST" {
			t.Errorf("unexpected backend request %s %s", r.Method, r.URL.Path)
		}
		w.Write([]byte("okay"))
	}))
	defer server.Close()
	address, _ := url.Parse(server.URL + token)
	bridge := &Bridge{}
	bridge.Connect(address)
	w := httptest.NewRecorder()
	bridge.ServeHTTP(w, httptest.NewRequest("POST", "http://wails.localhost/status", strings.NewReader("body")))
	if w.Code != 200 || w.Body.String() != "okay" {
		t.Fatal(w)
	}
	w = httptest.NewRecorder()
	bridge.ServeHTTP(w, httptest.NewRequest("GET", "http://wails.localhost/__shell/status", nil))
	if strings.Contains(w.Body.String(), token) || !strings.Contains(w.Body.String(), `"ready":true`) {
		t.Fatal(w.Body.String())
	}
}
func TestSessionURLMustBePrivateLoopback(t *testing.T) {
	good := "http://127.0.0.1:12345/" + strings.Repeat("a", 36) + "/"
	if _, err := ValidateAddress(1, good); err != nil {
		t.Fatal(err)
	}
	for _, bad := range []string{strings.Replace(good, "127.0.0.1", "example.com", 1), good + "?redirect=x", strings.Replace(good, "http:", "file:", 1), "http://127.0.0.1:1234/short/"} {
		if _, err := ValidateAddress(1, bad); err == nil {
			t.Fatal(bad)
		}
	}
}

func TestBackendDoesNotInheritGUIRuntimeOverrides(t *testing.T) {
	input := []string{"PATH=/usr/bin", "DISPLAY=:1", "LD_LIBRARY_PATH=/app/lib", "APPDIR=/app", "GIO_MODULE_DIR=/app/gio", "GSETTINGS_SCHEMA_DIR=/app/schemas", "GTK_IM_MODULE_FILE=/temp/gtk", "GST_PLUGIN_SYSTEM_PATH_1_0=/app/gst", "XDG_DATA_DIRS=/app/share:/user/data", "AR_BUILDER_HOST_XDG_DATA_DIRS=/user/data", "AR_BUILDER_HOST_XDG_DATA_DIRS_MODE=set"}
	got := strings.Join(backendEnvironment(input), "\n")
	if strings.Contains(got, "/app") || strings.Contains(got, "AR_BUILDER_HOST_") || strings.Contains(got, "/temp/gtk") {
		t.Fatal(got)
	}
	for _, want := range []string{"PATH=/usr/bin", "DISPLAY=:1", "XDG_DATA_DIRS=/user/data"} {
		if !strings.Contains(got, want) {
			t.Fatalf("lost %s: %s", want, got)
		}
	}
	got = strings.Join(backendEnvironment([]string{"XDG_DATA_DIRS=/app/share", "AR_BUILDER_HOST_XDG_DATA_DIRS_MODE=unset"}), "\n")
	if strings.Contains(got, "XDG_DATA_DIRS=") {
		t.Fatal(got)
	}
	got = strings.Join(backendEnvironment([]string{"XDG_DATA_DIRS=/untouched"}), "\n")
	if got != "XDG_DATA_DIRS=/untouched" {
		t.Fatal(got)
	}
	got = strings.Join(backendEnvironment([]string{"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER=C:\\runtime", "WebView2_USER_DATA_FOLDER=C:\\profile", "PATH=C:\\Windows"}), "\n")
	if got != "PATH=C:\\Windows" {
		t.Fatal(got)
	}
}

func TestWindowsPackagePathRulesOnEveryHost(t *testing.T) {
	for _, path := range []string{"NUL", "con.txt", "a/LPT1.h", "a/COM¹", "a/CONOUT$", "a/file:stream", "a/ending.", "a/ending ", "a/<file>", "a/what?", "a/back\\slash", "../escape"} {
		if WindowsPath(path) {
			t.Fatalf("unsafe Windows path %q", path)
		}
	}
	for _, path := range []string{"utils/tools/builder.exe", "Locales/en-US.pak", "dir with spaces/file.txt", "console.h", "auxiliary.txt"} {
		if !WindowsPath(path) {
			t.Fatalf("valid path rejected: %q", path)
		}
	}
}
