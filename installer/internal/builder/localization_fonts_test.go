package builder

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

func fontSave(t *testing.T, app *application, q localizationRequest, data []byte, code int) {
	t.Helper()
	var body bytes.Buffer
	w := multipart.NewWriter(&body)
	encoded, _ := json.Marshal(q)
	if err := w.WriteField("request", string(encoded)); err != nil {
		t.Fatal(err)
	}
	part, err := w.CreateFormFile("font0", "file.otf")
	if err != nil {
		t.Fatal(err)
	}
	part.Write(data)
	w.Close()
	r := httptest.NewRequest("POST", "/secret/localization/save", &body)
	r.Header.Set("Content-Type", w.FormDataContentType())
	response := httptest.NewRecorder()
	app.ServeHTTP(response, r)
	if response.Code != code {
		t.Fatalf("font save %d want %d: %s", response.Code, code, response.Body.String())
	}
}

func TestLocalizationFontSaveIsAtomicAndReopenable(t *testing.T) {
	app := editableWorkflowFixture(t)
	q := locIdentity(app)
	q.SaveFonts, q.Fonts, q.FontPaths = true, lk.PackFonts{Primary: "fonts/Fixture.otf", Fallback: []string{"builtin:actraiser-sans"}}, []string{"fonts/Fixture.otf"}
	q.SaveMessage, q.ID, q.Body, q.Status = true, "action.hud.act_1", "Nouvelle aventure\n@end\n", lk.TranslationDone
	q.SaveNotice, q.NoticeName, q.NoticeText = true, "FONT.txt", "Fixture license"
	fontSave(t, app, q, []byte("OTTOfixture"), 200)
	p := app.localization.current
	if p.Pack().Manifest().Fonts().Primary != q.Fonts.Primary || len(p.Notices()) != 1 {
		t.Fatal("lost font or license")
	}
	onDisk, err := app.localization.store.Open(q.ProjectID)
	if err != nil || onDisk.ProjectRevision() != p.ProjectRevision() {
		t.Fatal("cannot reopen saved font project", err)
	}
	fontSave(t, app, q, []byte("OTTOfixture"), 409)
	q.Revision = p.ProjectRevision()
	q.Body = "First\n@page\nUnreachable\n@end\n"
	fontSave(t, app, q, []byte("OTTOchanged"), 400)
	if app.localization.current != p {
		t.Fatal("bad text partially committed font or notice")
	}
	q.Body = "Another adventure\n@end\n"
	fontSave(t, app, q, []byte("not a font"), 400)
	if app.localization.current != p {
		t.Fatal("bad envelope committed")
	}
	q.FontPaths = []string{"../escape.otf"}
	fontSave(t, app, q, []byte("OTTOfixture"), 400)
	q = locIdentity(app)
	q.SaveFonts, q.Fonts = true, lk.PackFonts{Primary: "builtin:actraiser-sans"}
	locJSON(t, app, "save", q, 200)
	if _, exists := app.localization.current.Pack().Files()["fonts/Fixture.otf"]; exists || len(app.localization.current.Notices()) != 1 {
		t.Fatal("removed file retained or credits removed")
	}
}

func TestLocalizationCoverageGatesPublicationAndInstallation(t *testing.T) {
	app := editableWorkflowFixture(t)
	q := locIdentity(app)
	q.ID, q.Body, q.Status = "action.hud.act_1", "日\n@end\n", lk.TranslationDone
	locJSON(t, app, "edit", q, 200)
	p := app.localization.current
	app.options.fontCoverageProbe = func(ctx context.Context, fonts []lk.FontCoverageSource, scalars []rune) (lk.FontCoverageProbeResult, error) {
		r, _ := unitFontCoverageProbe(ctx, fonts, scalars)
		for i, scalar := range scalars {
			if scalar == '日' {
				r.Provided[i] = false
			}
		}
		return r, nil
	}
	q = locIdentity(app)
	for _, endpoint := range []string{"install", "installation-check", "publish", "publication-check"} {
		q.ConfirmRights = endpoint == "publish" || endpoint == "publication-check"
		if body := locJSON(t, app, endpoint, q, 400).Body.String(); !strings.Contains(body, "U+65E5") {
			t.Fatal(endpoint, body)
		}
	}
	if app.localization.current != p {
		t.Fatal("failed coverage changed project")
	}
	if _, err := os.Stat(filepath.Join(app.localizationRoot(), "packs", q.ProjectID)); !os.IsNotExist(err) {
		t.Fatal("failed gate wrote installed files", err)
	}
	q = locIdentity(app)
	var report lk.FontCoverageReport
	if err := json.Unmarshal(locJSON(t, app, "font-coverage", q, 200).Body.Bytes(), &report); err != nil {
		t.Fatal(err)
	}
	if report.Complete || report.MissingCount != 1 || report.FallbackRevision == "" {
		t.Fatal(report)
	}
	locJSON(t, app, "backup", q, 200) // Private progress can always be kept.
	// Publication checks only the selected output, not WIP text excluded from
	// it. Installation still includes that WIP and must continue to reject it.
	q.ID, q.Body, q.Status = "action.hud.act_1", "日\n@end\n", lk.TranslationWIP
	locJSON(t, app, "edit", q, 200)
	q = locIdentity(app)
	q.ID, q.Body, q.Status = "sky.action_mode.confirm", "@anchor reset_text_cursor.00\nNew English translation.\n@anchor yield.01\n@end\n", lk.TranslationDone
	locJSON(t, app, "edit", q, 200)
	q = locIdentity(app)
	q.ConfirmRights = true
	locJSON(t, app, "publication-check", q, 200)
	locJSON(t, app, "installation-check", locIdentity(app), 400)
	q.IncludeWIP = true
	locJSON(t, app, "publication-check", q, 400)
	app.options.fontCoverageProbe = nil
	if body := locJSON(t, app, "font-coverage", locIdentity(app), 400).Body.String(); !strings.Contains(body, "finish building") {
		t.Fatal(body)
	}
}

func TestLocalizationCoverageIsReadOnlyAndDoesNotBlockEdits(t *testing.T) {
	app := editableWorkflowFixture(t)
	started, release, finished := make(chan struct{}), make(chan struct{}), make(chan struct{})
	app.options.fontCoverageProbe = func(ctx context.Context, fonts []lk.FontCoverageSource, scalars []rune) (lk.FontCoverageProbeResult, error) {
		close(started)
		<-release
		return unitFontCoverageProbe(ctx, fonts, scalars)
	}
	q := locIdentity(app)
	response := httptest.NewRecorder()
	go func() { defer close(finished); app.ServeHTTP(response, localizationRequestFor("font-coverage", q)) }()
	defer func() {
		current := app.localization.current
		close(release)
		awaitLocalization(t, finished, "coverage response")
		if response.Code != 200 || app.localization.current != current {
			t.Fatal("coverage published stale author state", response.Body.String())
		}
	}()
	awaitLocalization(t, started, "font probe")
	edited := make(chan struct{})
	go func() {
		defer close(edited)
		locGET(t, app, "state", nil)
		q.ID, q.Body, q.Status = "action.hud.act_1", "Edited during coverage\n@end\n", lk.TranslationDone
		locJSON(t, app, "edit", q, 200)
	}()
	awaitLocalization(t, edited, "edit during font check")
	if app.localization.current.ProjectRevision() == q.Revision {
		t.Fatal("edit lost")
	}
}

func TestLocalizationActualGameFontBackend(t *testing.T) {
	binary := os.Getenv("AR_AUTHOR_FONT_PROBE")
	if binary == "" {
		t.Skip("set AR_AUTHOR_FONT_PROBE to the built game executable")
	}
	root, err := filepath.Abs("../../..")
	if err != nil {
		t.Fatal(err)
	}
	builtin := filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf")
	jp := filepath.Join(root, "tests/fixtures/unicode-fonts/fonts/NotoSansJP-Bold.otf")
	data, err := os.ReadFile(jp)
	if err != nil {
		t.Fatal(err)
	}
	base := []lk.FontCoverageSource{{Reference: "builtin:actraiser-sans"}}
	scalars := []rune{'A', 'é', '日', 0x200d, 0xfffc, 0x10ffff}
	r, err := runLocalizationFontProbe(context.Background(), binary, builtin, base, scalars)
	if err != nil {
		t.Fatal(err)
	}
	if fmt.Sprint(r.Provided) != "[true true false true true false]" {
		t.Fatal(r)
	}
	base = append(base, lk.FontCoverageSource{Reference: "fonts/Japanese.otf", Size: int64(len(data)), Open: func() io.ReadCloser { return io.NopCloser(bytes.NewReader(data)) }})
	r, err = runLocalizationFontProbe(context.Background(), binary, builtin, base, scalars)
	if err != nil || fmt.Sprint(r.Provided) != "[true true true true true false]" || len(r.Fonts) != 2 {
		t.Fatal(r, err)
	}
	if _, err := runLocalizationFontProbe(context.Background(), binary, builtin, base, nil); err != nil {
		t.Fatal("empty input did not preflight", err)
	}
	base[1].Size = 4
	base[1].Open = func() io.ReadCloser { return io.NopCloser(strings.NewReader("OTTO")) }
	if _, err := runLocalizationFontProbe(context.Background(), binary, builtin, base, scalars); err == nil {
		t.Fatal("corrupt font accepted")
	}
	// Headless command must not start a game or create settings/runs/save files.
	dir := t.TempDir()
	command := exec.Command(binary, "--font-coverage-v1", builtin)
	command.Dir, command.Stdin = dir, strings.NewReader("0041\n")
	if output, err := command.CombinedOutput(); err != nil || string(output) != "0041\t1\n" {
		t.Fatal(string(output), err)
	}
	if entries, err := os.ReadDir(dir); err != nil || len(entries) != 0 {
		t.Fatal("headless command changed working directory", entries, err)
	}
}
