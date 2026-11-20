package builder

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

func renderFrontendSource(t *testing.T) string {
	t.Helper()
	body := renderPage(t)
	for _, name := range []string{"web/app.js", "web/theme.css", "web/scene.js"} {
		data, err := frontendFiles.ReadFile(name)
		if err != nil {
			t.Fatal(err)
		}
		body += "\n" + string(data)
	}
	return body
}

// Assert the embedded markup and behavior together without depending on inline
// script/style tags. The browser independently validates the served resources.
func TestPageHasTabShellAndHeaderBuildStatus(t *testing.T) {
	body := renderFrontendSource(t)
	for _, want := range []string{
		`role="tablist"`,
		`id="tab-build"`, `id="tab-assets"`, `id="tab-settings"`,
		`id="tab-localization"`, `id="panel-localization"`,
		`id="panel-build"`, `id="panel-assets"`, `id="panel-settings"`,
		`id="dock"`, `id="workspace-status"`, `id="dock-pct"`, `id="dock-launch"`,
	} {
		if !strings.Contains(body, want) {
			t.Errorf("page is missing %s", want)
		}
	}
	// Home is the landing tab; work panels are mounted but initially hidden.
	if !strings.Contains(body, `id="tab-home" role="tab" aria-selected="true"`) {
		t.Error("the Home tab should be selected on load")
	}
	if !strings.Contains(body, `id="panel-settings" role="tabpanel" aria-labelledby="tab-settings" hidden`) {
		t.Error("the Builder Settings panel should start hidden")
	}
	if !strings.Contains(body, `id="panel-assets" role="tabpanel" aria-labelledby="tab-assets" hidden`) {
		t.Error("the Assets panel should start hidden")
	}
}

func TestAssetsTabHasIdentifiedTrackPickers(t *testing.T) {
	body := renderFrontendSource(t)
	for _, want := range []string{
		`id="save-assets-top"`,
		`id="generate-previews"`, `class="original-audio"`,
		`class="replacement-audio"`,
		`name="track-title-theme"`, `name="track-song-00"`,
		`name="track-song-03"`, `name="track-song-06"`,
		`name="track-song-12"`, `name="track-song-15"`,
		`name="track-song-08"`, `name="track-song-16"`,
		`Track 03`, `Slot song-03`,
		`accept=".ogg,.oga,audio/ogg"`,
	} {
		if !strings.Contains(body, want) {
			t.Errorf("Assets tab is missing %s", want)
		}
	}
	if !strings.Contains(body, `fetchResponse("music-draft",{method:"POST",body})`) {
		t.Error("Assets form is not wired to the save endpoint")
	}
	if !strings.Contains(body, `fetchResponse(
      force?"audio-previews?force=1":"audio-previews",{method:"POST"})`) {
		t.Error("original-audio extraction is not wired to the preview endpoint")
	}
	if !strings.Contains(body, `URL.createObjectURL(input.files[0])`) {
		t.Error("selected replacement files do not get local browser playback")
	}
}

// Saving must be reachable from the top of a seventeen-row list, and it must be
// possible to take a replacement back off a slot. A file input can be filled but
// never emptied by a page, so the revert needs a control and a companion field
// of its own -- without the hidden field the button press is lost on submit.
func TestAssetsTabCanSaveFromTheTopAndRevertASlot(t *testing.T) {
	body := renderFrontendSource(t)
	for _, want := range []string{
		`id="asset-bar"`, `id="save-assets-top"`, `id="discard-assets"`,
		`id="asset-bar-note"`, `class="editor-bar"`,
		`class="asset-clear"`, `name="track-remove-song-00"`,
		`name="track-remove-title-theme"`, `id="regenerate-previews"`,
	} {
		if !strings.Contains(body, want) {
			t.Errorf("Assets tab is missing %s", want)
		}
	}
	// The toolbar is only useful if it follows the reader down the list.
	if !strings.Contains(body, ".editor-bar {\n  position:sticky;\n  top:var(--workspace-header-h);") {
		t.Error("the asset toolbar is not sticky")
	}
	for _, id := range []string{"song-00", "song-16", "title-theme"} {
		if !strings.Contains(body, `name="track-remove-`+id+`" type="hidden" value="0"`) {
			t.Errorf("%s has no revert field", id)
		}
	}
}

// The Play scene pauses when the app or its panel is hidden.
func TestSceneIsOptionalAndVisibilityAware(t *testing.T) {
	body := renderFrontendSource(t)
	for _, want := range []string{`id="scene-motion"`, `id="palace-scene"`, `id="scene-setting"`, `"visibilitychange",sync`, `"workshop:navigate",sync`, `prefers-reduced-motion: reduce`, `cancelAnimationFrame(raf)`, `"auto", "animated", "still", "off"`} {
		if !strings.Contains(body, want) {
			t.Errorf("scene is missing %s", want)
		}
	}
}

// Build controls share the header; sticky editor/asset bars account for its
// actual height, including wrapping. No footer may cover the work surface.
func TestHeaderClearanceIsMeasuredNotGuessed(t *testing.T) {
	body := renderFrontendSource(t)
	header := strings.Index(body, `<header class="workspace-bar">`)
	if header < 0 {
		t.Fatal("workspace header is missing")
	}
	end := strings.Index(body[header:], "</header>") + header
	for _, id := range []string{`id="dock"`, `id="dock-pct"`, `id="dock-launch"`, `id="interface-language"`, `id="interface-language-status"`} {
		if at := strings.Index(body, id); at < header || at > end {
			t.Errorf("%s is outside the header", id)
		}
	}
	if strings.Contains(body, "--dock-h") || strings.Contains(body, `id="bar"`) {
		t.Error("obsolete footer clearance/progress bar remains")
	}
	if !strings.Contains(body, `setProperty("--workspace-header-h",workspaceHeader.offsetHeight+"px")`) || !strings.Contains(body, "new ResizeObserver(syncHeaderHeight).observe(workspaceHeader)") {
		t.Error("sticky controls do not track header reflow")
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
	body := renderFrontendSource(t)
	if strings.Contains(body, `id="phase"`) || strings.Contains(body, `id="pct"`) {
		t.Error("the duplicate in-panel phase/percent elements are back")
	}
	if !strings.Contains(body, `id="steps-box" hidden`) {
		t.Error("the step checklist must be collapsed until a build starts")
	}
	if count := strings.Count(body, `id="state"`); count != 1 {
		t.Errorf("found %d status lines, want exactly 1", count)
	}
	// Percentage stays hidden until a build starts; Run uses detected readiness.
	if !strings.Contains(body, `id="dock" data-open="false"`) {
		t.Error("the dock should start closed")
	}
}

func TestMusicUsesSearchableDropdown(t *testing.T) {
	body := renderFrontendSource(t)
	for _, want := range []string{`id="asset-track-toggle"`, `aria-controls="asset-track-options"`, `id="asset-track-options" hidden`, `trackSearch.addEventListener("input",filterTracks)`, `event.key==="Escape"`, `event.key==="Enter"`, `id="generate-previews" type="button"`} {
		if !strings.Contains(body, want) {
			t.Error("missing music affordance", want)
		}
	}
	if strings.Contains(body, "asset-list-panel") {
		t.Error("old permanent track sidebar remains")
	}
}

// Play links to the original box art below its primary actions.
func TestCoverArtIsBelowPlayActions(t *testing.T) {
	body := renderFrontendSource(t)
	primary := strings.Index(body, `id="home-primary"`)
	build := strings.Index(body, `id="panel-build"`)
	art := strings.Index(body, `src="boxart.webp"`)
	if primary < 0 || art < primary || art >= build {
		t.Fatal("cover art should follow the Play actions")
	}
}

func TestWorkbenchNavigationPreservesMountedDrafts(t *testing.T) {
	body := renderFrontendSource(t)
	for _, want := range []string{`class="skip-link"`, `"popstate",route`, `"hashchange",route`, `"ArrowDown","ArrowUp","Home","End"`, `id="asset-track-list"`, `id="asset-search"`, `function syncAssetSelection()`, `id="home-projects"`, `id="loc-library"`, `window.localizationOpenProject?.(row.id)`, `new FormData(assetForm)`, `window.localizationHasEdits?.()`} {
		if !strings.Contains(body, want) {
			t.Errorf("workbench missing %s", want)
		}
	}
}

func TestExistingBuildResumesProgressOnPageLoad(t *testing.T) {
	body := renderFrontendSource(t)
	if !strings.Contains(body, "polling=true; stepsBox.hidden=false;") {
		t.Fatal("an already-running build must resume polling and show its steps")
	}
	if !strings.Contains(body, "if(closed||launching) return;") {
		t.Fatal("the Play affordances must share an in-flight launch guard")
	}
}

func TestBuilderSettingsContainsHelpAndUpdates(t *testing.T) {
	body := renderFrontendSource(t)
	start := strings.Index(body, `id="panel-settings"`)
	end := strings.Index(body[start:], "</main>")
	for _, want := range []string{`id="settings-help-title"`, `data-project-link="releases"`, `data-project-link="repository"`, `class="workshop-help"`} {
		if !strings.Contains(body[start:start+end], want) {
			t.Errorf("Builder Settings is missing %s", want)
		}
	}
	for _, removed := range []string{`id="tab-manual"`, `id="manual-frame"`, `id="manual-form"`} {
		if strings.Contains(body, removed) {
			t.Errorf("obsolete manual page control: %s", removed)
		}
	}
}
