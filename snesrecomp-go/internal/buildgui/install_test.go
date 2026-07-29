package buildgui

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
)

// The load-bearing property: launch and rebuild are INDEPENDENT capabilities,
// and the mode a page takes follows from their combination. A slimmed install
// (playable, no tools) must report "launcher" so the build affordances are
// hidden rather than merely disabled.
func TestModeFollowsCapabilities(t *testing.T) {
	cases := []struct {
		name            string
		launch, rebuild bool
		want            string
	}{
		{"fresh copy, nothing built", false, true, "buildable"},
		{"built with tools present", true, true, "ready"},
		{"slimmed: built, tools gone", true, false, "launcher"},
		{"neither possible", false, false, "unusable"},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			state := InstallState{CanLaunch: testCase.launch, CanRebuild: testCase.rebuild}
			if got := state.mode(); got != testCase.want {
				t.Fatalf("mode() = %q, want %q", got, testCase.want)
			}
		})
	}
}

// A build must be refused when its inputs are gone. Enforced server-side on
// purpose: hiding the button is presentation, and presentation is not a
// guarantee -- a stale page, or a cleanup performed in another window, would
// otherwise start a build that dies partway with a confusing toolchain error.
func TestBuildRefusedWhenRebuildImpossible(t *testing.T) {
	buildCalls := 0
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		Build: func(context.Context, string, io.Writer) (Result, error) {
			buildCalls++
			return Result{}, nil
		},
		Detect: func() InstallState {
			return InstallState{CanLaunch: true, CanRebuild: false}
		},
	}, "token")

	recorder := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodPost, "/token/build", nil)
	app.ServeHTTP(recorder, request)

	if recorder.Code != http.StatusConflict {
		t.Fatalf("status = %d, want %d", recorder.Code, http.StatusConflict)
	}
	if buildCalls != 0 {
		t.Fatalf("build ran %d times despite missing inputs", buildCalls)
	}
	// The message has to say what to DO about it, not just that it failed.
	if body := recorder.Body.String(); !bytes.Contains([]byte(body), []byte("download the package again")) {
		t.Fatalf("refusal does not tell the user how to recover: %s", body)
	}
}

// Launching an EXISTING build without building first is the whole point of
// detection: reopening the GUI beside a finished game must offer to play it.
func TestLaunchWorksOnDetectedBuildWithoutBuilding(t *testing.T) {
	launched := make(chan string, 1)
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		Build: func(context.Context, string, io.Writer) (Result, error) {
			return Result{}, nil
		},
		Launch: func(result Result) error {
			launched <- result.OutputPath
			return nil
		},
		Detect: func() InstallState {
			return InstallState{
				CanLaunch: true, CanRebuild: true,
				Result: Result{OutputPath: "/games/run-game.sh"},
			}
		},
	}, "token")

	// Note the state is still "idle" -- no build has run in this process.
	if app.state != "idle" {
		t.Fatalf("state = %q, want idle", app.state)
	}
	recorder := httptest.NewRecorder()
	app.ServeHTTP(recorder, httptest.NewRequest(http.MethodPost, "/token/launch", nil))
	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d (%s), want 200", recorder.Code, recorder.Body.String())
	}
	select {
	case path := <-launched:
		if path != "/games/run-game.sh" {
			t.Fatalf("launched %q, want the detected launcher", path)
		}
	default:
		t.Fatal("launch did not reach the host")
	}
}

// Without a detected build there is nothing to launch, and the old refusal must
// still apply -- otherwise the Play path would call the host with an empty path.
func TestLaunchStillRefusedWithNothingBuilt(t *testing.T) {
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		Build:       func(context.Context, string, io.Writer) (Result, error) { return Result{}, nil },
		Launch:      func(Result) error { t.Fatal("launch must not run"); return nil },
		Detect:      func() InstallState { return InstallState{CanRebuild: true} },
	}, "token")
	recorder := httptest.NewRecorder()
	app.ServeHTTP(recorder, httptest.NewRequest(http.MethodPost, "/token/launch", nil))
	if recorder.Code != http.StatusConflict {
		t.Fatalf("status = %d, want %d", recorder.Code, http.StatusConflict)
	}
}

// The cleanup offer must not appear when there is nothing to clean, when the
// host cannot do it, or when the game is not playable yet -- reclaiming space
// before there is a working build trades away the ability to make one.
func TestSlimOfferedOnlyWhenMeaningful(t *testing.T) {
	cases := []struct {
		name      string
		install   InstallState
		hasSlim   bool
		wantOffer bool
	}{
		{"built, tools present", InstallState{CanLaunch: true, CanRebuild: true, CanSlim: true}, true, true},
		{"already lean", InstallState{CanLaunch: true, CanSlim: false}, true, false},
		{"host cannot slim", InstallState{CanLaunch: true, CanSlim: true}, false, false},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			options := Options{
				ProjectRoot: t.TempDir(),
				Build:       func(context.Context, string, io.Writer) (Result, error) { return Result{}, nil },
				Detect:      func() InstallState { return testCase.install },
			}
			if testCase.hasSlim {
				options.Slim = func(io.Writer) error { return nil }
			}
			app := newApplication(context.Background(), options, "token")
			recorder := httptest.NewRecorder()
			app.ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/token/status", nil))
			var got status
			if err := json.Unmarshal(recorder.Body.Bytes(), &got); err != nil {
				t.Fatalf("decode status: %v", err)
			}
			if got.Install.CanSlim != testCase.wantOffer {
				t.Fatalf("canSlim = %v, want %v", got.Install.CanSlim, testCase.wantOffer)
			}
		})
	}
}

// After a cleanup the page must settle into launcher mode from the SERVER's
// answer, so the page never has to infer capability from a transition.
func TestSlimReprobesAndReportsLauncherMode(t *testing.T) {
	slimmed := false
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		Build:       func(context.Context, string, io.Writer) (Result, error) { return Result{}, nil },
		Slim: func(output io.Writer) error {
			slimmed = true
			_, _ = io.WriteString(output, "removed tools\n")
			return nil
		},
		Detect: func() InstallState {
			if slimmed {
				return InstallState{CanLaunch: true, CanRebuild: false, CanSlim: false,
					Result: Result{OutputPath: "run-game.sh"}}
			}
			return InstallState{CanLaunch: true, CanRebuild: true, CanSlim: true,
				SlimBytes: 700 << 20, Result: Result{OutputPath: "run-game.sh"}}
		},
	}, "token")

	// Before: offered, and a rebuild is possible.
	var before status
	recorder := httptest.NewRecorder()
	app.ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/token/status", nil))
	_ = json.Unmarshal(recorder.Body.Bytes(), &before)
	if before.Mode != "ready" || !before.Install.CanSlim {
		t.Fatalf("before: mode=%q canSlim=%v", before.Mode, before.Install.CanSlim)
	}
	if before.SlimSize != "700 MB" {
		t.Fatalf("SlimSize = %q, want 700 MB", before.SlimSize)
	}

	recorder = httptest.NewRecorder()
	app.ServeHTTP(recorder, httptest.NewRequest(http.MethodPost, "/token/slim", nil))
	if recorder.Code != http.StatusOK {
		t.Fatalf("slim status = %d (%s)", recorder.Code, recorder.Body.String())
	}

	// After: launcher mode, no further offer, and a rebuild now refused.
	var after status
	recorder = httptest.NewRecorder()
	app.ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/token/status", nil))
	_ = json.Unmarshal(recorder.Body.Bytes(), &after)
	if after.Mode != "launcher" {
		t.Fatalf("mode = %q, want launcher", after.Mode)
	}
	if after.Install.CanSlim {
		t.Fatal("cleanup still offered after it ran")
	}
	if !after.SlimDone {
		t.Fatal("slimDone not reported, so the page cannot confirm it")
	}
	// And the game is still launchable -- the point of the whole exercise.
	if !after.Install.CanLaunch {
		t.Fatal("cleanup left the install unable to launch")
	}
}

// A second cleanup must be refused rather than running against a changed
// filesystem.
func TestSlimRefusedWhenNothingLeftToRemove(t *testing.T) {
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		Build:       func(context.Context, string, io.Writer) (Result, error) { return Result{}, nil },
		Slim:        func(io.Writer) error { t.Fatal("slim must not run"); return nil },
		Detect:      func() InstallState { return InstallState{CanLaunch: true, CanSlim: false} },
	}, "token")
	recorder := httptest.NewRecorder()
	app.ServeHTTP(recorder, httptest.NewRequest(http.MethodPost, "/token/slim", nil))
	if recorder.Code != http.StatusConflict {
		t.Fatalf("status = %d, want %d", recorder.Code, http.StatusConflict)
	}
}

// Every existing caller passes no Detect hook. Those sessions must behave
// exactly as the builder always did, or this change breaks the CLI.
func TestWithoutDetectBehavesLikeTheOriginalBuilder(t *testing.T) {
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		Build:       func(context.Context, string, io.Writer) (Result, error) { return Result{}, nil },
	}, "token")
	recorder := httptest.NewRecorder()
	app.ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, "/token/status", nil))
	var got status
	if err := json.Unmarshal(recorder.Body.Bytes(), &got); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if got.Mode != "buildable" {
		t.Fatalf("mode = %q, want buildable", got.Mode)
	}
	if got.Install.CanLaunch || got.Install.CanSlim {
		t.Fatal("no-Detect session claimed capabilities it cannot have")
	}
}

func TestSlimSummaryRoundsAndOmitsSmallSizes(t *testing.T) {
	cases := []struct {
		bytes int64
		want  string
	}{
		{0, ""},
		{512 << 10, ""}, // under a MB: not worth a figure
		{700 << 20, "700 MB"},
		{(1 << 30) + (512 << 20), "1.5 GB"},
		{2 << 30, "2 GB"}, // no trailing ".0"
	}
	for _, testCase := range cases {
		if got := slimSummary(testCase.bytes); got != testCase.want {
			t.Fatalf("slimSummary(%d) = %q, want %q", testCase.bytes, got, testCase.want)
		}
	}
}
