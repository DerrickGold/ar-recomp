package builder

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http/httptest"
	"os"
	"runtime"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestBuildErrorGuidance(t *testing.T) {
	blocked := fmt.Errorf("start snesbuild regen: %w", &os.PathError{
		Op: "fork/exec", Path: `C:\Builder\snesbuild.exe`, Err: syscall.Errno(4551),
	})
	for _, test := range []struct {
		name, goos string
		err        error
		blocked    bool
	}{
		{"wrapped Windows policy error", "windows", blocked, true},
		{"joined Windows policy error", "windows", errors.Join(errors.New("cleanup failed"), blocked), true},
		{"ordinary access denied", "windows", syscall.Errno(5), false},
		{"compiler output mentioning policy", "windows", errors.New("An Application Control policy has blocked this file."), false},
		{"same number on macOS", "darwin", blocked, false},
		{"same number on Linux", "linux", blocked, false},
		{"no failure", "windows", nil, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			code, recovery := buildErrorGuidance(test.err, test.goos)
			if test.blocked {
				if code != "builder.errors.windows_app_control" || recovery != "builder.recovery.windows_app_control" {
					t.Fatalf("missing policy guidance: %q, %q", code, recovery)
				}
			} else if code != "" || recovery != "" {
				t.Fatalf("unrelated error received policy guidance: %q, %q", code, recovery)
			}
		})
	}
}

func TestBuildFailureReportsAndClearsGuidance(t *testing.T) {
	failure := fmt.Errorf("start snesbuild regen: %w", &os.PathError{
		Op: "fork/exec", Path: `C:\Builder\snesbuild.exe`, Err: syscall.Errno(4551),
	})
	builds := 0
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		Build: func(context.Context, string, io.Writer) (Result, error) {
			builds++
			if builds == 1 {
				return Result{}, failure
			}
			return Result{Message: "done"}, nil
		},
	}, "tok")
	for _, wantState := range []string{"failed", "succeeded"} {
		response := httptest.NewRecorder()
		app.ServeHTTP(response, rebuildRequest(t, false, "test.sfc", "test-rom"))
		if response.Code != 202 {
			t.Fatal(response.Code, response.Body.String())
		}
		deadline := time.Now().Add(2 * time.Second)
		for {
			response = httptest.NewRecorder()
			app.ServeHTTP(response, httptest.NewRequest("GET", "/tok/status", nil))
			var current status
			if err := json.Unmarshal(response.Body.Bytes(), &current); err != nil {
				t.Fatal(err)
			}
			if current.State == wantState {
				if wantState == "failed" {
					if current.Error != failure.Error() || !strings.Contains(current.Log, failure.Error()) {
						t.Fatalf("lost original diagnostic: %+v", current)
					}
					if runtime.GOOS == "windows" && (current.ErrorCode != "builder.errors.windows_app_control" || current.RecoveryKey != "builder.recovery.windows_app_control") {
						t.Fatalf("status omitted policy guidance: %+v", current)
					}
				} else if current.Error != "" || current.ErrorCode != "" || current.RecoveryKey != "" || strings.Contains(current.Log, failure.Error()) {
					t.Fatalf("retry retained old failure: %+v", current)
				}
				break
			}
			if time.Now().After(deadline) {
				t.Fatalf("build did not reach %s: %+v", wantState, current)
			}
			time.Sleep(10 * time.Millisecond)
		}
	}
}
