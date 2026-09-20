package builder

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"mime/multipart"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"testing/fstest"
	"time"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"github.com/DerrickGold/ar-recomp/installer/internal/textpreview"
)

func playbackTestProjects(t *testing.T) (*lk.AuthorProject, *lk.AuthorProject) {
	t.Helper()
	metadata := localizationTestSource(t).Pack().Manifest().Metadata()
	manifest, err := lk.NewPackManifestVersion(metadata, lk.PackFonts{Primary: "builtin:actraiser-sans"}, []string{"text/preview.artext"}, 2)
	if err != nil {
		t.Fatal(err)
	}
	pack, err := lk.LoadAuthorPack(fstest.MapFS{"pack.ini": {Data: []byte(manifest.Text())}, "text/preview.artext": {Data: []byte(":: dialogue.event.relay.aitos\nSource words.\n@end\n")}})
	if err != nil {
		t.Fatal(err)
	}
	source, err := lk.NewSourceProject(pack)
	if err != nil {
		t.Fatal(err)
	}
	metadata.ID = "playback.translation"
	project, err := lk.NewTranslationProject(pack, metadata)
	if err != nil {
		t.Fatal(err)
	}
	return source, project
}

func TestLocalizationPlaybackKeepsUnsavedTextAndFontsPrivate(t *testing.T) {
	source, project := playbackTestProjects(t)
	before := project.ProjectRevision()
	work := &localizationWork{localizationStateData: localizationStateData{current: project, source: &localizationSourceCache{native: source, checked: true}}}
	var mu sync.Mutex
	observed := map[string]string{}
	work.preview = func(_ context.Context, id string, p, fallback *lk.AuthorPack, scenario textpreview.Scenario) (textpreview.Movie, error) {
		ops, err := p.MessageOperations(id)
		if err != nil {
			return textpreview.Movie{}, err
		}
		var text strings.Builder
		for _, op := range ops {
			if op.Op == "text" {
				text.WriteString(op.Value)
			}
		}
		mu.Lock()
		observed[p.Manifest().Metadata().ID] = text.String()
		mu.Unlock()
		if p.Manifest().Metadata().ID == "playback.translation" && len(p.Manifest().Fonts().Roles) != 1 {
			return textpreview.Movie{}, fmt.Errorf("draft role absent")
		}
		return textpreview.Movie{MessageID: id}, nil
	}
	q := localizationDraftRequest{ProjectID: "playback.translation", Revision: before, ID: "dialogue.event.relay.aitos", Body: "Unsaved <span font=\"hud\">draft</span>.\n@end\n", Status: lk.TranslationWIP, SaveFonts: true, Fonts: lk.PackFonts{Primary: "builtin:actraiser-sans", Roles: []lk.PackFontRole{{Name: "hud", Primary: "builtin:actraiser-sans"}}}}
	if _, err := work.playback(context.Background(), q); err != nil {
		t.Fatal(err)
	}
	if observed["native-us"] != "Source words." || observed[q.ProjectID] != "Unsaved draft." {
		t.Fatal(observed)
	}
	if work.current != project || project.ProjectRevision() != before || len(project.Pack().Manifest().Fonts().Roles) != 0 {
		t.Fatal("preview published a draft")
	}
	q.Revision = "stale"
	if _, err := work.playback(context.Background(), q); err == nil {
		t.Fatal("stale editor accepted")
	}
}

func TestLocalizationPlaybackPreservesOriginalFailureAndCancelsSibling(t *testing.T) {
	source, project := playbackTestProjects(t)
	work := &localizationWork{localizationStateData: localizationStateData{current: project, source: &localizationSourceCache{native: source, checked: true}}}
	started := make(chan struct{})
	work.preview = func(ctx context.Context, id string, p, fallback *lk.AuthorPack, scenario textpreview.Scenario) (textpreview.Movie, error) {
		if p.Manifest().Metadata().ID == "native-us" {
			close(started)
			<-ctx.Done()
			return textpreview.Movie{}, ctx.Err()
		}
		<-started
		return textpreview.Movie{}, errors.New("draft font could not load")
	}
	_, err := work.playback(context.Background(), localizationDraftRequest{ProjectID: project.Pack().Manifest().Metadata().ID, Revision: project.ProjectRevision(), ID: "dialogue.event.relay.aitos", Body: "Draft.\n@end\n", Status: lk.TranslationWIP})
	if err == nil || !strings.Contains(err.Error(), "translation preview: draft font") {
		t.Fatal(err)
	}
}

func TestLocalizationPlaybackHTTPDoesNotHoldEditLock(t *testing.T) {
	worker := os.Getenv("AR_AUTHOR_TEXT_PREVIEW")
	if worker == "" {
		t.Skip("native preview worker unavailable")
	}
	source, project := playbackTestProjects(t)
	app := localizationTestApp(t)
	app.result.BinaryPath = worker
	// Fonts come from the repository; the project store stays in its temp root.
	root, err := filepath.Abs("../../..")
	if err != nil {
		t.Fatal(err)
	}
	if _, err = app.localizationSnapshot(); err != nil {
		t.Fatal(err)
	}
	app.options.ProjectRoot = root
	app.localization.current = project
	app.localization.source = &localizationSourceCache{native: source, checked: true}
	q := localizationRequest{ProjectID: project.Pack().Manifest().Metadata().ID, Revision: project.ProjectRevision(), ID: "dialogue.event.relay.aitos", Body: "Unsaved preview.\n@end\n", Status: lk.TranslationWIP}
	// Run the same read-only request as JSON and as an unsaved font upload.
	for _, upload := range []bool{false, true} {
		data, _ := json.Marshal(q)
		body := bytes.NewBuffer(data)
		contentType := "application/json"
		if upload {
			q.SaveFonts = true
			q.FontPaths = []string{"fonts/draft.ttf"}
			q.Fonts = lk.PackFonts{Primary: "builtin:actraiser-sans", Roles: []lk.PackFontRole{{Name: "draft", Primary: "fonts/draft.ttf"}}}
			q.Body = "<span font=\"draft\">Unsaved upload.</span>\n@end\n"
			q.SaveDetails = true
			q.Metadata = project.Pack().Manifest().Metadata()
			q.Metadata.Locale, q.Metadata.Direction = "fr", "rtl"
			data, _ = json.Marshal(q)
			body = &bytes.Buffer{}
			form := multipart.NewWriter(body)
			if err = form.WriteField("request", string(data)); err != nil {
				t.Fatal(err)
			}
			part, err := form.CreateFormFile("font0", "draft.ttf")
			if err != nil {
				t.Fatal(err)
			}
			font, err := os.ReadFile(filepath.Join(root, "game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf"))
			if err != nil {
				t.Fatal(err)
			}
			if _, err = part.Write(font); err != nil {
				t.Fatal(err)
			}
			if err = form.Close(); err != nil {
				t.Fatal(err)
			}
			contentType = form.FormDataContentType()
		}
		request := httptest.NewRequest("POST", "/secret/localization/playback", body)
		request.Header.Set("Content-Type", contentType)
		response := httptest.NewRecorder()
		// Holding the writer lock must not block the read-only preview request.
		app.localization.editMu.Lock()
		done := make(chan struct{})
		go func() { defer close(done); app.ServeHTTP(response, request) }()
		select {
		case <-done:
		case <-time.After(10 * time.Second):
			app.localization.editMu.Unlock()
			t.Fatal("preview waited for the edit lock")
		}
		app.localization.editMu.Unlock()
		if response.Code != 200 {
			t.Fatal(response.Code, response.Body.String())
		}
		var result struct {
			Source, Draft textpreview.Movie
			DraftLanguage string
		}
		if err = json.Unmarshal(response.Body.Bytes(), &result); err != nil {
			t.Fatal(err)
		}
		if len(result.Source.Frames) == 0 || len(result.Draft.Frames) == 0 || app.localization.current != project {
			t.Fatal("invalid preview publication")
		}
		if upload {
			if result.DraftLanguage != "fr" {
				t.Fatal("unsaved locale lost")
			}
			found := false
			for _, font := range result.Draft.Fonts {
				found = found || strings.HasSuffix(font.Reference, "fonts/draft.ttf")
			}
			if !found {
				t.Fatal("unsaved font not rendered", result.Draft.Fonts)
			}
			if len(project.Pack().Manifest().Fonts().Roles) != 0 {
				t.Fatal("preview saved draft font")
			}
		}
	}
}
