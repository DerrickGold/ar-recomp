package buildgui

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func renderPage(t *testing.T) string {
	t.Helper()
	app := newApplication(context.Background(), Options{
		Title: "Builder", ProjectRoot: t.TempDir(),
	}, "tok")
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/tok/", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("page status = %d", response.Code)
	}
	return response.Body.String()
}

/* The shell is a two-tab layout with a sticky progress dock. Each piece is
 * asserted because the page is a single Go string constant: an editing slip
 * cannot be caught by the compiler, only here. */
func TestPageHasTabShellAndDock(t *testing.T) {
	body := renderPage(t)
	for _, want := range []string{
		`role="tablist"`,
		`id="tab-build"`, `id="tab-manual"`,
		`id="panel-build"`, `id="panel-manual"`,
		`id="dock"`, `id="dock-phase"`, `id="dock-pct"`, `id="dock-launch"`,
	} {
		if !strings.Contains(body, want) {
			t.Errorf("page is missing %s", want)
		}
	}
	// Build is the landing tab; Manual starts hidden.
	if !strings.Contains(body, `id="tab-build" role="tab" aria-selected="true"`) {
		t.Error("the Build tab should be selected on load")
	}
	if !strings.Contains(body, `id="panel-manual" role="tabpanel" aria-labelledby="tab-manual" hidden`) {
		t.Error("the Manual panel should start hidden")
	}
}

/* The old layout reported the same state three times over -- a phase label, a
 * separate state line, and eight empty step circles -- before a ROM was even
 * chosen. There must now be exactly one idle status element, and the checklist
 * must start collapsed. */
func TestIdlePageHasOneStatusAndNoVisibleChecklist(t *testing.T) {
	body := renderPage(t)
	if strings.Contains(body, `id="phase"`) || strings.Contains(body, `id="pct"`) {
		t.Error("the duplicate in-panel phase/percent elements are back")
	}
	if !strings.Contains(body, `id="steps-box" hidden`) {
		t.Error("the step checklist must be collapsed until a build starts")
	}
	if count := strings.Count(body, `id="state"`); count != 1 {
		t.Errorf("found %d status lines, want exactly 1", count)
	}
	// The dock is furniture while idle, so it must start closed.
	if !strings.Contains(body, `id="dock" data-open="false"`) {
		t.Error("the dock should start closed")
	}
}

/* The cover art belongs to the header now, so it is visible on BOTH tabs
 * without needing a section of its own. */
func TestCoverArtIsInTheHeader(t *testing.T) {
	body := renderPage(t)
	masthead := strings.Index(body, `class="masthead"`)
	tablist := strings.Index(body, `role="tablist"`)
	art := strings.Index(body, `src="boxart.webp"`)
	if masthead < 0 || tablist < 0 || art < 0 {
		t.Fatal("masthead, tablist or cover art missing")
	}
	if !(masthead < art && art < tablist) {
		t.Errorf("cover art at %d should sit inside the masthead (%d) above the tabs (%d)",
			art, masthead, tablist)
	}
}

/* The manual iframe must be MOUNTED but srcless: mounted so switching tabs
 * cannot refetch 8 MB or lose the reader's page, srcless so a user who never
 * opens the Manual tab never pays for it. */
func TestManualFrameIsMountedButNotPreloaded(t *testing.T) {
	body := renderPage(t)
	frame := strings.Index(body, `id="manual-frame"`)
	if frame < 0 {
		t.Fatal("manual iframe missing")
	}
	tagEnd := strings.Index(body[frame:], ">")
	tag := body[frame : frame+tagEnd]
	if strings.Contains(tag, "src=") {
		t.Errorf("manual iframe is preloaded; it should get its src on first open: %s", tag)
	}
	if strings.Contains(tag, "hidden") {
		t.Error("the iframe itself must not be hidden — its PANEL is, so the " +
			"frame stays mounted and keeps the reader's page across tab switches")
	}
	if !strings.Contains(body, `frame.setAttribute("src","manual.pdf")`) {
		t.Error("nothing assigns the manual src on first open")
	}
}
