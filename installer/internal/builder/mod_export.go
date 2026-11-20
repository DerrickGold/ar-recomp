package builder

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
)

type modExportRequest struct {
	modMetadata
	ID      string   `json:"id"`
	Targets []string `json:"targets"`
}

type modExportTrack struct {
	ID         string `json:"id"`
	Track      string `json:"track,omitempty"`
	Region     string `json:"region,omitempty"`
	Error      string `json:"error,omitempty"`
	path       string
	assignment modTrack
}

func modExportTracks(root string) ([]modExportTrack, error) {
	manifest, manifestPath, err := readAssetManifest(root)
	if err != nil {
		return nil, err
	}
	var rows = []modExportTrack{}
	for _, section := range manifestSections(manifest) {
		id, music := strings.CutPrefix(section.Name, "music:")
		if !music {
			continue
		}
		values := manifestSectionValues(manifest[section.Start:section.End])
		file, ok := installedAudioPath(root, manifestPath, values["file"])
		if !ok {
			continue
		}
		row := modExportTrack{ID: id}
		spec, supported := musicModTargets[id]
		if !supported || !strings.EqualFold(values["src"], spec.Song.Src) ||
			withoutModWhitespace(values["when"]) != withoutModWhitespace(spec.Gate) || values["song"] != "" {
			row.Error = "This song or condition cannot be represented as a music pack assignment."
		} else {
			row.Track, row.Region = spec.Song.ID, spec.Region
			row.path, err = filepath.Rel(filepath.Join(root, "game-assets"), file)
			if err != nil {
				return nil, err
			}
			row.assignment, err = exportedModTrack(id, spec, values)
			if err != nil {
				row.Error = err.Error()
			}
		}
		rows = append(rows, row)
	}
	return rows, nil
}

func manifestSectionValues(block string) map[string]string {
	values := map[string]string{}
	for line := range strings.SplitSeq(block, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}
		if key, value, ok := strings.Cut(line, "="); ok {
			values[strings.TrimSpace(key)] = strings.TrimSpace(value)
		}
	}
	return values
}

func withoutModWhitespace(raw string) string {
	return strings.ToLower(strings.Join(strings.Fields(raw), ""))
}

func exportedModTrack(id string, spec musicModTarget, values map[string]string) (modTrack, error) {
	result := modTrack{Track: spec.Song.ID, Region: spec.Region, File: "audio/" + id + ".ogg"}
	loop := true
	if raw := values["loop"]; raw != "" {
		number, err := strconv.ParseUint(raw, 0, 64)
		if err != nil {
			return result, fmt.Errorf("invalid loop setting")
		}
		loop = number != 0
	}
	result.Loop = &loop
	for key, dest := range map[string]**uint32{"loop_start": &result.LoopStart, "loop_end": &result.LoopEnd} {
		if raw := values[key]; raw != "" {
			number, err := strconv.ParseUint(raw, 0, 32)
			if err != nil {
				return result, fmt.Errorf("invalid %s setting", key)
			}
			frames := uint32(number)
			*dest = &frames
		}
	}
	if raw := values["gain"]; raw != "" {
		gain, err := strconv.ParseInt(raw, 0, 32)
		if err != nil || gain < 0 || gain > 400 {
			return result, fmt.Errorf("gain must be between 0 and 400")
		}
		percent := int(gain)
		result.Gain = &percent
	}
	return result, nil
}

func exportMusicMod(root string, out io.Writer, request modExportRequest) error {
	if err := validateModMetadata(request.modMetadata); err != nil {
		return err
	}
	if (request.ID != "" && !modIDPattern.MatchString(request.ID)) || len(request.Targets) == 0 || len(request.Targets) > len(musicModTargets) {
		return fmt.Errorf("select at least one valid music replacement")
	}
	tracks, err := modExportTracks(root)
	if err != nil {
		return err
	}
	available := make(map[string]modExportTrack)
	for _, row := range tracks {
		if _, exists := available[row.ID]; exists {
			row.Error = "Duplicate manifest section"
		}
		available[row.ID] = row
	}
	assets, err := os.OpenRoot(filepath.Join(root, "game-assets"))
	if err != nil {
		return err
	}
	defer assets.Close()
	archive := zip.NewWriter(out)
	payload := modPayload{Format: 1, Type: musicModType, Game: modGameID, ID: request.ID}
	used := map[string]bool{}
	total := int64(0)
	targets := slices.Clone(request.Targets)
	slices.Sort(targets)
	digests := make([]string, 0, len(targets))
	for _, id := range targets {
		row, ok := available[id]
		if !ok || row.Error != "" || used[id] {
			return fmt.Errorf("%q is not an exportable replacement", id)
		}
		used[id] = true
		digest := sha256.New()
		size, err := exportModAudio(assets, archive, row, digest, int64(maxModBytes-maxModMetadata*2)-total)
		if err != nil {
			return fmt.Errorf("%s: %w", id, err)
		}
		total += size
		digests = append(digests, fmt.Sprintf("%x", digest.Sum(nil)))
		payload.Tracks = append(payload.Tracks, row.assignment)
	}
	return finishModArchive(archive, request.modMetadata, payload, digests)
}

func exportTitleMod(out io.Writer, metadata modMetadata, id string, source io.Reader) error {
	const name = "art/title.png"
	payload := modPayload{Format: 1, Type: titleModType, Game: modGameID, ID: id, Artwork: name}
	return exportFileMod(out, metadata, payload, name, source, func(out io.Writer, input io.Reader) error {
		if id == "" {
			return writeModTitle(out, input, true)
		}
		return copyModTitle(out, input)
	})
}

func exportManualMod(out io.Writer, metadata modMetadata, id string, source io.Reader) error {
	const name = "manual/manual.pdf"
	payload := modPayload{Format: 1, Type: manualModType, Game: modGameID, ID: id, Manual: name}
	return exportFileMod(out, metadata, payload, name, source, func(out io.Writer, input io.Reader) error {
		_, err := copyModManual(out, input)
		return err
	})
}

func exportFileMod(out io.Writer, metadata modMetadata, payload modPayload, name string, source io.Reader, copyAsset func(io.Writer, io.Reader) error) error {
	archive := zip.NewWriter(out)
	member, err := archive.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Store})
	if err != nil {
		return err
	}
	digest := sha256.New()
	if err := copyAsset(io.MultiWriter(member, digest), source); err != nil {
		return err
	}
	return finishModArchive(archive, metadata, payload, []string{fmt.Sprintf("%x", digest.Sum(nil))})
}

func finishModArchive(archive *zip.Writer, metadata modMetadata, payload modPayload, digests []string) error {
	if payload.ID == "" {
		identity, err := json.Marshal([]any{payload, metadata, digests})
		if err != nil {
			return err
		}
		digest := sha256.Sum256(identity)
		payload.ID = strings.TrimPrefix(payload.Type, "actraiser-") + "-" + fmt.Sprintf("%x", digest[:20])
	}

	for _, document := range []struct {
		name string
		data any
	}{
		{name: "manifest.json", data: metadata},
		{name: "pack.json", data: payload},
	} {
		member, err := archive.Create(document.name)
		if err != nil {
			return err
		}
		if err := json.NewEncoder(member).Encode(document.data); err != nil {
			return err
		}
	}
	return archive.Close()
}

func exportModAudio(assets *os.Root, archive *zip.Writer, row modExportTrack, digest io.Writer, remaining int64) (int64, error) {
	info, err := assets.Stat(row.path)
	if err != nil {
		return 0, err
	}
	if !info.Mode().IsRegular() || info.Size() <= 0 || info.Size() > int64(maxTrackBytes) || info.Size() > remaining {
		return 0, fmt.Errorf("invalid or oversized audio file")
	}
	source, err := assets.Open(row.path)
	if err != nil {
		return 0, err
	}
	defer source.Close()
	actual, err := source.Stat()
	if err != nil || !os.SameFile(info, actual) {
		return 0, fmt.Errorf("audio changed while opening")
	}
	member, err := archive.CreateHeader(&zip.FileHeader{Name: row.assignment.File, Method: zip.Store})
	if err != nil {
		return 0, err
	}
	limited := &io.LimitedReader{R: source, N: info.Size() + 1}
	if err := copyModAudio(io.MultiWriter(member, digest), limited); err != nil {
		return 0, err
	}
	if limited.N != 1 {
		return 0, fmt.Errorf("audio changed while exporting")
	}
	return info.Size(), nil
}
