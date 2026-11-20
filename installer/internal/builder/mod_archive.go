package builder

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"fmt"
	"image/png"
	"io"
	"os"
	"path"
	"regexp"
	"strings"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

const (
	maxModBytes        = 1 << 30
	maxModMetadata     = 64 << 10
	maxModMembers      = 128
	musicModType       = "actraiser-music"
	titleModType       = "actraiser-title"
	titleModTarget     = "title-artwork"
	manualModType      = "actraiser-manual"
	manualModTarget    = "in-game-manual"
	languageModType    = lk.LanguageModType
	maxTitlePNGBytes   = 32 << 20
	titleArtworkWidth  = 2212
	titleArtworkHeight = 760
	maxTitlePixels     = 8 << 20
	modGameID          = "actraiser-recomp"
)

var (
	modIDPattern     = regexp.MustCompile(`^[a-z0-9][a-z0-9._-]{0,63}$`)
	modMemberPattern = regexp.MustCompile(`^[A-Za-z0-9_. /-]+$`)
)

type modMetadata struct {
	Name        string `json:"name"`
	Version     string `json:"version"`
	Author      string `json:"author"`
	Description string `json:"description,omitempty"`
	License     string `json:"license,omitempty"`
}

type modPayload struct {
	Format  int        `json:"format"`
	Type    string     `json:"type"`
	ID      string     `json:"id"`
	Game    string     `json:"game"`
	Tracks  []modTrack `json:"tracks,omitempty"`
	Artwork string     `json:"artwork,omitempty"`
	Manual  string     `json:"manual,omitempty"`
}

type modTrack struct {
	Track     string  `json:"track"`
	Region    string  `json:"region,omitempty"`
	File      string  `json:"file"`
	Loop      *bool   `json:"loop,omitempty"`
	LoopStart *uint32 `json:"loop_start,omitempty"`
	LoopEnd   *uint32 `json:"loop_end,omitempty"`
	Gain      *int    `json:"gain,omitempty"`
}

func (t modTrack) target() string {
	if t.Region == "" {
		return t.Track
	}
	return splitSectionName(t.Track, t.Region)
}

func (payload modPayload) targets() []string {
	switch payload.Type {
	case titleModType:
		return []string{titleModTarget}
	case manualModType:
		return []string{manualModTarget}
	}
	targets := make([]string, len(payload.Tracks))
	for i, track := range payload.Tracks {
		targets[i] = track.target()
	}
	return targets
}

func (payload modPayload) assetPaths() []string {
	switch payload.Type {
	case titleModType:
		return []string{payload.Artwork}
	case manualModType:
		return []string{payload.Manual}
	}
	paths := make([]string, len(payload.Tracks))
	for i, track := range payload.Tracks {
		paths[i] = track.File
	}
	return paths
}

type musicModTarget struct {
	Song   assetTrack
	Gate   string
	Region string
}

var musicModTargets = func() map[string]musicModTarget {
	targets := make(map[string]musicModTarget)
	for _, track := range assetTracks {
		targets[track.ID] = musicModTarget{Song: track}
		for _, region := range splitRegions(track) {
			targets[splitSectionName(track.ID, region.Slug)] = musicModTarget{
				Song: track, Gate: splitGate(region.Group), Region: region.Slug,
			}
		}
	}
	return targets
}()

type modArchive struct {
	modMetadata
	modPayload
	files map[string]*zip.File
}

func readModArchive(input io.ReaderAt, size int64) (modArchive, error) {
	var pack modArchive
	if size <= 0 || size > maxModBytes {
		return pack, fmt.Errorf("mod archive must be between 1 byte and %d MiB", maxModBytes>>20)
	}
	z, err := zip.NewReader(input, size)
	if err != nil {
		return pack, fmt.Errorf("open mod ZIP: %w", err)
	}
	descriptors := map[string]*zip.File{}
	for _, entry := range z.File {
		if entry.Name == "manifest.json" || entry.Name == "pack.json" {
			if descriptors[entry.Name] != nil || !entry.Mode().IsRegular() || entry.Flags&1 != 0 {
				return pack, fmt.Errorf("invalid or duplicate archive descriptor %q", entry.Name)
			}
			descriptors[entry.Name] = entry
		}
	}
	var manifest struct {
		modMetadata
		Main     string            `json:"main"`
		Binaries map[string]string `json:"binaries"`
	}
	if err := readModJSON(descriptors["manifest.json"], &manifest); err != nil {
		return pack, fmt.Errorf("manifest.json: %w", err)
	}
	pack.modMetadata = manifest.modMetadata
	if err := validateModMetadata(pack.modMetadata); err != nil {
		return pack, err
	}
	if manifest.Main != "" || len(manifest.Binaries) != 0 {
		return pack, fmt.Errorf("ActRaiser packs contain assets only")
	}
	if err := readModJSON(descriptors["pack.json"], &pack.modPayload); err != nil {
		return pack, fmt.Errorf("pack.json: %w", err)
	}
	if err := pack.modPayload.validate(); err != nil {
		return pack, err
	}
	if pack.Type == languageModType {
		if size > lk.MaxAuthorArchiveBytes || len(z.File) > lk.MaxLanguageModMembers {
			return pack, fmt.Errorf("language mod exceeds archive limits")
		}
		_, err := lk.ParseLanguageRelease(pack.Version)
		return pack, err
	}
	if len(z.File) > maxModMembers {
		return pack, fmt.Errorf("too many files in mod archive")
	}
	names := make(map[string]bool, len(z.File))
	pack.files = make(map[string]*zip.File, len(z.File))
	var total uint64
	for _, entry := range z.File {
		name := strings.TrimSuffix(entry.Name, "/")
		key := strings.ToLower(name)
		if !modMemberPattern.MatchString(name) || !validModMember(name) || names[key] {
			return pack, fmt.Errorf("invalid or duplicate archive path %q", entry.Name)
		}
		names[key] = true
		if entry.Mode().IsDir() {
			continue
		}
		if !entry.Mode().IsRegular() || entry.Flags&1 != 0 ||
			entry.UncompressedSize64 > maxTrackBytes || entry.UncompressedSize64 > maxModBytes-total {
			return pack, fmt.Errorf("invalid or oversized archive member %q", entry.Name)
		}
		total += entry.UncompressedSize64
		pack.files[name] = entry
	}
	return pack, pack.validateAssetMembers()
}

func validModMember(name string) bool {
	if strings.HasPrefix(name, "/") || path.Clean(name) != name || name == "." ||
		name == ".." || strings.HasPrefix(name, "../") {
		return false
	}
	for part := range strings.SplitSeq(name, "/") {
		if strings.TrimRight(part, " .") != part || strings.HasPrefix(part, ".") {
			return false
		}
	}
	return true
}

func readModJSON(file *zip.File, value any) error {
	if file == nil {
		return fmt.Errorf("missing root metadata file")
	}
	if file.UncompressedSize64 > maxModMetadata {
		return fmt.Errorf("metadata file is too large")
	}
	r, err := file.Open()
	if err != nil {
		return err
	}
	defer r.Close()
	data, err := io.ReadAll(io.LimitReader(r, maxModMetadata+1))
	if err != nil {
		return err
	}
	if len(data) > maxModMetadata {
		return fmt.Errorf("metadata file is too large")
	}
	return json.Unmarshal(data, value)
}

func validateModMetadata(metadata modMetadata) error {
	if strings.TrimSpace(metadata.Name) == "" || len(metadata.Name) > 160 ||
		len(metadata.Version) > 64 ||
		len(metadata.Author) > 256 || len(metadata.License) > 256 ||
		len(metadata.Description) > 4096 {
		return fmt.Errorf("manifest.json needs a name and bounded metadata")
	}
	return nil
}

func (payload modPayload) validate() error {
	if payload.Format != 1 || payload.Game != modGameID || !modIDPattern.MatchString(payload.ID) {
		return fmt.Errorf("invalid ActRaiser pack format, game, or ID")
	}
	switch payload.Type {
	case musicModType:
		return payload.validateMusic()
	case titleModType:
		if len(payload.Tracks) != 0 || payload.Manual != "" || !validModAsset(payload.Artwork, ".png") {
			return fmt.Errorf("a title artwork pack needs one PNG assigned as artwork")
		}
	case manualModType:
		if len(payload.Tracks) != 0 || payload.Artwork != "" || !validModAsset(payload.Manual, ".pdf") {
			return fmt.Errorf("a manual pack needs one PDF assigned as manual")
		}
	case languageModType:
		if len(payload.Tracks) != 0 || payload.Artwork != "" || payload.Manual != "" {
			return fmt.Errorf("unexpected asset assignment in language mod descriptor")
		}
	default:
		return fmt.Errorf("unsupported mod type %q", payload.Type)
	}
	return nil
}

func validModAsset(name, ext string) bool {
	return modMemberPattern.MatchString(name) && validModMember(name) && strings.EqualFold(path.Ext(name), ext)
}

func (payload modPayload) validateMusic() error {
	if payload.Artwork != "" || payload.Manual != "" || len(payload.Tracks) == 0 || len(payload.Tracks) > len(musicModTargets) {
		return fmt.Errorf("unexpected number of music assignments")
	}
	targets := make(map[string]bool, len(payload.Tracks))
	for _, track := range payload.Tracks {
		target := track.target()
		spec, ok := musicModTargets[target]
		if !ok || spec.Song.ID != track.Track || spec.Region != track.Region {
			return fmt.Errorf("unknown music target %q", target)
		}
		if targets[target] {
			return fmt.Errorf("duplicate music target %q", target)
		}
		targets[target] = true
		if !validModAsset(track.File, ".ogg") {
			return fmt.Errorf("%s needs an Ogg Vorbis file in the archive", target)
		}
		if track.Gain != nil && (*track.Gain < 0 || *track.Gain > 400) {
			return fmt.Errorf("%s gain must be between 0 and 400", target)
		}
		if track.LoopStart != nil && track.LoopEnd != nil && *track.LoopEnd != 0 && *track.LoopEnd <= *track.LoopStart {
			return fmt.Errorf("%s loop end must follow the loop start", target)
		}
	}
	return nil
}

func (pack modArchive) validateAssetMembers() error {
	used := map[string]bool{
		"manifest.json": true, "pack.json": true, "README.md": true,
		"LICENSE": true, "LICENSE.txt": true, "LICENSE.md": true, "icon.png": true,
	}
	for _, name := range pack.assetPaths() {
		entry := pack.files[name]
		if entry == nil || entry.UncompressedSize64 == 0 {
			return fmt.Errorf("missing or empty asset %q in %s pack", name, pack.Type)
		}
		if pack.Type == titleModType && entry.UncompressedSize64 > maxTitlePNGBytes {
			return fmt.Errorf("title artwork exceeds %d MiB", maxTitlePNGBytes>>20)
		}
		if pack.Type == manualModType && entry.UncompressedSize64 > maxManualBytes {
			return fmt.Errorf("the manual exceeds the %d MiB safety limit", maxManualBytes>>20)
		}
		used[name] = true
	}
	for name := range pack.files {
		if !used[name] {
			return fmt.Errorf("unexpected file %q in %s pack", name, pack.Type)
		}
	}
	return nil
}

func copyModTitle(out io.Writer, source io.Reader) error {
	return writeModTitle(out, source, false)
}

func writeModTitle(out io.Writer, source io.Reader, authoring bool) error {
	data, err := io.ReadAll(io.LimitReader(source, maxTitlePNGBytes+1))
	if err != nil {
		return err
	}
	if len(data) > maxTitlePNGBytes {
		return fmt.Errorf("title artwork exceeds %d MiB", maxTitlePNGBytes>>20)
	}
	info, err := png.DecodeConfig(bytes.NewReader(data))
	if err != nil {
		return fmt.Errorf("read title PNG: %w", err)
	}
	if int64(info.Width)*int64(info.Height) > maxTitlePixels {
		return fmt.Errorf("title artwork exceeds %d pixels", maxTitlePixels)
	}
	if authoring && (info.Width != titleArtworkWidth || info.Height != titleArtworkHeight) {
		return fmt.Errorf("title artwork must use a %d × %d pixel canvas; the selected PNG is %d × %d", titleArtworkWidth, titleArtworkHeight, info.Width, info.Height)
	}
	image, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		return fmt.Errorf("decode title PNG: %w", err)
	}
	if authoring {
		// Ignore nearly transparent pixels so stray editing artifacts do not
		// make an otherwise empty logo pass.
		visible := 0
		minimum := titleArtworkWidth * titleArtworkHeight / 100
		for y := 0; y < info.Height && visible < minimum; y++ {
			for x := range info.Width {
				if _, _, _, alpha := image.At(x, y).RGBA(); alpha >= 0x0ccc {
					visible++
				}
			}
		}
		if visible < minimum {
			return fmt.Errorf("title artwork must cover at least 1%% of the canvas with visible pixels")
		}
	}
	_, err = out.Write(data)
	return err
}

func readModVorbisHeader(source io.Reader) ([]byte, error) {
	var page [27 + 255 + 30]byte
	header := page[:27]
	if _, err := io.ReadFull(source, header); err != nil {
		return nil, fmt.Errorf("read Ogg page: %w", err)
	}
	segments := int(header[26])
	if string(header[:4]) != "OggS" || header[4] != 0 || header[5]&3 != 2 || segments == 0 {
		return nil, fmt.Errorf("expected an Ogg Vorbis identification page")
	}
	lacing := page[len(header) : len(header)+segments]
	if _, err := io.ReadFull(source, lacing); err != nil {
		return nil, err
	}
	end := len(header) + segments + 30
	packet := page[end-30 : end]
	if lacing[0] != byte(len(packet)) {
		return nil, fmt.Errorf("expected a Vorbis identification packet")
	}
	if _, err := io.ReadFull(source, packet); err != nil {
		return nil, err
	}
	if !bytes.Equal(packet[:7], []byte("\x01vorbis")) || !bytes.Equal(packet[7:11], []byte{0, 0, 0, 0}) ||
		packet[11] == 0 || bytes.Equal(packet[12:16], []byte{0, 0, 0, 0}) || packet[29]&1 == 0 {
		return nil, fmt.Errorf("invalid Vorbis identification packet")
	}
	return page[:end], nil
}

// Stream the member to EOF so the ZIP reader verifies its length and CRC.
// The game's decoder probes playback at startup.
func copyModAudio(out io.Writer, source io.Reader) error {
	header, err := readModVorbisHeader(source)
	if err != nil {
		return err
	}
	if _, err := out.Write(header); err != nil {
		return err
	}
	_, err = io.Copy(out, source)
	return err
}

func openModFile(name string) (*os.File, error) {
	info, err := os.Lstat(name)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() <= 0 || info.Size() > maxModBytes {
		return nil, fmt.Errorf("invalid or oversized mod archive")
	}
	file, err := os.Open(name)
	if err != nil {
		return nil, err
	}
	opened, err := file.Stat()
	if err != nil || !os.SameFile(info, opened) {
		file.Close()
		return nil, fmt.Errorf("mod archive changed while opening")
	}
	return file, nil
}
