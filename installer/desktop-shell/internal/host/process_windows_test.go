package host

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
	"testing"
	"time"

	"github.com/DerrickGold/ar-recomp/installer/internal/subprocess"
	"golang.org/x/sys/windows"
)

// Re-exec before testing parses flags: the production startup gate is appended
// to this fake backend exactly as it is to actraiser-builder.
func init() {
	role := os.Getenv("AR_JOB_TEST_ROLE")
	if role == "" {
		return
	}
	time.AfterFunc(30*time.Second, func() { os.Exit(3) })
	if err := runJobTestRole(role); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	os.Exit(0)
}

func jobTestCommand(role string) *exec.Cmd {
	cmd := exec.Command(os.Args[0])
	for _, entry := range os.Environ() {
		if len(entry) >= len("AR_JOB_TEST_ROLE=") && entry[:len("AR_JOB_TEST_ROLE=")] == "AR_JOB_TEST_ROLE=" {
			continue
		}
		cmd.Env = append(cmd.Env, entry)
	}
	cmd.Env = append(cmd.Env, "AR_JOB_TEST_ROLE="+role)
	return cmd
}

func runJobTestRole(role string) error {
	switch role {
	case "host":
		backend := jobTestCommand("backend")
		closeTree, err := startBackendCommand(backend)
		if err != nil {
			return err
		}
		defer closeTree()
		_ = backend.Wait()
		return nil
	case "backend":
		if err := subprocess.WaitForDesktopGate(os.Stdin); err != nil {
			return err
		}
		child := jobTestCommand("compiler")
		if err := child.Start(); err != nil {
			return err
		}
		game := jobTestCommand("game")
		game.SysProcAttr = &syscall.SysProcAttr{CreationFlags: windows.CREATE_BREAKAWAY_FROM_JOB | windows.CREATE_NEW_PROCESS_GROUP}
		if err := game.Start(); err != nil {
			return err
		}
		data, _ := json.Marshal(map[string]int{"backend": os.Getpid(), "compiler": child.Process.Pid, "game": game.Process.Pid})
		path := os.Getenv("AR_JOB_TEST_RECORD")
		if err := os.WriteFile(path+".tmp", data, 0600); err != nil {
			return err
		}
		if err := os.Rename(path+".tmp", path); err != nil {
			return err
		}
	}
	time.Sleep(25 * time.Second)
	return nil
}

func TestWindowsJobReapsCompilersButPreservesDetachedGame(t *testing.T) {
	for _, kill := range []string{"host", "backend"} {
		t.Run(kill, func(t *testing.T) {
			record := filepath.Join(t.TempDir(), "children.json")
			t.Setenv("AR_JOB_TEST_RECORD", record)
			host := jobTestCommand("host")
			if err := host.Start(); err != nil {
				t.Fatal(err)
			}
			defer func() { _ = host.Process.Kill(); _ = host.Wait() }()
			var pids map[string]int
			deadline := time.Now().Add(10 * time.Second)
			for time.Now().Before(deadline) {
				if raw, err := os.ReadFile(record); err == nil && json.Unmarshal(raw, &pids) == nil {
					break
				}
				time.Sleep(20 * time.Millisecond)
			}
			if len(pids) != 3 {
				t.Fatal("process tree did not start")
			}
			handles := map[string]windows.Handle{}
			for role, pid := range pids {
				h, err := windows.OpenProcess(windows.SYNCHRONIZE|windows.PROCESS_TERMINATE, false, uint32(pid))
				if err != nil {
					t.Fatal(err)
				}
				handles[role] = h
				defer func() { _ = windows.TerminateProcess(h, 1); _ = windows.CloseHandle(h) }()
			}
			if kill == "host" {
				if err := host.Process.Kill(); err != nil {
					t.Fatal(err)
				}
			} else if err := windows.TerminateProcess(handles["backend"], 1); err != nil {
				t.Fatal(err)
			}
			for _, role := range []string{"backend", "compiler"} {
				state, err := windows.WaitForSingleObject(handles[role], 5000)
				if err != nil || state != windows.WAIT_OBJECT_0 {
					t.Errorf("%s survived %s termination: %d %v", role, kill, state, err)
				}
			}
			state, err := windows.WaitForSingleObject(handles["game"], 0)
			if err != nil || state != uint32(windows.WAIT_TIMEOUT) {
				t.Fatalf("detached game was killed: %d %v", state, err)
			}
		})
	}
}
