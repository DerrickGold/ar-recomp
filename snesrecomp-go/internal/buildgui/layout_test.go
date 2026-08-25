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

/* The shell is a three-tab layout with a sticky progress dock. Each piece is
 * asserted because the page is a single Go string constant: an editing slip
 * cannot be caught by the compiler, only here. */
func TestPageHasTabShellAndDock(t *testing.T) {
	body := renderPage(t)
	for _, want := range []string{
		`role="tablist"`,
		`id="tab-build"`, `id="tab-assets"`, `id="tab-manual"`,
		`id="panel-build"`, `id="panel-assets"`, `id="panel-manual"`,
		`id="dock"`, `id="dock-phase"`, `id="dock-pct"`, `id="dock-launch"`,
	} {
		if !strings.Contains(body, want) {
			t.Errorf("page is missing %s", want)
		}
	}
	// Build is the landing tab; Assets and Manual start hidden.
	if !strings.Contains(body, `id="tab-build" role="tab" aria-selected="true"`) {
		t.Error("the Build tab should be selected on load")
	}
	if !strings.Contains(body, `id="panel-manual" role="tabpanel" aria-labelledby="tab-manual" hidden`) {
		t.Error("the Manual panel should start hidden")
	}
	if !strings.Contains(body, `id="panel-assets" role="tabpanel" aria-labelledby="tab-assets" hidden`) {
		t.Error("the Assets panel should start hidden")
	}
}

func TestAssetsTabHasTitleToggleAndIdentifiedTrackPickers(t *testing.T) {
	body := renderPage(t)
	for _, want := range []string{
		`id="title-toggle"`, `id="save-assets"`, `src="title-logo.png"`,
		`id="generate-previews"`, `class="original-audio"`,
		`class="replacement-audio"`,
		`name="track-title-theme"`, `name="track-song-00"`,
		`name="track-song-03"`, `name="track-song-06"`,
		`name="track-song-12"`, `name="track-song-15"`,
		`name="track-song-08"`, `name="track-song-16"`,
		`Unidentified track 03`, `Manifest [music:song-03]`,
		`accept=".ogg,.oga,audio/ogg"`,
	} {
		if !strings.Contains(body, want) {
			t.Errorf("Assets tab is missing %s", want)
		}
	}
	if !strings.Contains(body, `fetch("assets",{method:"POST",body:new FormData(assetForm)})`) {
		t.Error("Assets form is not wired to the save endpoint")
	}
	if !strings.Contains(body, `fetch("audio-previews",{method:"POST"})`) {
		t.Error("original-audio extraction is not wired to the preview endpoint")
	}
	if !strings.Contains(body, `URL.createObjectURL(input.files[0])`) {
		t.Error("selected replacement files do not get local browser playback")
	}
}

func TestAssetsTabCoversEverySongTableImage(t *testing.T) {
	want := []string{
		"title-theme",
		"song-00", "song-01", "song-02", "song-03", "song-04", "song-05", "song-06",
		"song-08", "song-09", "song-10", "song-11", "song-12", "song-13", "song-14",
		"song-15", "song-16",
	}
	if len(assetTracks) != len(want) {
		t.Fatalf("Assets tab has %d song images, want all %d", len(assetTracks), len(want))
	}
	for index, id := range want {
		if assetTracks[index].ID != id {
			t.Errorf("song image %d = %q, want %q", index, assetTracks[index].ID, id)
		}
		if assetTracks[index].PreviewSource == 0 {
			t.Errorf("song image %q has no preview source", id)
		}
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
