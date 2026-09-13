package main

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
	"unsafe"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
	"golang.org/x/sys/windows"
)

func TestStartupInstanceIdentity(t *testing.T) {
	name := startupInstanceName(`C:\Builder Data\workspace`, "user-a")
	if name != startupInstanceName(`c:/builder data/other/../workspace/`, "user-a") {
		t.Fatal("case/separator aliases bypass the startup guard")
	}
	if name == startupInstanceName(`D:\Builder Data\workspace`, "user-a") || name == startupInstanceName(`C:\Builder Data\workspace`, "user-b") {
		t.Fatal("unrelated workspace or user shares a guard")
	}
	if unsafe.Sizeof(splashClass{}) != 80 || unsafe.Sizeof(splashMessage{}) != 48 {
		t.Fatal("incorrect Win64 window/message ABI")
	}
}

// Opt-in because this really opens native windows. No WebView2, packaged
// payload, network, compiler or ROM is needed; run on a Windows desktop session.
func TestNativeStartupSplashLifecycle(t *testing.T) {
	if os.Getenv("AR_BUILDER_TEST_SPLASH") != "1" {
		t.Skip("set AR_BUILDER_TEST_SPLASH=1 on a Windows desktop")
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	workspace := t.TempDir()
	window, err := beginStartup(workspace, cancel)
	if err != nil {
		t.Fatal(err)
	}
	defer window.Close()
	s := window.(*nativeStartup)
	s.mu.Lock()
	hwnd := s.hwnd
	s.mu.Unlock()
	if splashCall("IsWindowVisible", hwnd) == 0 {
		t.Fatal("startup returned before showing the splash")
	}
	window.Update(winbundle.Progress{Stage: "Testing startup progress", Completed: 42, Total: 100})
	waitStartup(t, func() bool { return strings.Contains(startupControlText(s.status), "42%") })
	second, err := beginStartup(workspace, func() { t.Error("duplicate cancelled primary") })
	if second != nil || !errors.Is(err, errAlreadyRunning) {
		t.Fatalf("duplicate launch: %v %v", second, err)
	}
	checkSecondStartupProcess(t, workspace)
	// Close button requests cancellation but keeps the progress window/guard
	// alive until the extraction worker has cleaned up and calls Close.
	splashCall("PostMessageW", hwnd, wmClose, 0, 0)
	waitStartup(t, func() bool { return ctx.Err() != nil })
	waitStartup(t, func() bool { return strings.Contains(startupControlText(s.status), "cleaning up") })
	if splashCall("IsWindowVisible", hwnd) == 0 {
		t.Fatal("hid splash before cancellation cleanup")
	}
	window.Close()
	window.Close() // Idempotent; guard/window must not leak after shutdown.
	if splashCall("IsWindow", hwnd) != 0 {
		t.Fatal("startup window leaked")
	}

	window, err = beginStartup(workspace, func() { t.Error("handoff cancelled startup") })
	if err != nil {
		t.Fatalf("guard survived clean exit: %v", err)
	}
	defer window.Close()
	s = window.(*nativeStartup)
	s.mu.Lock()
	hwnd = s.hwnd
	s.mu.Unlock()
	window.Handoff()
	waitStartup(t, func() bool { return splashCall("IsWindowVisible", hwnd) == 0 })
	if splashCall("IsWindow", hwnd) == 0 {
		t.Fatal("lost activation target on handoff")
	}
	second, err = beginStartup(workspace, func() {})
	if second != nil || !errors.Is(err, errAlreadyRunning) {
		t.Fatalf("handoff released startup guard: %v %v", second, err)
	}
	checkSecondStartupProcess(t, workspace)
	window.Close()
}

func checkSecondStartupProcess(t *testing.T, workspace string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, os.Args[0], "-test.run=^TestNativeStartupSecondProcess$")
	cmd.Env = append(os.Environ(), "AR_BUILDER_TEST_SPLASH_WORKSPACE="+workspace)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("second process: %v\n%s", err, out)
	}
}

func TestNativeStartupSecondProcess(t *testing.T) {
	workspace := os.Getenv("AR_BUILDER_TEST_SPLASH_WORKSPACE")
	if workspace == "" {
		t.Skip("subprocess fixture")
	}
	window, err := beginStartup(workspace, func() {})
	if window != nil {
		window.Close()
	}
	if !errors.Is(err, errAlreadyRunning) {
		t.Fatalf("another process bypassed the startup guard: %v", err)
	}
}

func startupControlText(hwnd uintptr) string {
	buf := make([]uint16, 1024)
	splashCall("GetWindowTextW", hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	return windows.UTF16ToString(buf)
}

func waitStartup(t *testing.T, ready func() bool) {
	t.Helper()
	for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); time.Sleep(10 * time.Millisecond) {
		if ready() {
			return
		}
	}
	t.Fatal("native startup window did not reach the expected state")
}
