//go:build !windows

package main

import (
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/internal/builder"
)

// The launched game must NOT share the builder's process group.
//
// Sharing it means every signal aimed at the builder also hits the game: Ctrl-C
// in the run-build window, closing that window (SIGHUP), or the builder exiting
// as the pty's group leader. The run-build script keeps that window open for the
// whole session, so this is the ordinary way a user "finishes with the builder" --
// and it took the running game and any unsaved battery state with it.
//
// Asserts the real pgid of a real child, since a paraphrase of the syscall would
// not have caught this: the bug was the ABSENCE of a call, and the code read fine.
func TestLaunchedGameGetsItsOwnProcessGroup(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the stub game is a shell script")
	}
	root := t.TempDir()
	utils := filepath.Join(root, "utils")
	if err := os.MkdirAll(utils, 0o755); err != nil {
		t.Fatal(err)
	}
	record := filepath.Join(root, "pgid.txt")
	// The stub reports its PID and stays alive long enough for the parent to ask
	// the kernel for its process group. Querying it from the test avoids relying
	// on `ps`, which is intentionally unavailable in some build sandboxes.
	stub := "#!/bin/sh\necho $$ > " + record + "\nsleep 30\n"
	binary := filepath.Join(root, "ActRaiserRecomp")
	if err := os.WriteFile(binary, []byte(stub), 0o755); err != nil {
		t.Fatal(err)
	}

	if err := launchBuiltGame(builder.Result{
		BinaryPath: binary, WorkingDir: utils,
	}); err != nil {
		t.Fatalf("launchBuiltGame: %v", err)
	}

	var text string
	for attempt := 0; attempt < 200; attempt++ {
		if data, err := os.ReadFile(record); err == nil && len(data) > 0 {
			text = strings.TrimSpace(string(data))
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if text == "" {
		t.Fatal("the game was never invoked")
	}
	gamePID, err := strconv.Atoi(text)
	if err != nil {
		t.Fatalf("stub reported %q, not a pid: %v", text, err)
	}
	gamePgid, err := syscall.Getpgid(gamePID)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.Kill(-gamePgid, syscall.SIGKILL)
	builderPgid, err := syscall.Getpgid(os.Getpid())
	if err != nil {
		t.Fatal(err)
	}
	if gamePgid == builderPgid {
		t.Fatalf("the game shares the builder's process group (%d), so a Ctrl-C "+
			"or a closed run-build window would kill it", builderPgid)
	}
}
