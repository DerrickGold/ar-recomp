package builder

import (
	"net/http"
	"os"
	"path/filepath"
)

// assetSplitStatus is one region offered for a slot, and whether that slot
// currently has a record for it.
type assetSplitStatus struct {
	Slug    string `json:"slug"`
	Label   string `json:"label"`
	Acts    byte   `json:"acts"` // Semantic act mask; the browser translates presentation only.
	Enabled bool   `json:"enabled"`
	// Name, Gate and File are exactly what a save WILL write for this region.
	// Sent up front so the page can show the row the moment the box is ticked,
	// instead of making the reader save once to reveal the picker and again to
	// fill it -- and so the row it shows is the record that will exist, not the
	// page's own guess at one.
	Name string `json:"name"`
	Gate string `json:"gate"`
	File string `json:"file"`
}

type assetTitleStatus struct {
	Enabled bool   `json:"enabled"`
	File    string `json:"file,omitempty"`
}

type assetTrackStatus struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Src        string `json:"src"`
	Configured bool   `json:"configured"`
	File       string `json:"file,omitempty"`
	// URL plays the replacement that is ALREADY installed. Without it a
	// replacement could only be heard in the session that uploaded it: the
	// row's player is fed by URL.createObjectURL on the file the reader just
	// picked, so re-opening the builder left the audio unplayable and the only
	// way to hear what was installed was to upload it again.
	URL string `json:"url,omitempty"`
	// Variants are the OTHER manifest records that name this same ROM song --
	// hand-authored gated alternates like a per-region act theme. They are
	// listed under the slot they belong to because that is what they are: the
	// song is identified by src, and the section name is only a label.
	Variants []assetVariantStatus `json:"variants,omitempty"`
	// Splits is the per-region offer: which levels this song can be given its
	// own track in, and which already have one.
	Splits []assetSplitStatus `json:"splits,omitempty"`
}

// assetVariantStatus is a gated alternate for one slot. The builder supplies
// its FILE and nothing else: `when` gates are hand-authored, an entry is
// dropped outright if it loses its `src` or `file`, and rewriting a record
// somebody wrote by hand to express a condition is not the builder's business.
// So a variant is managed exactly the way the manifest header tells a user to
// manage one by hand -- by putting a file at the path the record already names.
type assetVariantStatus struct {
	Name       string `json:"name"`
	When       string `json:"when,omitempty"`
	File       string `json:"file"`
	Configured bool   `json:"configured"`
	URL        string `json:"url,omitempty"`
}

type assetConfiguration struct {
	Title        assetTitleStatus   `json:"title"`
	Tracks       []assetTrackStatus `json:"tracks"`
	ManifestPath string             `json:"manifestPath"`
}

// installedAudioURL is versioned by the file itself for the same reason the ROM
// previews are: saving a new replacement reuses the track id, so a URL keyed
// only on that id would let a browser serve the previous file's cached bytes.
func installedAudioURL(root, manifestPath, id, value string) string {
	path, ok := installedAudioPath(root, manifestPath, value)
	if !ok {
		return ""
	}
	return "asset-audio/" + id + "?v=" + previewVersionToken(path)
}

// managedTrackID reports whether a name is one the builder itself manages: a
// song-table slot, or a split it can write under <slot>-<region>. Everything
// the builder will not edit it also will not serve -- one answer to "is this
// mine", used by the rows, the save path and the playback route alike.
func managedTrackID(id string) bool {
	for _, track := range assetTracks {
		if track.ID == id {
			return true
		}
		for _, region := range splitRegions(track) {
			if splitSectionName(track.ID, region.Slug) == id {
				return true
			}
		}
	}
	return false
}

func builtInTrackIDs() map[string]bool {
	known := make(map[string]bool, len(assetTracks))
	for _, track := range assetTracks {
		known[track.ID] = true
	}
	return known
}

// manifestVariants lists the builder-owned <slot>-<region> records with
// reachable file paths. Matching by source address would also expose
// hand-authored records that the builder cannot edit or remove.
func manifestVariants(root, manifest, manifestPath string,
	track assetTrack) []assetVariantStatus {
	var variants []assetVariantStatus
	for _, region := range splitRegions(track) {
		name := splitSectionName(track.ID, region.Slug)
		section := "music:" + name
		if _, exists := findManifestSection(manifest, section); !exists {
			continue
		}
		file, hasFile := manifestSectionValue(manifest, section, "file")
		if !hasFile {
			continue
		}
		path, reachable := manifestAudioTarget(root, manifestPath, file)
		if !reachable {
			continue
		}
		gate, _ := manifestSectionValue(manifest, section, "when")
		variant := assetVariantStatus{
			Name: name, When: gate, File: file,
			Configured: regularFileExists(path),
		}
		if variant.Configured {
			variant.URL = "asset-audio/" + name + "?v=" + previewVersionToken(path)
		}
		variants = append(variants, variant)
	}
	return variants
}

func loadAssetConfiguration(root string) (assetConfiguration, error) {
	manifest, manifestPath, err := readAssetManifest(root)
	if err != nil {
		return assetConfiguration{}, err
	}
	configuration := assetConfiguration{
		ManifestPath: filepath.ToSlash(filepath.Join("game-assets", "manifest.ini")),
	}
	logo, logoFound := manifestSectionValue(manifest, "replace:title-logo", "image")
	swirl, swirlFound := manifestSectionValue(manifest, "replace:title-swirl", "image")
	configuration.Title.File = logo
	configuration.Title.Enabled = logoFound && swirlFound &&
		regularFileExists(resolveManifestFile(manifestPath, logo)) &&
		regularFileExists(resolveManifestFile(manifestPath, swirl))

	for _, track := range assetTracks {
		file, found := manifestSectionValue(manifest, "music:"+track.ID, "file")
		configured := found && regularFileExists(resolveManifestFile(manifestPath, file))
		status := assetTrackStatus{
			ID: track.ID, Name: track.Name, Src: track.Src,
			Configured: configured, File: file,
		}
		if configured {
			status.URL = installedAudioURL(root, manifestPath, track.ID, file)
		}
		status.Variants = manifestVariants(root, manifest, manifestPath, track)
		for _, region := range splitRegions(track) {
			_, exists := findManifestSection(manifest,
				"music:"+splitSectionName(track.ID, region.Slug))
			status.Splits = append(status.Splits, assetSplitStatus{
				Slug: region.Slug, Label: region.Label, Acts: region.Acts, Enabled: exists,
				Name: splitSectionName(track.ID, region.Slug),
				Gate: splitGate(region.Group),
				File: splitStubFile(track.ID, region.Slug),
			})
		}
		configuration.Tracks = append(configuration.Tracks, status)
	}
	return configuration, nil
}

// serveInstalledAudio plays back a replacement that is already installed. The
// id is looked up in the manifest on every request rather than cached: the
// manifest can change under a long-lived builder session -- by hand or by a
// save in another tab -- and a stale map would serve the file that slot USED to
// name.
func (app *application) serveInstalledAudio(response http.ResponseWriter,
	request *http.Request, id string) {
	if !safeManifestTrackID.MatchString(id) || !managedTrackID(id) {
		http.NotFound(response, request)
		return
	}
	app.assetMu.Lock()
	manifest, manifestPath, err := readAssetManifest(app.options.ProjectRoot)
	app.assetMu.Unlock()
	if err != nil {
		http.NotFound(response, request)
		return
	}
	value, found := manifestSectionValue(manifest, "music:"+id, "file")
	if !found {
		http.NotFound(response, request)
		return
	}
	path, ok := installedAudioPath(app.options.ProjectRoot, manifestPath, value)
	if !ok {
		http.NotFound(response, request)
		return
	}
	file, err := os.Open(path)
	if err != nil {
		http.NotFound(response, request)
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		http.NotFound(response, request)
		return
	}
	response.Header().Set("Content-Type", "audio/ogg")
	response.Header().Set("Cache-Control", "no-store")
	response.Header().Set("X-Content-Type-Options", "nosniff")
	http.ServeContent(response, request, id+".ogg", info.ModTime(), file)
}

func (app *application) writeAssets(response http.ResponseWriter) {
	configuration, err := loadAssetConfiguration(app.options.ProjectRoot)
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(response, http.StatusOK, configuration)
}
