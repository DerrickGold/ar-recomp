package host

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/internal/builder"
)

// Opt-in player acceptance: uses a fresh, manifest-bearing desktop payload and
// the tester's private ROM. Never packages that ROM into a public Builder.
// chmod a disposable payload read-only first to enforce the input boundary.
func TestBundledPlayerBuildColdWarmAndIndependentLaunch(t *testing.T) {
	payload, rom := os.Getenv("AR_TEST_BUNDLED_PAYLOAD"), os.Getenv("AR_TEST_BUNDLED_ROM")
	if payload == "" || rom == "" {
		t.Skip("requires AR_TEST_BUNDLED_PAYLOAD and AR_TEST_BUNDLED_ROM")
	}
	if runtime.GOOS != "darwin" {
		t.Skip("native macOS player acceptance; Linux AppImage acceptance is separate")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Minute)
	defer cancel()
	parent := t.TempDir()
	work, output := filepath.Join(parent, "workspace"), filepath.Join(parent, "Game Output")
	t.Cleanup(func() {
		if t.Failed() {
			logs, _ := filepath.Glob(filepath.Join(work, "logs", "*.log"))
			for _, path := range logs {
				data, _ := os.ReadFile(path)
				if len(data) > 16000 {
					data = data[len(data)-16000:]
				}
				t.Logf("backend log %s:\n%s", path, data)
			}
		}
	})
	id, err := PrepareSession(ctx, payload, work, nil)
	if err != nil {
		t.Fatal(err)
	}
	b, err := StartBundledBackend(ctx, work, payload, id, output, 4)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(b.Stop)
	client := &http.Client{Timeout: 10 * time.Second}
	var status struct {
		State, Log, Error string
		Install           builder.InstallState
	}
	build := func(warm bool) {
		t.Helper()
		var body bytes.Buffer
		form := multipart.NewWriter(&body)
		part, err := form.CreateFormFile("rom", "private-test.sfc")
		if err != nil {
			t.Fatal(err)
		}
		input, err := os.Open(rom)
		if err != nil {
			t.Fatal(err)
		}
		_, err = io.Copy(part, input)
		input.Close()
		if err != nil {
			t.Fatal(err)
		}
		if err := form.Close(); err != nil {
			t.Fatal(err)
		}
		response, err := client.Post(b.URL.String()+"build", form.FormDataContentType(), &body)
		if err != nil {
			t.Fatal(err)
		}
		data, _ := io.ReadAll(response.Body)
		response.Body.Close()
		if response.StatusCode != http.StatusAccepted {
			t.Fatalf("start build: %d %s", response.StatusCode, data)
		}
		nextProgress := time.Now()
		for {
			if ctx.Err() != nil {
				t.Fatal(ctx.Err())
			}
			response, err := client.Get(b.URL.String() + "status")
			if err != nil {
				t.Fatal(err)
			}
			err = json.NewDecoder(response.Body).Decode(&status)
			response.Body.Close()
			if err != nil {
				t.Fatal(err)
			}
			if status.State == "failed" {
				t.Fatalf("build failed: %s\n%s", status.Error, status.Log)
			}
			if status.State == "succeeded" {
				break
			}
			if time.Now().After(nextProgress) {
				lines := strings.Split(strings.TrimSpace(status.Log), "\n")
				t.Logf("warm=%v: %s", warm, lines[len(lines)-1])
				nextProgress = time.Now().Add(30 * time.Second)
			}
			select {
			case <-ctx.Done():
				t.Fatal(ctx.Err())
			case <-time.After(500 * time.Millisecond):
			}
		}
		if warm && !strings.Contains(status.Log, "cached, 0 to compile") {
			t.Fatalf("warm build recompiled unchanged inputs:\n%s", status.Log)
		}
		if !status.Install.CanLaunch || !status.Install.CanRebuild || status.Install.CanSlim {
			t.Fatalf("capabilities: %+v", status.Install)
		}
		current, err := PrepareSession(ctx, payload, work, nil)
		if err != nil || current != id {
			t.Fatalf("build modified bundled inputs: %s %v", current, err)
		}
	}
	build(false)
	for _, path := range []string{"utils", "src", "recomp", "tools"} {
		if _, err := os.Stat(filepath.Join(work, path)); !os.IsNotExist(err) {
			t.Fatalf("copied build inputs: %s %v", path, err)
		}
	}
	for _, path := range []string{"ActRaiserRecomp", "run-game.command", "run-game.sh", "tools", "src", "recomp"} {
		if _, err := os.Stat(filepath.Join(output, path)); !os.IsNotExist(err) {
			t.Fatalf("redundant player output: %s %v", path, err)
		}
	}
	settings := filepath.Join(output, "settings.ini")
	if err := os.WriteFile(settings, []byte("; player settings sentinel\n"), 0600); err != nil {
		t.Fatal(err)
	}
	build(true)
	if data, err := os.ReadFile(settings); err != nil || string(data) != "; player settings sentinel\n" {
		t.Fatalf("settings overwritten: %s %v", data, err)
	}
	b.Stop()
	if !b.command.ProcessState.Success() {
		t.Fatalf("backend did not stop: %v", b.command.ProcessState)
	}
	// Restart detects the app without needing a loose binary or remembered
	// AppImage mount path. It still must not seed/copy source into the workspace.
	b, err = StartBundledBackend(ctx, work, payload, id, output, 4)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(b.Stop)
	response, err := client.Get(b.URL.String() + "status")
	if err != nil {
		t.Fatal(err)
	}
	err = json.NewDecoder(response.Body).Decode(&status)
	response.Body.Close()
	if err != nil || !status.Install.CanLaunch {
		t.Fatalf("restart lost game: %+v %v", status.Install, err)
	}
	b.Stop()
	moved := filepath.Join(t.TempDir(), "Moved Game")
	if err := os.Rename(output, moved); err != nil {
		t.Fatal(err)
	}
	app := filepath.Join(moved, "ActRaiserRecomp.app")
	helper := filepath.Join(app, "Contents", "MacOS", "actraiser-builder")
	probe := exec.CommandContext(ctx, helper, "app-launch", "--font-coverage-v1", filepath.Join(moved, "game-assets", "fonts", "noto", "NotoSans-SemiCondensedExtraBold.ttf"))
	probe.Stdin = strings.NewReader("0041\n")
	probe.Dir = t.TempDir()
	data, err := probe.CombinedOutput()
	if err != nil || string(data) != "0041\t1\n" {
		t.Fatalf("installed font protocol: %q %v", data, err)
	}
	launchCtx, stop := context.WithTimeout(ctx, 30*time.Second)
	defer stop()
	launch := exec.CommandContext(launchCtx, helper, "app-launch")
	launch.Dir = t.TempDir()
	launch.Env = append(os.Environ(), "AR_USER_DATA_DIR=", "AR_HEADLESS=1", "AR_QUIT_FRAMES=2", "SDL_AUDIODRIVER=dummy")
	if data, err := launch.CombinedOutput(); err != nil {
		t.Fatalf("standalone portable game: %s %v", data, err)
	}
	t.Log("PASS: cold/warm game builds, unchanged payload, preserved settings, restart detection, relocated font protocol and independent headless launch")
}
