package logretention

import (
	"errors"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func TestInheritedOutputHelper(t *testing.T) {
	if os.Getenv("AR_TEST_INHERITED_LOG") == "1" {
		_, _ = io.Copy(io.Discard, os.Stdin)
		os.Exit(0)
	}
}

func TestBusyRotationLeavesCommandRunnable(t *testing.T) {
	dir := t.TempDir()
	lock, err := lockRotation(filepath.Join(dir, ".ar-log-rotation.lock"))
	if err != nil {
		t.Fatal(err)
	}
	defer lock.Close()
	binary, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(binary, "-test.run=^TestInheritedOutputHelper$")
	cmd.Env = append(os.Environ(), "AR_TEST_INHERITED_LOG=1")
	if log, err := CaptureGame(cmd, dir, "game-"); log != nil || !errors.Is(err, errBusy) {
		t.Fatalf("capture with a busy rotation lock returned %v, %v", log, err)
	}
	if err := cmd.Run(); err != nil {
		t.Fatalf("command failed without a startup log: %v", err)
	}
}

func TestPrunePreservesInheritedLogHandle(t *testing.T) {
	dir := t.TempDir()
	file, err := Create(dir, "game-")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	binary, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(binary, "-test.run=^TestInheritedOutputHelper$")
	cmd.Env = append(os.Environ(), "AR_TEST_INHERITED_LOG=1")
	cmd.Stdout, cmd.Stderr = file, file
	input, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		_ = input.Close()
		_ = cmd.Wait()
	}()
	if err := file.Close(); err != nil {
		t.Fatal(err)
	}
	prune(dir, "game-run-", 0, 0, time.Now().Add(time.Hour))
	if _, err := os.Stat(file.Name()); err != nil {
		t.Fatalf("removed a detached child's active log: %v", err)
	}
	if err := input.Close(); err != nil {
		t.Fatal(err)
	}
	if err := cmd.Wait(); err != nil {
		t.Fatal(err)
	}
	prune(dir, "game-run-", 0, 0, time.Now().Add(time.Hour))
	if _, err := os.Stat(file.Name()); !os.IsNotExist(err) {
		t.Fatalf("closed detached log was retained: %v", err)
	}
}

func TestPrunePreservesOpenLogsAndUnmanagedFiles(t *testing.T) {
	dir := t.TempDir()
	now := time.Now()
	open, err := Create(dir, "game-")
	if err != nil {
		t.Fatal(err)
	}
	defer open.Close()
	write := func(name string, size int, stamp time.Time) {
		t.Helper()
		path := filepath.Join(dir, name)
		if err := os.WriteFile(path, make([]byte, size), 0600); err != nil {
			t.Fatal(err)
		}
		if err := os.Chtimes(path, stamp, stamp); err != nil {
			t.Fatal(err)
		}
	}
	write("game-run-999.log", 90, now.Add(-time.Minute))
	write("game-run-998.log", 4, now.Add(-2*time.Minute))
	write("game-run-997.log", 4, now.Add(-3*time.Minute))
	write("game-run-996.log", 4, now.Add(-48*time.Hour))
	write("game-custom.log", 80, now.Add(-48*time.Hour))
	write("launch-run-995.log", 80, now.Add(-48*time.Hour))

	prune(dir, "game-run-", 2, 16, now.Add(-24*time.Hour))
	assert := func(path string, exists bool) {
		t.Helper()
		_, err := os.Stat(path)
		if (err == nil) != exists {
			t.Errorf("exists(%s) wanted %v, got %v", path, exists, err)
		}
	}
	assert(filepath.Join(dir, "game-run-999.log"), false)
	assert(filepath.Join(dir, "game-run-998.log"), true)
	assert(filepath.Join(dir, "game-run-997.log"), false)
	assert(filepath.Join(dir, "game-run-996.log"), false)
	assert(filepath.Join(dir, "game-custom.log"), true)
	assert(filepath.Join(dir, "launch-run-995.log"), true)
	prune(dir, "game-run-", 0, 0, now.Add(time.Hour))
	assert(open.Name(), true)
	if err := open.Close(); err != nil {
		t.Fatal(err)
	}
	prune(dir, "game-run-", 0, 0, now.Add(time.Hour))
	assert(open.Name(), false)
}
