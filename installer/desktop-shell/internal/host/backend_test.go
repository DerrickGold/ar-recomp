package host

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

// Re-exec this test binary in place of the build driver, on every target OS.
// The fake backend owns a helper and must wait for its cleanup before exiting.
func TestMain(m *testing.M) {
	if mode := os.Getenv("AR_HOST_CLOSE_TEST_HELPER"); mode != "" {
		if err := runCloseTestHelper(mode); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		os.Exit(0)
	}
	os.Exit(m.Run())
}

func runCloseTestHelper(mode string) error {
	// Keep a broken test from leaving unbounded subprocesses behind.
	time.AfterFunc(20*time.Second, func() { os.Exit(2) })
	if len(os.Args) > 1 && os.Args[1] == "close-test-worker" {
		if _, err := io.Copy(io.Discard, os.Stdin); err != nil {
			return err
		}
		// Longer than the old host's three-second kill timeout, but within
		// the backend's real five-second HTTP-drain allowance.
		time.Sleep(3250 * time.Millisecond)
		return os.WriteFile("helper-stopped", []byte("clean"), 0600)
	}
	var ready string
	for i := 1; i+1 < len(os.Args); i++ {
		if os.Args[i] == "--ready-file" {
			ready = os.Args[i+1]
			break
		}
	}
	if ready == "" {
		return errors.New("missing --ready-file")
	}
	if err := os.WriteFile("backend-started", []byte(filepath.Dir(ready)), 0600); err != nil {
		return err
	}
	if mode == "starting" {
		select {} // Parent must cancel a process that has not published readiness.
	}
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	worker := exec.Command(executable, "close-test-worker")
	input, err := worker.StdinPipe()
	if err != nil {
		return err
	}
	if err := worker.Start(); err != nil {
		input.Close()
		return err
	}
	defer func() { input.Close(); _ = worker.Process.Kill(); _ = worker.Wait() }()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	defer listener.Close()
	closed := make(chan struct{})
	var once sync.Once
	server := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "POST" || r.URL.Path != "/0123456789abcdef0123456789abcdef0123/close" {
			http.NotFound(w, r)
			return
		}
		w.WriteHeader(http.StatusOK)
		once.Do(func() { close(closed) })
	})}
	go server.Serve(listener)
	defer server.Close()
	descriptor, _ := json.Marshal(map[string]any{"schema": 1, "url": "http://" + listener.Addr().String() + "/0123456789abcdef0123456789abcdef0123/"})
	if err := os.WriteFile(ready, descriptor, 0600); err != nil {
		return err
	}
	<-closed
	if err := input.Close(); err != nil {
		return err
	}
	if err := worker.Wait(); err != nil {
		return err
	}
	return os.WriteFile("backend-stopped", []byte("clean"), 0600)
}

func backendProcessFixture(t *testing.T, mode string) string {
	t.Helper()
	t.Setenv("AR_HOST_CLOSE_TEST_HELPER", mode)
	payload := t.TempDir()
	path := filepath.Join(payload, "utils", "tools", executableName("actraiser-builder", runtime.GOOS))
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	in, err := os.Open(executable)
	if err != nil {
		t.Fatal(err)
	}
	defer in.Close()
	out, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0700)
	if err != nil {
		t.Fatal(err)
	}
	_, copyErr := io.Copy(out, in)
	closeErr := out.Close()
	if copyErr != nil {
		t.Fatal(copyErr)
	}
	if closeErr != nil {
		t.Fatal(closeErr)
	}
	return payload
}

func TestBackendShutdownWaitsForOwnedHelperCleanup(t *testing.T) {
	payload := backendProcessFixture(t, "ready")
	work, output := t.TempDir(), t.TempDir()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	b, err := StartBundledBackend(ctx, work, payload, strings.Repeat("e", 64), output, 1)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(b.Stop)
	// Reproduce OnBeforeClose -> OnShutdown: approve via HTTP, cancel the
	// startup context, then stop. Cancellation must not kill a ready backend.
	if err := b.RequestClose(); err != nil {
		t.Fatal(err)
	}
	cancel()
	var stopped sync.WaitGroup
	for range 3 {
		stopped.Go(b.Stop)
	}
	stopped.Wait()
	if !b.exited() || !b.command.ProcessState.Success() {
		t.Fatalf("backend did not exit cleanly: %v", b.command.ProcessState)
	}
	for _, marker := range []string{"helper-stopped", "backend-stopped"} {
		if data, err := os.ReadFile(filepath.Join(work, marker)); err != nil || string(data) != "clean" {
			t.Fatalf("shutdown interrupted %s: %q, %v", marker, data, err)
		}
	}
	if _, err := os.Stat(b.sessionDir); !os.IsNotExist(err) {
		t.Fatalf("session directory remains: %v", err)
	}
	b.Stop() // Safe even after logs and the private directory have been removed.
}

func TestBackendStartupCancellationReapsProcess(t *testing.T) {
	payload := backendProcessFixture(t, "starting")
	work, output := t.TempDir(), t.TempDir()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	finished := make(chan error, 1)
	go func() {
		_, err := StartBundledBackend(ctx, work, payload, strings.Repeat("e", 64), output, 1)
		finished <- err
	}()
	var private []byte
	for len(private) == 0 {
		select {
		case err := <-finished:
			t.Fatalf("backend stopped before cancellation: %v", err)
		case <-time.After(10 * time.Millisecond):
			private, _ = os.ReadFile(filepath.Join(work, "backend-started"))
		}
	}
	cancel()
	select {
	case err := <-finished:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("cancellation: %v", err)
		}
	case <-time.After(4 * time.Second):
		t.Fatal("startup cancellation did not reap the backend promptly")
	}
	if _, err := os.Stat(string(private)); !os.IsNotExist(err) {
		t.Fatalf("cancelled session remains: %v", err)
	}
}

func TestBackendAlreadyCancelledDoesNotStart(t *testing.T) {
	work := filepath.Join(t.TempDir(), "not-created")
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := StartBundledBackend(ctx, work, "", strings.Repeat("e", 64), "", 0); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancellation: %v", err)
	}
	if _, err := os.Stat(work); !os.IsNotExist(err) {
		t.Fatalf("created a workspace despite cancellation: %v", err)
	}
}
