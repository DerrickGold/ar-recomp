package buildgui

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A realistic prefix of a build log, assembled from the exact lines
// project.Regenerate and project.HermeticBuild emit.
const (
	logRegen    = "\n=== Regenerating banks (8 workers) ===\nv2regen: 96 banks, 1 -> 2 variants, 98 files (3 changed), 4.2s\n"
	logFuncs    = "\n=== Syncing funcs.h ===\nsync-funcs: wrote 12043 function declarations to recomp/funcs.h\n"
	logMetadata = "\n=== Refreshing generated-code metadata ===\n"
	logRTS      = "\n=== RTS-web census ===\nno new uncovered continuations since last regen\n"
	logStubs    = "\n=== Hard stub census ===\n"
	logDone     = "\n=== Regeneration complete ===\n"
	logUnits    = "hermetic: 612 translation units (100 cached, 512 to compile, 8 jobs)\n"
	logLinking  = "hermetic: compile done in 91.4s; linking\n"
	logBuilt    = "hermetic: built build/hermetic/ActRaiserRecomp\n"
)

func TestComputeProgressAdvancesThroughPhases(t *testing.T) {
	cases := []struct {
		name       string
		log        string
		state      string
		wantPhase  string
		wantMinPct int
		wantMaxPct int
	}{
		{"empty log is the first phase at zero", "", "idle", "regen", 0, 0},
		{"regen banner selects regen", logRegen, "building", "regen", 0, 0},
		{"funcs banner advances", logRegen + logFuncs, "building", "funcs", 29, 31},
		{"metadata banner advances", logRegen + logFuncs + logMetadata, "building", "metadata", 30, 32},
		{"rts banner advances", logRegen + logFuncs + logMetadata + logRTS, "building", "rts", 32, 34},
		{"stub banner advances", logRegen + logFuncs + logMetadata + logRTS + logStubs, "building", "stubs", 35, 37},
		{
			"regeneration complete moves to compile",
			logRegen + logFuncs + logMetadata + logRTS + logStubs + logDone,
			"building", "compile", 37, 39,
		},
		{
			"linking line moves to link",
			logRegen + logFuncs + logMetadata + logRTS + logStubs + logDone + logUnits + logLinking,
			"building", "link", 87, 89,
		},
		{
			// "hermetic: built ..." is itself the installer's cue, so the
			// final phase reads as complete -- but the overall bar must
			// still stop at 99 until the build actually succeeds.
			"built line moves to install",
			logRegen + logFuncs + logMetadata + logRTS + logStubs + logDone + logUnits + logLinking + logBuilt,
			"building", "install", 99, 99,
		},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			result := computeProgress(testCase.log, testCase.state)
			if result.PhaseID != testCase.wantPhase {
				t.Errorf("phase = %q, want %q", result.PhaseID, testCase.wantPhase)
			}
			if result.Percent < testCase.wantMinPct || result.Percent > testCase.wantMaxPct {
				t.Errorf("percent = %d, want within [%d,%d]",
					result.Percent, testCase.wantMinPct, testCase.wantMaxPct)
			}
			if result.PhaseCount != len(buildPhases) {
				t.Errorf("phase count = %d, want %d", result.PhaseCount, len(buildPhases))
			}
		})
	}
}

// The compile phase is the only one that can measure itself, and it is by far
// the longest. Cached units count as already done.
func TestComputeProgressCountsCompiledUnits(t *testing.T) {
	base := logRegen + logFuncs + logMetadata + logRTS + logStubs + logDone + logUnits
	atStart := computeProgress(base, "building")
	if atStart.PhaseID != "compile" {
		t.Fatalf("phase = %q, want compile", atStart.PhaseID)
	}
	if atStart.Units != 100 || atStart.UnitsTotal != 612 {
		t.Fatalf("units = %d/%d, want 100/612", atStart.Units, atStart.UnitsTotal)
	}
	if !strings.Contains(atStart.Detail, "100 of 612") {
		t.Errorf("detail = %q, want it to mention 100 of 612", atStart.Detail)
	}

	// 200 verbose "cc" lines => 300 of 612 done, so the bar must be strictly
	// further along than it was with only the cached units counted.
	withCompiles := base + strings.Repeat("  cc src/present.c\n", 200)
	later := computeProgress(withCompiles, "building")
	if later.Units != 300 {
		t.Fatalf("units = %d, want 300", later.Units)
	}
	if later.Percent <= atStart.Percent {
		t.Errorf("percent did not advance with compiled units: %d then %d",
			atStart.Percent, later.Percent)
	}
}

// A fully cached compile prints "0 to compile" and no cc lines; the phase is
// finished the moment it is announced, so it must not read as 0% of itself.
func TestComputeProgressTreatsFullyCachedCompileAsComplete(t *testing.T) {
	log := logRegen + logFuncs + logMetadata + logRTS + logStubs + logDone +
		"hermetic: 612 translation units (612 cached, 0 to compile, 8 jobs)\n"
	result := computeProgress(log, "building")
	if result.PhaseID != "compile" {
		t.Fatalf("phase = %q, want compile", result.PhaseID)
	}
	if result.Units != 612 || result.UnitsTotal != 612 {
		t.Errorf("units = %d/%d, want 612/612", result.Units, result.UnitsTotal)
	}
	// compile carries weight 50 of 100 and everything before it 38, so a
	// complete compile phase must be near 88%.
	if result.Percent < 87 || result.Percent > 89 {
		t.Errorf("percent = %d, want ~88", result.Percent)
	}
}

// The bar must never claim to be finished while work continues, and must read
// exactly 100 once the build has actually succeeded.
func TestComputeProgressReservesHundredForSuccess(t *testing.T) {
	everything := logRegen + logFuncs + logMetadata + logRTS + logStubs + logDone +
		"hermetic: 612 translation units (612 cached, 0 to compile, 8 jobs)\n" +
		logLinking + logBuilt + "Playable game installed at ./ActRaiserRecomp\n"

	building := computeProgress(everything, "building")
	if building.Percent >= 100 {
		t.Errorf("percent = %d while still building, want < 100", building.Percent)
	}

	succeeded := computeProgress(everything, "succeeded")
	if succeeded.Percent != 100 {
		t.Errorf("percent = %d on success, want 100", succeeded.Percent)
	}
	if len(succeeded.Completed) != len(buildPhases) {
		t.Errorf("completed = %d phases, want all %d",
			len(succeeded.Completed), len(buildPhases))
	}

	// A build that fails mid-compile must keep its partial reading, not jump.
	failed := computeProgress(logRegen+logFuncs, "failed")
	if failed.Percent >= 100 {
		t.Errorf("failed percent = %d, want < 100", failed.Percent)
	}
	if failed.PhaseID != "funcs" {
		t.Errorf("failed phase = %q, want funcs (where it stopped)", failed.PhaseID)
	}
}

// Progress must never go backwards as the log grows, whatever it contains.
// This is the invariant that matters most to a watching user.
func TestComputeProgressIsMonotonic(t *testing.T) {
	full := logRegen + logFuncs + logMetadata + logRTS + logStubs + logDone +
		logUnits + strings.Repeat("  cc src/present.c\n", 512) + logLinking + logBuilt
	previousPercent, previousIndex := -1, -1
	for length := 0; length <= len(full); length += 37 {
		result := computeProgress(full[:length], "building")
		if result.Percent < previousPercent {
			t.Fatalf("percent went backwards at %d bytes: %d then %d",
				length, previousPercent, result.Percent)
		}
		if result.PhaseIndex < previousIndex {
			t.Fatalf("phase index went backwards at %d bytes: %d then %d",
				length, previousIndex, result.PhaseIndex)
		}
		previousPercent, previousIndex = result.Percent, result.PhaseIndex
	}
}

// The phase only ever ratchets forward. Growing the log linearly never tests
// this, because a real build emits its banners in order -- but the log the GUI
// parses is a bounded TAIL (lockedLogWriter drops the head past maxLogBytes),
// and a build's own sub-tools can print a banner-shaped line at any time. So an
// earlier phase's marker appearing after a later one must not rewind the bar.
func TestComputeProgressNeverRewindsOnOutOfOrderMarkers(t *testing.T) {
	inOrder := logRegen + logFuncs + logMetadata + logRTS + logStubs + logDone + logUnits
	reference := computeProgress(inOrder, "building")
	if reference.PhaseID != "compile" {
		t.Fatalf("phase = %q, want compile", reference.PhaseID)
	}
	// The stub-census banner reappears after compilation has already begun.
	withStaleBanner := inOrder + logStubs
	result := computeProgress(withStaleBanner, "building")
	if result.PhaseIndex < reference.PhaseIndex {
		t.Errorf("phase rewound from %d (%s) to %d (%s) on a stale banner",
			reference.PhaseIndex, reference.PhaseID, result.PhaseIndex, result.PhaseID)
	}
	if result.Percent < reference.Percent {
		t.Errorf("percent rewound from %d to %d on a stale banner",
			reference.Percent, result.Percent)
	}
}

// Unrecognised output must leave the bar alone rather than confuse it.
func TestComputeProgressIgnoresUnrelatedOutput(t *testing.T) {
	noise := "warning: something happened\n+ zig cc -O2\nrandom text\n=== not a phase we know ===\n"
	result := computeProgress(logRegen+noise, "building")
	if result.PhaseID != "regen" {
		t.Errorf("phase = %q, want regen (noise must not advance it)", result.PhaseID)
	}
}

func TestBannerTitle(t *testing.T) {
	cases := map[string]string{
		"\n=== Syncing funcs.h ===":  "Syncing funcs.h",
		"=== Regenerating banks ===": "Regenerating banks",
		"  === Padded ===  ":         "Padded",
		"not a banner":               "",
		"=== unterminated":           "",
		"unopened ===":               "",
		"":                           "",
	}
	for line, want := range cases {
		if got := bannerTitle(line); got != want {
			t.Errorf("bannerTitle(%q) = %q, want %q", line, got, want)
		}
	}
}

func TestCompileTotals(t *testing.T) {
	total, cached, toCompile, ok := compileTotals(
		"hermetic: 612 translation units (100 cached, 512 to compile, 8 jobs)")
	if !ok || total != 612 || cached != 100 || toCompile != 512 {
		t.Fatalf("parsed %d/%d/%d ok=%t, want 612/100/512 ok=true",
			total, cached, toCompile, ok)
	}
	for _, line := range []string{
		"hermetic: built build/hermetic/ActRaiserRecomp",
		"hermetic: compile done in 91.4s; linking",
		"hermetic: xyz translation units (1 cached, 2 to compile, 3 jobs)",
		"hermetic: 612 translation units (garbage)",
		"v2regen: 96 banks",
		"",
	} {
		if _, _, _, ok := compileTotals(line); ok {
			t.Errorf("compileTotals(%q) unexpectedly parsed", line)
		}
	}
}

// The phase banners are copied strings; if project.Regenerate renames a step,
// the bar silently stops advancing at that point. Pin them against the real
// source so that rename fails here instead.
func TestPhaseBannersMatchBuildOutput(t *testing.T) {
	source, err := os.ReadFile(filepath.Join("..", "project", "regen.go"))
	if err != nil {
		t.Skipf("cannot read the build source to cross-check banners: %v", err)
	}
	text := string(source)
	for _, item := range buildPhases {
		if item.banner == "" {
			continue
		}
		if !strings.Contains(text, `"`+item.banner) &&
			!strings.Contains(text, `("`+item.banner) {
			t.Errorf("phase %q expects a step banner starting %q, "+
				"but project/regen.go no longer emits it", item.id, item.banner)
		}
	}
	// The two hermetic markers computeProgress keys on likewise live in
	// project/hermetic.go.
	hermetic, err := os.ReadFile(filepath.Join("..", "project", "hermetic.go"))
	if err != nil {
		t.Skipf("cannot read hermetic.go: %v", err)
	}
	for _, marker := range []string{"translation units", "; linking", "hermetic: built "} {
		if !strings.Contains(string(hermetic), marker) {
			t.Errorf("project/hermetic.go no longer emits %q, "+
				"which the compile/link phases key on", marker)
		}
	}
}

// The step list is injected server-side from buildPhases. If the placeholder is
// ever renamed or dropped, the page would ship a literal "{{STEPS}}" and the
// progress list would be empty -- both silent in a browser.
func TestPageEmbedsEveryPhaseAndLeavesNoPlaceholder(t *testing.T) {
	app := newApplication(context.Background(), Options{
		Title: "Builder", ProjectRoot: t.TempDir(),
	}, "secret")
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/secret/", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("page status = %d", response.Code)
	}
	body := response.Body.String()
	for _, placeholder := range []string{"{{STEPS}}", "{{TITLE}}"} {
		if strings.Contains(body, placeholder) {
			t.Errorf("page still contains the unsubstituted placeholder %s", placeholder)
		}
	}
	for _, item := range buildPhases {
		if !strings.Contains(body, `data-step="`+item.id+`"`) {
			t.Errorf("page is missing the step element for phase %q", item.id)
		}
		if !strings.Contains(body, item.label) {
			t.Errorf("page is missing the label for phase %q", item.id)
		}
	}
}

// The themed page draws its art with CSS and inline SVG. It must not need a
// remote origin, and the CSP must keep that true.
func TestPageArtNeedsNoRemoteOrigin(t *testing.T) {
	app := newApplication(context.Background(), Options{
		Title: "Builder", ProjectRoot: t.TempDir(),
	}, "secret")
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/secret/", nil))
	body := response.Body.String()
	for _, remote := range []string{"http://", "https://", "//fonts.", "url(http"} {
		if strings.Contains(body, remote) {
			t.Errorf("page references a remote origin (%q); the builder must work offline", remote)
		}
	}
	policy := response.Header().Get("Content-Security-Policy")
	for _, directive := range []string{"default-src 'self'", "object-src 'none'", "base-uri 'none'"} {
		if !strings.Contains(policy, directive) {
			t.Errorf("CSP lost %q: %s", directive, policy)
		}
	}
	// Inline SVG needs no img-src at all, but data: URLs are permitted for the
	// artwork; a wildcard or a remote host would not be.
	if strings.Contains(policy, "img-src *") || strings.Contains(policy, "img-src http") {
		t.Errorf("CSP permits remote images: %s", policy)
	}
}

// Status must always carry a progress object so the page never has to cope with
// a missing field.
func TestStatusAlwaysIncludesProgress(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "secret")
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/secret/status", nil))
	var payload struct {
		State    string   `json:"state"`
		Progress progress `json:"progress"`
	}
	if err := json.NewDecoder(response.Body).Decode(&payload); err != nil {
		t.Fatal(err)
	}
	if payload.State != "idle" {
		t.Fatalf("state = %q, want idle", payload.State)
	}
	if payload.Progress.PhaseCount != len(buildPhases) {
		t.Errorf("progress.phaseCount = %d, want %d",
			payload.Progress.PhaseCount, len(buildPhases))
	}
	if payload.Progress.Percent != 0 {
		t.Errorf("idle progress.percent = %d, want 0", payload.Progress.Percent)
	}
	if payload.Progress.Completed == nil {
		t.Error("progress.completed is null; the page expects an array")
	}
}
