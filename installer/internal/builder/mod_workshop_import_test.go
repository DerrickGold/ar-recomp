package builder

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/json"
	"image/color"
	"io"
	"mime"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"slices"
	"testing"
)

func TestWorkshopMusicImportEditAndExport(t *testing.T) {
	project := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: project}, "tok")
	root := musicDraftRoot(project)
	audio := testModAudio(t)
	saveTrack := func(id string) {
		t.Helper()
		response := postAssetFormTo(t, app, "/tok/music-draft", nil, map[string][]byte{"track-" + id: audio})
		if response.Code != http.StatusOK {
			t.Fatal(response.Body)
		}
	}
	saveTrack("song-01")
	saveTrack("title-theme")
	loop, start, end, gain := false, uint32(200), uint32(2300), 80
	metadata := modMetadata{Name: "Thèmes", Version: "1.2.0", Description: "Original\ntrack selection", License: "CC BY"}
	tracks := []modTrack{{Track: "title-theme", File: "music/opening.ogg"}, {
		Track: "song-09", Region: "fillmore", File: "music/opening.ogg", Loop: &loop, LoopStart: &start, LoopEnd: &end, Gain: &gain,
	}}
	// A single archive member may be used by multiple targets. Each draft
	// slot must become independently editable.
	pack := testMusicMod(t, "published", tracks, "")
	rewrite := func(broken bool) []byte {
		t.Helper()
		var output bytes.Buffer
		archive := zip.NewWriter(&output)
		original, err := zip.NewReader(bytes.NewReader(pack), int64(len(pack)))
		if err != nil {
			t.Fatal(err)
		}
		for _, original := range original.File {
			switch {
			case original.Name == "manifest.json":
				member, _ := archive.Create(original.Name)
				err = json.NewEncoder(member).Encode(metadata)
			case broken && original.Name == "music/opening.ogg":
				member, _ := archive.Create(original.Name)
				_, err = io.WriteString(member, "not music")
			default:
				err = archive.Copy(original)
			}
			if err != nil {
				t.Fatal(err)
			}
		}
		readme, _ := archive.Create("README.md")
		io.WriteString(readme, "Composer credits")
		if err := archive.Close(); err != nil {
			t.Fatal(err)
		}
		return output.Bytes()
	}
	importArchive := func(pack []byte, kind string) *httptest.ResponseRecorder {
		return postAssetFormTo(t, app, "/tok/mods/import-draft?type="+kind, nil, map[string][]byte{"pack": pack})
	}
	original := rewrite(false)
	response := importArchive(original, musicModType)
	var config assetConfiguration
	if err := json.Unmarshal(response.Body.Bytes(), &config); response.Code != 200 || err != nil || config.Source == nil {
		t.Fatalf("import: %d %s", response.Code, response.Body)
	}
	if config.Source.modMetadata != metadata || !slices.Equal(config.Source.Excluded, []string{"song-01"}) {
		t.Fatalf("wrong source defaults: %+v", config.Source)
	}
	for _, id := range []string{"title-theme", "song-09-fillmore"} {
		preview := httptest.NewRecorder()
		app.ServeHTTP(preview, httptest.NewRequest(http.MethodGet, "/tok/music-draft-audio/"+id, nil))
		if preview.Code != 200 || !bytes.Equal(audio, preview.Body.Bytes()) {
			t.Fatalf("preview for %s: %d", id, preview.Code)
		}
	}
	saveTrack("song-02")
	manifest, _, _ := readAssetManifest(root)
	if response := importArchive(rewrite(true), musicModType); response.Code != 400 {
		t.Fatalf("corrupt import returned %d", response.Code)
	}
	if response := importArchive(original, manualModType); response.Code != 400 {
		t.Fatalf("wrong editor returned %d", response.Code)
	}
	if after, _, _ := readAssetManifest(root); after != manifest {
		t.Fatal("failed import changed previous draft")
	}
	reopened, err := loadAssetView(root, true)
	if err != nil || reopened.Source == nil || reopened.Source.modMetadata != metadata {
		t.Fatal("reopening lost the pack details", err)
	}
	metadata.Version = "1.3.0"
	var updated bytes.Buffer
	err = exportMusicMod(root, &updated, modExportRequest{modMetadata: metadata, Targets: []string{"song-02", "title-theme", "song-09-fillmore"}})
	if err != nil {
		t.Fatal(err)
	}
	release, err := readModArchive(bytes.NewReader(updated.Bytes()), int64(updated.Len()))
	if err != nil || release.modMetadata != metadata || len(release.Tracks) != 3 {
		t.Fatalf("release: %v %+v", err, release.modPayload)
	}
	variant := release.Tracks[1]
	if variant.target() != "song-09-fillmore" || *variant.Loop || *variant.LoopStart != start || *variant.LoopEnd != end || *variant.Gain != gain {
		t.Fatalf("lost playback settings: %+v", variant)
	}
	if release.files["README.md"] == nil {
		t.Fatal("missing README in the revision")
	}
	readme, err := release.files["README.md"].Open()
	if err != nil {
		t.Fatal(err)
	}
	credits, err := io.ReadAll(readme)
	readme.Close()
	if err != nil || string(credits) != "Composer credits" {
		t.Fatal("lost credits", err)
	}
	finished := postAssetFormTo(t, app, "/tok/music-draft", map[string]string{"finish-pack": "1"}, nil)
	ready, err := loadAssetView(root, true)
	remaining, trackErr := modExportTracks(root)
	if finished.Code != 200 || err != nil || trackErr != nil || ready.Source != nil || len(remaining) != 4 {
		t.Fatalf("finish must keep the four saved assignments: %d %+v %v %v", finished.Code, remaining, err, trackErr)
	}
	if hasModSources(project, nil) || regularFileExists(liveAssetManifestPath(project)) {
		t.Fatal("authoring changed the game's assets")
	}
}

func TestWorkshopFilePackImportAndRepublish(t *testing.T) {
	for _, kind := range []string{titleModType, manualModType} {
		t.Run(kind, func(t *testing.T) {
			app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
			export, field, endpoint := exportTitleMod, "artwork", "export-title"
			var asset []byte
			if kind == manualModType {
				export, field, endpoint = exportManualMod, "manual", "export-manual"
				asset = albumPDF(t, 2)
			} else {
				asset = authoredTitlePNG(t, color.NRGBA{R: 32, A: 255})
			}
			metadata := modMetadata{Name: "作品", Version: "1.0", Description: "日本語\nCredits", License: "CC BY"}
			var original bytes.Buffer
			extra := modExtra{name: "LICENSE", open: func() (io.ReadCloser, error) { return io.NopCloser(bytes.NewBufferString("Attribution")), nil }}
			if err := export(&original, metadata, "", bytes.NewReader(asset), extra); err != nil {
				t.Fatal(err)
			}
			r := postAssetFormTo(t, app, "/tok/mods/import-draft?type="+kind, nil, map[string][]byte{"pack": original.Bytes()})
			_, params, err := mime.ParseMediaType(r.Header().Get("Content-Type"))
			if err != nil || r.Code != 200 {
				t.Fatalf("import: %d %s", r.Code, r.Body)
			}
			form, err := multipart.NewReader(r.Body, params["boundary"]).ReadForm(1 << 20)
			if err != nil {
				t.Fatal(err)
			}
			defer form.RemoveAll()
			var details modMetadata
			if err := json.Unmarshal([]byte(form.Value["metadata"][0]), &details); err != nil || details != metadata {
				t.Fatal("metadata mismatch", err)
			}
			uploads := map[string][]byte{}
			for name, files := range form.File {
				f, err := files[0].Open()
				if err != nil {
					t.Fatal(err)
				}
				data, err := io.ReadAll(f)
				f.Close()
				if err != nil {
					t.Fatal(err)
				}
				switch name {
				case "asset":
					if !bytes.Equal(data, asset) {
						t.Fatal("asset mismatch")
					}
					uploads[field] = data
				case "attachment":
					uploads["attachment:"+files[0].Filename] = data
				}
			}
			r = postAssetFormTo(t, app, "/tok/mods/"+endpoint, map[string]string{"name": metadata.Name, "description": metadata.Description, "version": "2.0", "license": metadata.License}, uploads)
			defer app.replaceModDownload(nil)
			var download struct{ URL string }
			if err := json.Unmarshal(r.Body.Bytes(), &download); err != nil || r.Code != 200 {
				t.Fatalf("export: %d %s", r.Code, r.Body)
			}
			out := httptest.NewRecorder()
			app.ServeHTTP(out, httptest.NewRequest(http.MethodGet, "/tok/"+download.URL, nil))
			release, err := readModArchive(bytes.NewReader(out.Body.Bytes()), int64(out.Body.Len()))
			if err != nil || release.Version != "2.0" || release.Description != metadata.Description || release.files["LICENSE"] == nil {
				t.Fatal("revised release lost imported data", err)
			}
		})
	}
}
