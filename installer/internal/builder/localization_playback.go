package builder

import (
	"context"
	"errors"
	"fmt"
	"path/filepath"
	"strings"
	"sync"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"github.com/DerrickGold/ar-recomp/installer/internal/textpreview"
)

type localizationPlayback func(context.Context, string, *lk.AuthorPack, *lk.AuthorPack, textpreview.Scenario) (textpreview.Movie, error)

func (app *application) playbackLocalization(ctx context.Context, id string, p, fallback *lk.AuthorPack, scenario textpreview.Scenario) (textpreview.Movie, error) {
	app.mu.Lock()
	binary, building := app.result.BinaryPath, app.state == "building"
	app.mu.Unlock()
	if binary == "" || building {
		return textpreview.Movie{}, fmt.Errorf("finish building the game before using dialogue playback")
	}
	builtin := filepath.Join(app.options.ProjectRoot, "game-assets", "fonts", "noto", "NotoSans-SemiCondensedExtraBold.ttf")
	return textpreview.Run(ctx, binary, builtin, id, p, fallback, scenario)
}

func playbackScenario(p, source *lk.AuthorPack, id string, given *textpreview.Scenario) (textpreview.Scenario, error) {
	if given != nil {
		return *given, nil
	}
	scenario := textpreview.Defaults()
	references, err := lk.AuthorReferences(p.Manifest().Metadata().SourceProfile)
	if err != nil {
		return scenario, err
	}
	kinds := map[string]lk.AuthorPlaceholder{}
	for _, ref := range references {
		if ref.ID == id {
			for _, value := range ref.Placeholders {
				kinds[value.Name] = value
			}
		}
	}
	for _, pack := range []*lk.AuthorPack{p, source} {
		ops, err := pack.MessageOperations(id)
		if err != nil {
			continue
		}
		for _, op := range ops {
			if op.Op == "placeholder" {
				scenario.Values[op.Name] = playbackSample(p, source, id, kinds[op.Name])
			}
		}
		break
	}
	return scenario, nil
}

func playbackSample(pack, fallback *lk.AuthorPack, id string, value lk.AuthorPlaceholder) string {
	sampleID := map[string]string{"location_name": "city.fillmore.name", "current_city_name": "town.name.fillmore", "town_name": "town.name.fillmore", "enemy_name": "enemy.name.slot_00"}[value.Name]
	if sampleID != "" {
		for _, source := range []*lk.AuthorPack{pack, fallback} {
			ops, err := source.MessageOperations(sampleID)
			if err != nil {
				continue
			}
			var text strings.Builder
			for _, op := range ops {
				if op.Op == "text" {
					text.WriteString(op.Value)
				}
			}
			if text.Len() != 0 {
				return text.String()
			}
		}
	}
	if value.Name == "hud_value" {
		switch id {
		case "action.hud.lives_value":
			return "07"
		case "action.hud.time_value":
			return "123"
		case "action.hud.score_value":
			return "00500"
		default:
			return "0123/0456"
		}
	}
	return textpreview.ValueSample(value)
}

func (work *localizationWork) playback(ctx context.Context, q localizationRequest) (any, error) {
	if err := work.checkLocalizationIdentity(q.ProjectID, q.Revision); err != nil {
		return nil, err
	}
	project, err := previewLocalizationDraft(work.current, q)
	if err != nil {
		return nil, err
	}

	native := work.nativeLocalizationSource()
	if project.Origin() == "native-source" {
		native = project
	}
	if native == nil {
		return nil, fmt.Errorf("extract the native source before playback")
	}
	source := native
	if work.reference != nil {
		if view, ok := work.reference.Pack().Workspace().Message(q.ID); ok && view.Present {
			source = work.reference
		}
	}
	scenario, err := playbackScenario(project.Pack(), source.Pack(), q.ID, q.Scenario)
	if err != nil {
		return nil, err
	}
	sourceScenario, err := playbackScenario(source.Pack(), source.Pack(), q.ID, q.SourceScenario)
	if err != nil {
		return nil, err
	}
	// Each worker has its own immutable pack, fonts, reveal clock and output.
	// A cancelled request kills both children without holding the editor lock.
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	var sourceMovie, draftMovie textpreview.Movie
	var sourceErr, draftErr error
	var jobs sync.WaitGroup
	jobs.Add(2)
	go func() {
		defer jobs.Done()
		sourceMovie, sourceErr = work.preview(ctx, q.ID, source.Pack(), source.Pack(), sourceScenario)
		if sourceErr != nil {
			cancel()
		}
	}()
	go func() {
		defer jobs.Done()
		draftMovie, draftErr = work.preview(ctx, q.ID, project.Pack(), native.Pack(), scenario)
		if draftErr != nil {
			cancel()
		}
	}()
	jobs.Wait()
	if sourceErr != nil && (draftErr == nil || !errors.Is(sourceErr, context.Canceled)) {
		return nil, fmt.Errorf("source preview: %w", sourceErr)
	}
	if draftErr != nil {
		return nil, fmt.Errorf("translation preview: %w", draftErr)
	}
	return map[string]any{"source": sourceMovie, "draft": draftMovie, "scenario": scenario, "sourceScenario": sourceScenario,
		"projectID": q.ProjectID, "revision": q.Revision, "sourceLanguage": source.Pack().Manifest().Metadata().Locale,
		"draftLanguage": project.Pack().Manifest().Metadata().Locale}, nil
}

// Both previews validate the same detached edits. No project/store state is
// published, and metadata is limited to the editable author fields.
func previewLocalizationDraft(project *lk.AuthorProject, q localizationRequest) (*lk.AuthorProject, error) {
	if project.Origin() == "native-source" {
		return project, nil
	}
	var err error
	if q.SaveDetails {
		m := project.Pack().Manifest().Metadata()
		m.Name, m.Locale, m.Autonym = q.Metadata.Name, q.Metadata.Locale, q.Metadata.Autonym
		m.Author, m.License, m.Direction = q.Metadata.Author, q.Metadata.License, q.Metadata.Direction
		project, err = project.WithMetadata(m)
		if err != nil {
			return nil, err
		}
	}
	if q.SaveFonts {
		return project.EditMessageAndFonts(q.ID, q.Body, q.Status, q.Fonts, q.fontUploads)
	}
	return project.EditMessage(q.ID, q.Body, q.Status)
}
