package buildgui

import (
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/DerrickGold/snesrecomp-go/internal/spcaudio"
)

const audioPreviewDuration = 30 * time.Second

type audioPreviewTrackStatus struct {
	ID    string `json:"id"`
	Ready bool   `json:"ready"`
	URL   string `json:"url,omitempty"`
}

type audioPreviewStatus struct {
	State        string                    `json:"state"`
	ROMAvailable bool                      `json:"romAvailable"`
	Completed    int                       `json:"completed"`
	Total        int                       `json:"total"`
	Current      string                    `json:"current,omitempty"`
	Message      string                    `json:"message,omitempty"`
	Error        string                    `json:"error,omitempty"`
	Tracks       []audioPreviewTrackStatus `json:"tracks"`
	Fingerprint  string                    `json:"-"`
}

func regularPath(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

// previewVersionToken identifies the exact FILE behind a preview URL, not just
// the ROM it came from. The URL previously varied only with the ROM
// fingerprint, so a regenerated preview kept the URL its predecessor had -- and
// a browser holding cached byte ranges for that URL (Safari range-requests
// <audio> aggressively) could serve the old head of the file and range-fetch
// the new tail, splicing two different renders into one track. That reads as
// audio that is corrupt for the first seconds and then clears, baked in
// identically on every replay, on the machine that had the stale cache and not
// on a freshly installed one. Keying the URL to size and modification time
// means new bytes are always a new URL.
func previewVersionToken(path string) string {
	info, err := os.Stat(path)
	if err != nil {
		return "0"
	}
	return strconv.FormatInt(info.Size(), 36) + "-" +
		strconv.FormatInt(info.ModTime().UnixNano(), 36)
}

func findAudioPreviewROM(root string) string {
	candidates := []string{
		filepath.Join(root, "user-rom.sfc"),
		filepath.Join(root, "game.sfc"),
	}
	// A packaged builder runs from <bundle>/utils while the installed ROM may
	// live beside the game executable one level up.
	parent := filepath.Dir(root)
	if parent != root {
		candidates = append(candidates,
			filepath.Join(parent, "user-rom.sfc"),
			filepath.Join(parent, "game.sfc"))
	}
	for _, candidate := range candidates {
		if regularPath(candidate) {
			return candidate
		}
	}
	return ""
}

func audioPreviewCacheRoot(options Options) (string, error) {
	if options.AudioPreviewCacheDir != "" {
		return filepath.Abs(options.AudioPreviewCacheDir)
	}
	directory, err := os.UserCacheDir()
	if err != nil {
		return "", fmt.Errorf("locate the system cache directory: %w", err)
	}
	return filepath.Join(directory, "ActRaiserRecomp", "audio-previews"), nil
}

func audioPreviewTracks() []spcaudio.Track {
	tracks := make([]spcaudio.Track, 0, len(assetTracks))
	for _, track := range assetTracks {
		tracks = append(tracks, spcaudio.Track{
			ID: track.ID, Name: track.Name, Source: track.PreviewSource,
			Song: track.PreviewSong,
		})
	}
	return tracks
}

func (app *application) currentAudioPreviewStatus() audioPreviewStatus {
	app.previewMu.Lock()
	status := app.preview
	paths := make(map[string]string, len(app.previewPaths))
	for id, path := range app.previewPaths {
		paths[id] = path
	}
	app.previewMu.Unlock()
	status.ROMAvailable = findAudioPreviewROM(app.options.ProjectRoot) != ""
	status.Tracks = make([]audioPreviewTrackStatus, 0, len(assetTracks))
	for _, track := range assetTracks {
		path, ready := paths[track.ID]
		ready = ready && regularPath(path)
		item := audioPreviewTrackStatus{ID: track.ID, Ready: ready}
		if ready {
			item.URL = "audio-preview/" + track.ID + ".wav?v=" + previewVersionToken(path)
		}
		status.Tracks = append(status.Tracks, item)
	}
	return status
}

func (app *application) writeAudioPreviewStatus(response http.ResponseWriter) {
	writeJSON(response, http.StatusOK, app.currentAudioPreviewStatus())
}

func (app *application) startAudioPreviews(response http.ResponseWriter,
	request *http.Request) {
	romPath := findAudioPreviewROM(app.options.ProjectRoot)
	if romPath == "" {
		writeJSONError(response, http.StatusConflict,
			"select a ROM on the Build tab before generating original-audio previews")
		return
	}
	cacheRoot, err := audioPreviewCacheRoot(app.options)
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	// Discarding the cache is the user's escape hatch for a preview that sounds
	// wrong: the renderer is deterministic, so re-rendering only helps once the
	// suspect file is actually gone.
	if request.URL.Query().Get("force") == "1" {
		fingerprint, fingerprintErr := spcaudio.ROMFingerprint(romPath)
		if fingerprintErr != nil {
			writeJSONError(response, http.StatusBadRequest, fingerprintErr.Error())
			return
		}
		stale := spcaudio.PreviewCacheDirectory(cacheRoot, fingerprint, audioPreviewDuration)
		if removeErr := os.RemoveAll(stale); removeErr != nil {
			writeJSONError(response, http.StatusInternalServerError,
				"discard the cached previews: "+removeErr.Error())
			return
		}
	}
	app.previewMu.Lock()
	if app.preview.State == "generating" {
		app.previewMu.Unlock()
		writeJSONError(response, http.StatusConflict, "audio previews are already being generated")
		return
	}
	app.preview = audioPreviewStatus{
		State: "generating", Total: len(assetTracks),
		Message: "Preparing the original SNES audio engine…",
	}
	app.previewPaths = make(map[string]string)
	app.previewMu.Unlock()
	writeJSON(response, http.StatusAccepted, app.currentAudioPreviewStatus())

	go func() {
		paths, fingerprint, renderErr := spcaudio.RenderActRaiserPreviews(
			app.ctx, romPath, cacheRoot, audioPreviewTracks(), audioPreviewDuration,
			func(progress spcaudio.Progress) {
				app.previewMu.Lock()
				app.preview.Completed = progress.Completed
				app.preview.Current = progress.Track.Name
				app.preview.Message = fmt.Sprintf("Rendered %d of %d tracks",
					progress.Completed, progress.Total)
				app.previewMu.Unlock()
			},
		)
		app.previewMu.Lock()
		defer app.previewMu.Unlock()
		if renderErr != nil {
			app.preview.State = "failed"
			app.preview.Error = renderErr.Error()
			app.preview.Message = "Original-audio preview generation failed."
			return
		}
		app.preview.State = "ready"
		app.preview.Completed = len(assetTracks)
		app.preview.Current = ""
		app.preview.Message = "Original ROM previews are ready for A/B playback."
		app.preview.Fingerprint = fingerprint
		app.previewPaths = paths
	}()
}

func (app *application) serveAudioPreview(response http.ResponseWriter,
	request *http.Request, leaf string) {
	if !strings.HasSuffix(leaf, ".wav") || strings.ContainsAny(leaf, `/\\`) {
		http.NotFound(response, request)
		return
	}
	id := strings.TrimSuffix(leaf, ".wav")
	app.previewMu.Lock()
	path, found := app.previewPaths[id]
	app.previewMu.Unlock()
	if !found {
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
	response.Header().Set("Content-Type", "audio/wav")
	// no-store, not max-age: these are local files that cost nothing to re-read,
	// and a browser-cached copy of a preview that was later regenerated is
	// exactly the corruption previewVersionToken exists to prevent. The token
	// alone would do it; this makes a stale copy unreachable either way.
	response.Header().Set("Cache-Control", "no-store")
	response.Header().Set("X-Content-Type-Options", "nosniff")
	http.ServeContent(response, request, id+".wav", info.ModTime(), file)
}
