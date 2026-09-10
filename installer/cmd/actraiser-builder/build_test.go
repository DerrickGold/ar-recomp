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
