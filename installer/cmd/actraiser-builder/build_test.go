package main

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

func TestSnesbuildClosedStdoutHelper(t *testing.T) {
	if os.Getenv("AR_CLOSED_STDOUT_HELPER") != "1" {
		return
	}
	_ = os.Stdout.Close()
	_ = os.WriteFile(os.Getenv("AR_CLOSED_STDOUT_READY"), []byte("ready"), 0600)
	time.Sleep(20 * time.Second)
	os.Exit(0)
}

func TestSnesbuildCancellationAfterStdoutCloses(t *testing.T) {
	root := t.TempDir()
	ready := filepath.Join(root, "ready")
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() {
		_, err := runSnesbuildAt(ctx, os.Args[0], root,
			append(os.Environ(), "AR_CLOSED_STDOUT_HELPER=1", "AR_CLOSED_STDOUT_READY="+ready),
			&bytes.Buffer{}, "-test.run=^TestSnesbuildClosedStdoutHelper$", "--")
		done <- err
	}()
	deadline := time.Now().Add(5 * time.Second)
	for {
		if _, err := os.Stat(ready); err == nil {
			break
		}
		if time.Now().After(deadline) {
			cancel()
			<-done
			t.Fatal("helper did not close stdout")
		}
		time.Sleep(10 * time.Millisecond)
	}
	cancel()
	select {
	case err := <-done:
		if err != context.Canceled {
			t.Fatalf("cancellation = %v", err)
		}
	case <-time.After(4 * time.Second):
		t.Fatal("cancellation hung after stdout closed")
	}
}

func TestEventStreamRejectsMalformedAndUnsupportedEvents(t *testing.T) {
	for _, testCase := range []struct {
		name, stream, want string
	}{
		{"malformed", "not-json\n", "malformed"},
		{"schema", `{"schema":"other","version":1,"type":"phase","phase":"regen"}` + "\n", "unsupported"},
		{"version", `{"schema":"snesbuild-event","version":2,"type":"phase","phase":"regen"}` + "\n", "version 2"},
		{"type", `{"schema":"snesbuild-event","version":1,"type":"mystery"}` + "\n", "unknown"},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			var output bytes.Buffer
			err := scanEventStream(strings.NewReader(testCase.stream), &output,
				&commandResult{Artifacts: make(map[string][]string)})
			if err == nil || !strings.Contains(err.Error(), testCase.want) {
				t.Fatalf("error = %v, want %q", err, testCase.want)
			}
		})
	}
}

func TestDiscoverSnesbuildReportsMissingExplicitDriver(t *testing.T) {
	missing := filepath.Join(t.TempDir(), "missing-snesbuild")
	if runtime.GOOS == "windows" {
		missing += ".exe"
	}
	_, err := discoverSnesbuild(missing, t.TempDir())
	if err == nil || !strings.Contains(err.Error(), "build driver") ||
		!strings.Contains(err.Error(), "unavailable") {
		t.Fatalf("missing explicit driver error = %v", err)
	}
}

func TestRunSnesbuildCompletionFailureAndCancellation(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell fixture is POSIX-only")
	}
	dir := t.TempDir()
	artifact := filepath.Join(dir, "game")
	script := filepath.Join(dir, "fake-snesbuild")
	writeScript := func(body string) {
		t.Helper()
		if err := os.WriteFile(script, []byte("#!/bin/sh\n"+body), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	writeScript(fmt.Sprintf("printf '%%s\\n' '%s'\n", fmt.Sprintf(
		`{"schema":"snesbuild-event","version":1,"type":"artifact","kind":"game-binary","path":%q}`,
		artifact)))
	result, err := runSnesbuild(context.Background(), script, &bytes.Buffer{}, "build")
	if err != nil {
		t.Fatal(err)
	}
	if got, err := oneArtifact(result, "game-binary"); err != nil || got != artifact {
		t.Fatalf("artifact = %q, %v", got, err)
	}

	writeScript("exit 7\n")
	if _, err := runSnesbuild(context.Background(), script, &bytes.Buffer{}, "build"); err == nil {
		t.Fatal("nonzero exit accepted")
	}

	writeScript("sleep 30 &\nwait\n")
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	started := time.Now()
	if _, err := runSnesbuild(ctx, script, &bytes.Buffer{}, "build"); err == nil {
		t.Fatal("cancelled process reported success")
	}
	if time.Since(started) > 3*time.Second {
		t.Fatal("cancellation did not stop the process tree promptly")
	}
}

func TestPrepareBuildToolchainUsesAvailableCompilerBeforeFetching(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("shell fixture is POSIX-only")
	}
	for _, tc := range []struct {
		name, available, override, want string
		wantError                       bool
	}{
		{"bundled compiler is offline", "1", "", "status\n", false},
		{"source checkout fetches missing compiler", "0", "", "status\nfetch\n", false},
		{"broken override does not download", "0", "/missing/zig", "status\n", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			calls := filepath.Join(dir, "calls")
			script := filepath.Join(dir, "snesbuild")
			t.Setenv("TEST_TOOLCHAIN_CALLS", calls)
			t.Setenv("TEST_TOOLCHAIN_AVAILABLE", tc.available)
			t.Setenv("SNESBUILD_ZIG", tc.override)
			body := `#!/bin/sh
printf '%s\n' "$2" >> "$TEST_TOOLCHAIN_CALLS"
if [ "$2" = status ] && [ "$TEST_TOOLCHAIN_AVAILABLE" != 1 ]; then exit 1; fi
printf '%s\n' '{"schema":"snesbuild-event","version":1,"type":"artifact","kind":"toolchain","path":"/fixture/bundled-zig"}'
`
			if err := os.WriteFile(script, []byte(body), 0755); err != nil {
				t.Fatal(err)
			}
			err := prepareBuildToolchain(context.Background(), script, dir, &bytes.Buffer{})
			if (err != nil) != tc.wantError {
				t.Fatalf("prepare toolchain: %v", err)
			}
			got, err := os.ReadFile(calls)
			if err != nil || string(got) != tc.want {
				t.Fatalf("commands = %q, %v; want %q", got, err, tc.want)
			}
		})
	}
}
