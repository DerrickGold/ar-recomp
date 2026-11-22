package builder

import (
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/desktop"
)

// Keep the source metadata and initial selection in the same atomic commit as
// the assignments. Other tracks already in the Workshop stay available to edit.
type musicDraftSource struct {
	modMetadata
	Excluded []string          `json:"excluded,omitempty"`
	Extras   map[string]string `json:"extras,omitempty"`
}

const draftSourceSection = "workshop:music-pack"
const draftImportDir = "pack-imports"

func readMusicDraftSource(manifest string) *musicDraftSource {
	raw, ok := manifestSectionValue(manifest, draftSourceSection, "source")
	if !ok {
		return nil
	}
	var source musicDraftSource
	if json.Unmarshal([]byte(raw), &source) != nil || validateModMetadata(source.modMetadata) != nil {
		return nil
	}
	return &source
}

func (source *musicDraftSource) archiveExtras(assets *os.Root) ([]modExtra, error) {
	var extras []modExtra
	if source == nil {
		return extras, nil
	}
	for _, name := range modExtraNames {
		file, ok := source.Extras[name]
		if !ok {
			continue
		}
		if !filepath.IsLocal(file) || filepath.Base(file) != file || !strings.HasPrefix(file, "extra-") {
			return nil, fmt.Errorf("invalid Workshop attachment for %s", name)
		}
		extras = append(extras, modExtra{name: name, open: func() (io.ReadCloser, error) {
			return assets.Open(filepath.Join(draftImportDir, file))
		}})
	}
	return extras, nil
}

func (app *application) serveWorkshopImport(w http.ResponseWriter, r *http.Request) {
	kind := r.URL.Query().Get("type")
	if kind != musicModType && kind != titleModType && kind != manualModType {
		writeJSONError(w, http.StatusBadRequest, "choose a music, title artwork, or manual pack in its Workshop editor")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxModBytes+maxModMetadata)
	if err := r.ParseMultipartForm(1 << 20); err != nil {
		writeJSONError(w, http.StatusBadRequest, "choose an .o2r file within the mod archive size limit")
		return
	}
	defer r.MultipartForm.RemoveAll()
	file, header, err := r.FormFile("pack")
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, "choose an .o2r pack to edit")
		return
	}
	defer file.Close()
	pack, err := readModArchive(file, header.Size)
	if err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	if pack.Type != kind {
		editors := map[string]string{musicModType: "Music", manualModType: "Create manual pack", titleModType: "Create title artwork pack", languageModType: "Languages"}
		writeJSONError(w, http.StatusBadRequest, "open this pack in Workshop → "+editors[pack.Type])
		return
	}
	if kind != musicModType {
		if err := sendWorkshopFile(w, pack); err != nil {
			writeJSONError(w, http.StatusBadRequest, err.Error())
		}
		return
	}
	app.assetMu.Lock()
	defer app.assetMu.Unlock()
	unlock, err := desktop.LockGameData(app.options.ProjectRoot)
	if err != nil {
		writeJSONError(w, http.StatusConflict, err.Error())
		return
	}
	defer unlock()
	root := musicDraftRoot(app.options.ProjectRoot)
	if err := importMusicDraft(root, pack); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	app.writeAssets(w, root, true)
}

func importMusicDraft(root string, pack modArchive) error {
	manifest, manifestPath, err := readAssetManifest(root)
	if err != nil {
		return err
	}
	tracks, err := modExportManifestTracks(root, manifest, manifestPath)
	if err != nil {
		return err
	}
	directory := filepath.Dir(manifestPath)
	imports := filepath.Join(directory, draftImportDir)
	for _, dir := range []string{filepath.Dir(root), root, directory, imports} {
		if err := ensureModDir(dir); err != nil {
			return err
		}
	}

	source := musicDraftSource{modMetadata: pack.modMetadata}
	incoming := make(map[string]bool, len(pack.Tracks))
	var sections strings.Builder
	var staged []string
	committed := false
	defer func() {
		if !committed {
			for _, file := range staged {
				_ = os.Remove(file)
			}
		}
	}()
	var total uint64
	for _, track := range pack.Tracks {
		member := pack.files[track.File]
		if member.UncompressedSize64 > maxModBytes-2*maxModMetadata-total {
			return fmt.Errorf("music assignments exceed the export size limit")
		}
		total += member.UncompressedSize64
		id := track.target()
		input, err := member.Open()
		if err != nil {
			return err
		}
		output, err := os.CreateTemp(imports, id+"-*.ogg")
		if err != nil {
			input.Close()
			return err
		}
		staged = append(staged, output.Name())
		err = copyModAudio(output, input)
		input.Close()
		closeErr := output.Close()
		if err != nil {
			return fmt.Errorf("%s: %w", id, err)
		}
		if closeErr != nil {
			return closeErr
		}
		incoming[id] = true
		manifest = removeManifestSection(manifest, "music:"+id)
		writeModMusicSection(&sections, id, musicModTargets[id], track, draftImportDir+"/"+filepath.Base(output.Name()))
	}
	for _, extra := range pack.extras() {
		file, err := os.CreateTemp(imports, "extra-*")
		if err != nil {
			return err
		}
		staged = append(staged, file.Name())
		n, err := extra.copyTo(file, int64(maxModBytes-2*maxModMetadata-total))
		closeErr := file.Close()
		if err != nil {
			return err
		}
		if closeErr != nil {
			return closeErr
		}
		total += uint64(n)
		if source.Extras == nil {
			source.Extras = make(map[string]string)
		}
		source.Extras[extra.name] = filepath.Base(file.Name())
	}
	for _, track := range tracks {
		if !incoming[track.ID] {
			source.Excluded = append(source.Excluded, track.ID)
		}
	}
	metadata, err := json.Marshal(source)
	if err != nil {
		return err
	}
	manifest = removeManifestSection(manifest, draftSourceSection) + sections.String() +
		"\n[" + draftSourceSection + "]\nsource = " + string(metadata) + "\n"
	if err := writeAtomicFile(manifestPath, []byte(manifest)); err != nil {
		return err
	}
	committed = true
	pruneWorkshopImports(manifestPath, manifest, source.Extras)
	return nil
}

func pruneWorkshopImports(manifestPath, manifest string, extras map[string]string) {
	imports := filepath.Join(filepath.Dir(manifestPath), draftImportDir)
	if info, err := os.Lstat(imports); err != nil || !info.IsDir() {
		return
	}
	// Only reap files from our private import directory. A different slot can
	// still refer to a previous import after merging two overlapping packs.
	keep := make(map[string]bool)
	for _, section := range manifestSections(manifest) {
		file := manifestSectionValues(manifest[section.Start:section.End])["file"]
		keep[filepath.Clean(resolveManifestFile(manifestPath, file))] = true
	}
	for _, file := range extras {
		keep[filepath.Join(imports, file)] = true
	}
	files, _ := os.ReadDir(imports)
	for _, file := range files {
		name := filepath.Join(imports, file.Name())
		owned := strings.HasSuffix(file.Name(), ".ogg") || strings.HasPrefix(file.Name(), "extra-")
		if owned && file.Type().IsRegular() && !keep[name] {
			_ = os.Remove(name)
		}
	}
}

// A single multipart response carries Unicode metadata and the original asset
// bytes directly into the native file input. File packs need no server draft.
func sendWorkshopFile(w http.ResponseWriter, pack modArchive) error {
	name := pack.Artwork
	if pack.Type == manualModType {
		name = pack.Manual
	}
	member := pack.files[name]
	input, err := member.Open()
	if err != nil {
		return err
	}
	if pack.Type == manualModType {
		_, err = copyModManual(io.Discard, input)
	} else {
		err = copyModTitle(io.Discard, input)
	}
	input.Close()
	if err != nil {
		return err
	}
	input, err = member.Open()
	if err != nil {
		return err
	}
	defer input.Close()
	metadata, err := json.Marshal(pack.modMetadata)
	if err != nil {
		return err
	}
	body := multipart.NewWriter(w)
	w.Header().Set("Content-Type", body.FormDataContentType())
	w.Header().Set("Cache-Control", "no-store")
	if body.WriteField("metadata", string(metadata)) != nil {
		return nil // Client disconnected after the response started.
	}
	part, err := body.CreateFormFile("asset", path.Base(name))
	if err != nil {
		return nil
	}
	if _, err = io.Copy(part, input); err != nil {
		return nil
	}
	for _, extra := range pack.extras() {
		part, err := body.CreateFormFile("attachment", extra.name)
		if err != nil {
			return nil
		}
		if _, err = extra.copyTo(part, maxTrackBytes); err != nil {
			return nil
		}
	}
	_ = body.Close()
	return nil
}
