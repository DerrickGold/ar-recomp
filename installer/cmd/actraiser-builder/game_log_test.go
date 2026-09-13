package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestGameLogHelper(t *testing.T) {
	if os.Getenv("AR_TEST_GAME_LOG") != "1" {
		return
	}
	fmt.Fprintln(os.Stderr, "SDL_Init failed: synthetic video failure")
	os.Exit(42)
}

func TestGameStartupFailureHasShareableLog(t *testing.T) {
	root := t.TempDir()
	command := exec.Command(os.Args[0], "-test.run=^TestGameLogHelper$")
	command.Dir = root
	command.Env = append(os.Environ(), "AR_TEST_GAME_LOG=1")
	detachFromBuilder(command)
	err := startLoggedGame(command, root)
	if err == nil || !strings.Contains(err.Error(), "stopped during startup") {
		t.Fatalf("startup failure = %v", err)
	}
	logs, _ := filepath.Glob(filepath.Join(root, "logs", "game-*.log"))
	if len(logs) != 1 || !strings.Contains(err.Error(), logs[0]) {
		t.Fatalf("missing log path: %v, %v", err, logs)
	}
	raw, readErr := os.ReadFile(logs[0])
	if readErr != nil || !strings.Contains(string(raw), "synthetic video failure") || !strings.Contains(string(raw), "exit status 42") {
		t.Fatalf("log = %s, %v", raw, readErr)
	}
}
