package builder

import (
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
)

const maxAssetRequestBytes = 1 << 30

var errUnknownTitleArtwork = errors.New("select an available title artwork variant")

func (app *application) saveAssets(response http.ResponseWriter, request *http.Request) {
	app.assetMu.Lock()
	defer app.assetMu.Unlock()

	request.Body = http.MaxBytesReader(response, request.Body, maxAssetRequestBytes)
	if err := request.ParseMultipartForm(8 << 20); err != nil {
		writeJSONError(response, http.StatusBadRequest,
			"could not read the selected asset files")
		return
	}
	if request.MultipartForm == nil {
		writeJSONError(response, http.StatusBadRequest,
			"asset saves must use a multipart form")
		return
	}
	defer request.MultipartForm.RemoveAll()

	manifest, manifestPath, err := readAssetManifest(app.options.ProjectRoot)
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	plan := assetSavePlan{root: app.options.ProjectRoot, manifest: manifest, manifestPath: manifestPath}
	defer plan.discardStaged()
	if err := plan.prepareTitle(request); err != nil {
		status := http.StatusInternalServerError
		if errors.Is(err, errUnknownTitleArtwork) {
			status = http.StatusBadRequest
		}
		writeJSONError(response, status, err.Error())
		return
	}
	if err := plan.prepareTracks(request); err != nil {
		writeJSONError(response, http.StatusBadRequest, err.Error())
		return
	}
	plan.prepareSplits(request)
	if err := plan.prepareVariants(request); err != nil {
		writeJSONError(response, http.StatusBadRequest, err.Error())
		return
	}
	// A file-only first save must also materialize the live manifest.
	if plan.touched && !regularFileExists(plan.manifestPath) {
		plan.changed = true
	}
	if !plan.changed && !plan.touched {
		configuration, loadErr := loadAssetConfiguration(plan.root)
		if loadErr != nil {
			writeJSONError(response, http.StatusInternalServerError, loadErr.Error())
			return
		}
		writeJSON(response, http.StatusOK, map[string]any{
			"message": "No asset changes were selected.", "changed": false, "config": configuration,
		})
		return
	}

	if err := plan.install(); err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}

	configuration, err := loadAssetConfiguration(app.options.ProjectRoot)
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(response, http.StatusOK, map[string]any{
		"message": "Assets saved. Changes apply the next time the game starts.",
		"changed": true,
		"config":  configuration,
	})
}

// assetSavePlan owns staged files until installation finishes. Manifest edits
// and file edits are tracked separately so an audio-only save preserves the
// player's manifest bytes. Preparation never replaces an installed file.
type assetSavePlan struct {
	root, manifestPath, manifest string
	changed, touched             bool
	removeTitle                  string // Only a selected, builder-owned content-addressed path.
	staged                       []stagedAsset
	reverted                     []string
}

func (plan *assetSavePlan) discardStaged() {
	for _, asset := range plan.staged {
		if asset.Temporary != "" {
			_ = os.Remove(asset.Temporary)
		}
	}
}

func (plan *assetSavePlan) prepareTitle(request *http.Request) error {
	if request.FormValue("title-change") == "1" {
		artwork, ok := bundledTitleArtwork(request.FormValue("title-variant"))
		if !ok {
			return errUnknownTitleArtwork
		}
		relative := bundledTitleRelativePath(artwork)
		logo, swirl := titleManifestValues(relative)
		plan.manifest = upsertManifestSection(plan.manifest, "replace:title-logo", logo)
		plan.manifest = upsertManifestSection(plan.manifest, "replace:title-swirl", swirl)
		if request.FormValue("title") == "on" {
			asset, stageErr := stageBytes(plan.root, relative, artwork)
			if stageErr != nil {
				return fmt.Errorf("prepare bundled title art: %w", stageErr)
			}
			plan.staged = append(plan.staged, asset)
		} else {
			plan.removeTitle = relative
		}
		plan.changed = true
		plan.touched = true
	}

	return nil
}

func (plan *assetSavePlan) prepareTracks(request *http.Request) error {
	for _, track := range assetTracks {
		relative, target, reachable := slotAudioTarget(
			plan.root, plan.manifest, plan.manifestPath, track)
		files := request.MultipartForm.File["track-"+track.ID]
		if len(files) == 0 || (files[0].Filename == "" && files[0].Size == 0) {
			// Reverting to the original ROM music. Checked only when no
			// replacement was also chosen, so a slot that is cleared and then
			// re-filled in one visit ends up with the new file rather than
			// racing its own removal.
			if request.FormValue("track-remove-"+track.ID) != "1" {
				continue
			}
			// The FILE goes; the RECORD stays, naming the same path. That is
			// what keeps the hand-managed route working: the slot is back to
			// authentic audio now, and dropping a file at that path re-engages
			// it without the manifest needing an entry put back by hand.
			// Removing the record instead left the shipped stub gone for good,
			// since the template never re-seeds an existing manifest.
			if reachable {
				plan.reverted = append(plan.reverted, target)
				plan.touched = true
			}
			continue
		}
		if len(files) != 1 {
			return fmt.Errorf("select only one file for %s", track.Name)
		}
		if !reachable {
			return fmt.Errorf("%s points outside game-assets; edit its manifest record to a path inside it, or manage that file by hand", track.Name)
		}
		asset, stageErr := stageAudioUpload(plan.root, target, files[0])
		if stageErr != nil {
			return stageErr
		}
		plan.staged = append(plan.staged, asset)
		plan.touched = true
		// The manifest is touched only when it does not already say this. A
		// record that names the target path is left exactly as the user wrote
		// it, gain/loop/gate keys and all -- re-upserting identical values
		// would rewrite the file on every upload for nothing.
		if current, found := manifestSectionValue(
			plan.manifest, "music:"+track.ID, "file"); !found || current != relative {
			values := []manifestValue{
				{Key: "src", Value: track.Src},
				{Key: "file", Value: relative},
			}
			if track.Loop != nil {
				loop := "0"
				if *track.Loop {
					loop = "1"
				}
				values = append(values, manifestValue{Key: "loop", Value: loop})
			}
			plan.manifest = upsertManifestSection(plan.manifest, "music:"+track.ID, values)
			plan.changed = true
			plan.touched = true
		}
	}

	return nil
}

func (plan *assetSavePlan) prepareSplits(request *http.Request) {

	// Splits: the builder authors these records, because the region byte is a
	// fixed table it already knows. Only sections in its own <slot>-<region>
	// namespace are created or removed; a hand-authored variant beside them is
	// never touched.
	for _, track := range assetTracks {
		if request.FormValue("split-change-"+track.ID) != "1" {
			continue
		}
		wanted := make(map[string]bool)
		for _, slug := range request.Form["split-"+track.ID] {
			wanted[slug] = true
		}
		for _, region := range splitRegions(track) {
			name := "music:" + splitSectionName(track.ID, region.Slug)
			_, exists := findManifestSection(plan.manifest, name)
			if wanted[region.Slug] == exists {
				continue
			}
			if !exists {
				// Created as a STUB: the record and its gate, naming a file
				// that is not there yet. That is the manifest's own idiom for
				// an available hook, and it makes the new row appear straight
				// away with a picker on it.
				plan.manifest = upsertManifestSection(plan.manifest, name, []manifestValue{
					{Key: "src", Value: track.Src},
					{Key: "when", Value: splitGate(region.Group)},
					{Key: "file", Value: splitStubFile(track.ID, region.Slug)},
				})
				plan.changed = true
				plan.touched = true
				continue
			}
			if file, found := manifestSectionValue(plan.manifest, name, "file"); found {
				if path, ok := manifestAudioTarget(
					plan.root, plan.manifestPath, file); ok {
					plan.reverted = append(plan.reverted, path)
				}
			}
			plan.manifest = removeManifestSection(plan.manifest, name)
			plan.changed = true
			plan.touched = true
		}
	}

}

// prepareVariants stages files for managed splits without rewriting their records.
func (plan *assetSavePlan) prepareVariants(request *http.Request) error {
	// Same namespace the rows come from, so a crafted request cannot reach a
	// record the page would not have shown.
	for _, track := range assetTracks {
		for _, region := range splitRegions(track) {
			name := splitSectionName(track.ID, region.Slug)
			section := "music:" + name
			if _, exists := findManifestSection(plan.manifest, section); !exists {
				continue
			}
			file, hasFile := manifestSectionValue(plan.manifest, section, "file")
			if !hasFile {
				continue
			}
			target, reachable := manifestAudioTarget(
				plan.root, plan.manifestPath, file)
			if !reachable {
				continue
			}
			files := request.MultipartForm.File["variant-"+name]
			hasUpload := len(files) > 0 &&
				!(files[0].Filename == "" && files[0].Size == 0)
			if !hasUpload {
				if request.FormValue("variant-remove-"+name) == "1" {
					plan.reverted = append(plan.reverted, target)
					plan.touched = true
				}
				continue
			}
			if len(files) != 1 {
				return fmt.Errorf("select only one file for %s", name)
			}
			asset, err := stageAudioUpload(
				plan.root, target, files[0])
			if err != nil {
				return err
			}
			plan.staged = append(plan.staged, asset)
			plan.touched = true
		}
	}
	return nil
}

func (plan *assetSavePlan) install() error {
	// Install staged files before publishing manifest changes. Audio replaces
	// its existing path; title art uses a content-addressed path. A later failure
	// can leave earlier files updated, matching the existing save contract.
	for index := range plan.staged {
		if err := replaceStagedFile(plan.staged[index]); err != nil {
			return fmt.Errorf("install replacement asset: %w", err)
		}
		plan.staged[index].Temporary = ""
	}
	// Only when a managed record actually changed. A variant-only save must
	// leave the file byte-for-byte alone: rewriting it would reformat records
	// the builder does not own for no reason at all.
	if plan.changed {
		if err := writeAtomicFile(plan.manifestPath, []byte(plan.manifest)); err != nil {
			return fmt.Errorf("update asset manifest: %w", err)
		}
	}

	// Process removals after manifest edits. Base song stubs deliberately keep
	// their paths: a missing replacement selects the original ROM audio. Removed
	// split entries may leave an orphaned file if a save is interrupted here.
	for _, path := range plan.reverted {
		if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("remove the replaced music file: %w", err)
		}
	}

	// Turning the included title off means the manifest keeps a valid hook but
	// its builder-owned image is absent. The embedded bytes make this deletion
	// recoverable: checking the toggle and saving restores the exact file.
	if plan.removeTitle != "" {
		managedTitle := filepath.Join(plan.root, "game-assets",
			filepath.FromSlash(plan.removeTitle))
		if err := os.Remove(managedTitle); err != nil && !errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("disable bundled title art: %w", err)
		}
	}

	return nil
}
