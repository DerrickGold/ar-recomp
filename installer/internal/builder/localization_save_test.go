package builder

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

func saveInstalledFixture(t *testing.T, app *application, archive, enabled bool) lk.InstalledPackSummary {
	t.Helper()
	root := filepath.Join(app.localizationRoot(), "packs")
	p := app.localization.current
	key := p.Pack().Manifest().Metadata().ID
	if archive {
		var err error
		p, err = p.EditMessage("action.hud.act_1", "Previously published text.\n@end\n", lk.TranslationDone)
		if err != nil {
			t.Fatal(err)
		}
		published, _, err := p.Publication(lk.PublicationOptions{ConfirmRights: true})
		if err != nil {
			t.Fatal(err)
		}
		var data bytes.Buffer
		if err := published.WriteArchive(&data, "publication"); err != nil {
			t.Fatal(err)
		}
		key = "My installed translation.arlang"
		if err := os.MkdirAll(root, 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(root, key), data.Bytes(), 0644); err != nil {
			t.Fatal(err)
		}
	} else {
		locJSON(t, app, "install", locIdentity(app), 200)
	}
	row, err := lk.InspectInstalledPack(root, key)
	if err != nil {
		t.Fatal(err)
	}
	if !enabled {
		if err := lk.SetLanguagePackEnabled(root, key, row.Metadata.ID, row.Revision, false); err != nil {
			t.Fatal(err)
		}
		row, err = lk.InspectInstalledPack(root, key)
		if err != nil {
			t.Fatal(err)
		}
	}
	return row
}

func saveMessageRequest(app *application) localizationRequest {
	q := locIdentity(app)
	q.SaveMessage = true
	q.ID, q.Body, q.Status = "action.hud.act_1", "Saved for testing!\n@end\n", lk.TranslationWIP
	return q
}

func TestLocalizationSaveRefreshesInstalledPack(t *testing.T) {
	for _, archive := range []bool{false, true} {
		for _, enabled := range []bool{false, true} {
			t.Run(fmt.Sprintf("archive=%t/enabled=%t", archive, enabled), func(t *testing.T) {
				app := editableWorkflowFixture(t)
				project, _, err := app.localization.current.UpgradeV2("test.workflow-v2")
				if err != nil {
					t.Fatal(err)
				}
				if err := app.saveLocalization(project, ""); err != nil {
					t.Fatal(err)
				}
				before := saveInstalledFixture(t, app, archive, enabled)
				q := saveMessageRequest(app)
				q.SaveFonts = true
				q.Fonts = lk.PackFonts{Primary: "builtin:actraiser-sans", Roles: []lk.PackFontRole{{Name: "hud", Primary: "builtin:actraiser-sans"}}}
				q.SaveDetails, q.SaveNotice = true, true
				q.Metadata = app.localization.current.Pack().Manifest().Metadata()
				q.Metadata.Name, q.Notes = "Renamed translation", "Private author note"
				q.NoticeName, q.NoticeText = "CREDITS.txt", "Public contributor credits"
				var result struct {
					InstallationUpdate localizationInstallationUpdate
				}
				if err := json.Unmarshal(locJSON(t, app, "save", q, 200).Body.Bytes(), &result); err != nil {
					t.Fatal(err)
				}
				update := result.InstallationUpdate
				if !update.Updated || !update.Installed || update.Enabled != enabled || update.Error != "" {
					t.Fatal(update)
				}
				root := filepath.Join(app.localizationRoot(), "packs")
				row, err := lk.InspectInstalledPack(root, before.Key)
				if err != nil || row.Revision == before.Revision || row.Enabled != enabled || row.Metadata.Name != q.Metadata.Name {
					t.Fatal("installation was not refreshed in place", row, err)
				}
				if !enabled { // Open the saved copy through the normal author reader.
					if err := lk.SetLanguagePackEnabled(root, row.Key, row.Metadata.ID, row.Revision, true); err != nil {
						t.Fatal(err)
					}
				}
				installed, err := lk.OpenAuthorInput(filepath.Join(root, row.Key))
				if err != nil {
					t.Fatal(err)
				}
				view, _ := installed.Pack().Workspace().Message(q.ID)
				if !strings.Contains(view.Body, "Saved for testing!") || installed.Pack().Manifest().Version() != 2 {
					t.Fatal("new draft message missing from game copy", view)
				}
				if roles := installed.Pack().Manifest().Fonts().Roles; len(roles) != 1 || roles[0].Name != "hud" {
					t.Fatal("saved font roles missing from game copy", roles)
				}
				if installed.Notes() != "" || installed.Notices()["notices/CREDITS.txt"] != q.NoticeText {
					t.Fatal("private notes or public credits handled incorrectly", installed.Notices())
				}
				if err := app.localization.current.WriteArchive(&bytes.Buffer{}, "publication"); err == nil {
					t.Fatal("saving granted publication rights")
				}
			})
		}
	}
}

func TestLocalizationSaveDoesNotInstallMissingPack(t *testing.T) {
	for _, uninstalled := range []bool{false, true} {
		t.Run(fmt.Sprint(uninstalled), func(t *testing.T) {
			app := editableWorkflowFixture(t)
			root := filepath.Join(app.localizationRoot(), "packs")
			if uninstalled {
				row := saveInstalledFixture(t, app, false, true)
				if _, err := lk.UninstallLanguagePack(root, row.Key, row.Metadata.ID, row.Revision); err != nil {
					t.Fatal(err)
				}
			}
			// A different package with the same locale must not be overwritten.
			other := app.localization.current
			metadata := other.Pack().Manifest().Metadata()
			metadata.ID = "test.other"
			other, err := other.WithMetadata(metadata)
			if err != nil {
				t.Fatal(err)
			}
			other, _, err = other.Installation()
			if err != nil {
				t.Fatal(err)
			}
			path, err := lk.InstallProjectInLibrary(root, other, false)
			if err != nil {
				t.Fatal(err)
			}
			before, _ := os.ReadFile(path)
			response := locJSON(t, app, "save", saveMessageRequest(app), 200)
			var result struct {
				InstallationUpdate localizationInstallationUpdate
			}
			json.Unmarshal(response.Body.Bytes(), &result)
			if result.InstallationUpdate != (localizationInstallationUpdate{}) {
				t.Fatal(response.Body.String())
			}
			after, _ := os.ReadFile(path)
			rows, err := lk.ListInstalledPacks(root)
			if err != nil || len(rows) != 1 || !bytes.Equal(before, after) {
				t.Fatal("save changed the installed library", rows, err)
			}
		})
	}
}

func TestLocalizationSaveKeepsProgressWhenRefreshFails(t *testing.T) {
	app := editableWorkflowFixture(t)
	row := saveInstalledFixture(t, app, false, true)
	root := filepath.Join(app.localizationRoot(), "packs")
	app.options.fontCoverageProbe = func(context.Context, []lk.FontCoverageSource, []rune) (lk.FontCoverageProbeResult, error) {
		return lk.FontCoverageProbeResult{}, fmt.Errorf("font backend unavailable")
	}
	q := saveMessageRequest(app)
	var result struct {
		Project            struct{ Revision string }
		InstallationUpdate localizationInstallationUpdate
	}
	if err := json.Unmarshal(locJSON(t, app, "save", q, 200).Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	saved, err := app.localization.store.Open(q.ProjectID)
	if err != nil || result.Project.Revision == q.Revision || saved.ProjectRevision() != result.Project.Revision {
		t.Fatal("refresh failure lost the author save", result, err)
	}
	if result.InstallationUpdate.Updated || !strings.Contains(result.InstallationUpdate.Error, "font backend unavailable") {
		t.Fatal(result.InstallationUpdate)
	}
	after, err := lk.InspectInstalledPack(root, row.Key)
	if err != nil || after.Revision != row.Revision {
		t.Fatal("failed refresh changed game copy", after, err)
	}
}

type failedLocalizationSaveStore struct{ localizationProjectStore }

func (failedLocalizationSaveStore) Save(*lk.AuthorProject, string) error {
	return fmt.Errorf("project storage unavailable")
}

func TestLocalizationSaveFailureDoesNotUpdateInstallation(t *testing.T) {
	for _, invalid := range []bool{false, true} {
		t.Run(fmt.Sprintf("invalid=%t", invalid), func(t *testing.T) {
			app := editableWorkflowFixture(t)
			row := saveInstalledFixture(t, app, false, true)
			q := saveMessageRequest(app)
			if invalid {
				q.Body = "@invalid-control\n@end\n"
			} else {
				app.localization.store = failedLocalizationSaveStore{app.localization.store}
			}
			locJSON(t, app, "save", q, 400)
			onDisk, err := app.localization.store.Open(q.ProjectID)
			if err != nil || onDisk.ProjectRevision() != q.Revision || app.localization.current.ProjectRevision() != q.Revision {
				t.Fatal("failed save changed project state", err)
			}
			after, err := lk.InspectInstalledPack(filepath.Join(app.localizationRoot(), "packs"), row.Key)
			if err != nil || after.Revision != row.Revision {
				t.Fatal("failed save changed game copy", after, err)
			}
		})
	}
}
