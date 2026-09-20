// Package textpreview runs the shipped game's ROM-free playback worker. The
// browser plays rendered images; only the game engine shapes and reveals text.
package textpreview

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
	"github.com/DerrickGold/ar-recomp/installer/internal/subprocess"
)

const maxResultBytes = 48 << 20

type Scenario struct {
	Page       uint32            `json:"page"`
	Delay      uint32            `json:"delay"`
	Size       uint32            `json:"size"`
	Sampling   uint32            `json:"sampling"`
	Pixelation uint32            `json:"pixelation"`
	PixelSize  uint32            `json:"pixelSize"`
	Values     map[string]string `json:"values"`
	Inks       map[string]string `json:"inks"`
}

type Frame struct {
	Tick     uint32 `json:"tick"`
	Sheet    uint32 `json:"sheet"`
	Index    uint32 `json:"index"`
	Revealed uint32 `json:"revealed"`
	Kind     string `json:"kind"`
	Control  string `json:"control,omitempty"`
}
type FontUse struct {
	Start     uint32 `json:"start"`
	End       uint32 `json:"end"`
	Pixels    uint32 `json:"pixels"`
	Missing   bool   `json:"missing"`
	Reference string `json:"reference"`
}
type Movie struct {
	PackID     string                  `json:"packID"`
	Fallback   bool                    `json:"fallback"`
	Appearance lk.AuthorTextAppearance `json:"appearance"`
	Treatments []lk.AuthorTreatment    `json:"treatments"`

	Version             int       `json:"version"`
	Width               int       `json:"width"`
	Height              int       `json:"height"`
	Page                int       `json:"page"`
	Pages               int       `json:"pages"`
	NativeBounds        bool      `json:"nativeBounds"`
	ArtworkPlaceholders bool      `json:"artworkPlaceholders"`
	Source              string    `json:"source"`
	MessageID           string    `json:"messageID"`
	ResolvedID          string    `json:"resolvedID"`
	Line                int       `json:"line"`
	Layout              string    `json:"layout"`
	Frames              []Frame   `json:"frames"`
	Fonts               []FontUse `json:"fonts"`
	Duration            uint32    `json:"duration"`
	SheetCount          uint32    `json:"sheetCount"`
	Sheets              []string  `json:"sheets"`
}

func Defaults() Scenario {
	return Scenario{Delay: 3, Size: 140, Pixelation: 2, PixelSize: 2, Values: map[string]string{}, Inks: map[string]string{}}
}

// Samples are visible scenario inputs, never claims about the current game.
func ValueSample(value lk.AuthorPlaceholder) string {
	switch value.Kind {
	case "number":
		return "123"
	case "localized_term":
		return "growth_state.00"
	case "icon":
		return value.Name
	}
	switch value.Name {
	case "master_name":
		return "Player"
	case "current_city_name", "town_name", "location_name":
		return "Fillmore"
	case "enemy_name":
		return "Goblin"
	case "hud_value":
		return "123/456"
	}
	return "Sample"
}

func encodeScenario(s Scenario) ([]byte, error) {
	if s.Page >= 64 || s.Delay > 9 || s.Size < 80 || s.Size > 140 || s.Size%5 != 0 || s.Sampling > 1 || s.Pixelation > 2 ||
		(s.Pixelation == 0 && s.PixelSize != 0) || (s.Pixelation != 0 && s.PixelSize != 2 && s.PixelSize != 4 && s.PixelSize != 6 && s.PixelSize != 8) || len(s.Values) > 32 {
		return nil, fmt.Errorf("invalid playback page, timing or text settings")
	}
	var out bytes.Buffer
	out.WriteString("ARTPREV1")
	write := func(n uint32) { _ = binary.Write(&out, binary.LittleEndian, n) }
	for _, n := range []uint32{s.Page, s.Delay, s.Size, s.Sampling, s.Pixelation, s.PixelSize, uint32(len(s.Values))} {
		write(n)
	}
	keys := make([]string, 0, len(s.Values))
	for key := range s.Values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		for i, value := range []string{key, s.Values[key]} {
			limit := 1023
			if i == 0 {
				limit = 127
			}
			if len(value) > limit || strings.ContainsRune(value, 0) || !utf8.ValidString(value) {
				return nil, fmt.Errorf("invalid scenario value %q", key)
			}
			write(uint32(len(value)))
			out.WriteString(value)
		}
	}
	allowed := map[string]bool{}
	for _, ink := range lk.AuthorNativeInks() {
		allowed[ink.Name] = true
		value := defaultInk(ink.Name)
		if override, ok := s.Inks[ink.Name]; ok {
			n, err := strconv.ParseUint(strings.TrimPrefix(override, "#"), 16, 24)
			if err != nil || len(override) != 7 || override[0] != '#' {
				return nil, fmt.Errorf("invalid preview color for %s", ink.Name)
			}
			value = uint32(n)
		}
		write(value)
	}
	for key := range s.Inks {
		if !allowed[key] {
			return nil, fmt.Errorf("unknown preview ink %q", key)
		}
	}
	return out.Bytes(), nil
}

func defaultInk(name string) uint32 {
	if strings.HasSuffix(name, "shadow") {
		return 0x101828
	}
	if strings.Contains(name, "hud") {
		if strings.HasSuffix(name, "band") {
			return 0xf8a038
		}
		return 0xffe098
	}
	if strings.HasSuffix(name, "accent") {
		return 0xffa050
	}
	if strings.HasSuffix(name, "band") {
		return 0xc8c8e8
	}
	return 0xffffff
}

type limitedOutput struct {
	bytes.Buffer
	remaining int
}

func (w *limitedOutput) Write(p []byte) (int, error) {
	if len(p) > w.remaining {
		return 0, fmt.Errorf("preview worker output exceeds its limit")
	}
	w.remaining -= len(p)
	return w.Buffer.Write(p)
}

func snapshot(root string, p *lk.AuthorPack) (string, error) {
	if p == nil || p.Manifest().Version() != 2 {
		return "", fmt.Errorf("upgrade the project and source reference to v2 before playback")
	}
	for name, data := range p.Files() {
		if !lk.PortablePackPath(name) {
			return "", fmt.Errorf("invalid pack member %q", name)
		}
		path := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
			return "", err
		}
		if err := os.WriteFile(path, data, 0600); err != nil {
			return "", err
		}
	}
	return filepath.Join(root, "pack.ini"), nil
}

// Run never loads a ROM, installs a pack or executes an imported program. The
// application chooses binary; all other files are immutable private snapshots.
func Run(ctx context.Context, binaryPath, builtin, id string, pack, fallback *lk.AuthorPack, s Scenario) (Movie, error) {
	var movie Movie
	if !filepath.IsAbs(binaryPath) || !filepath.IsAbs(builtin) || len(id) > 255 {
		return movie, fmt.Errorf("invalid playback host")
	}
	input, err := encodeScenario(s)
	if err != nil {
		return movie, err
	}
	ctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	root, err := os.MkdirTemp("", "actraiser-text-preview-")
	if err != nil {
		return movie, err
	}
	defer os.RemoveAll(root) // Exact private directory created for this request.
	manifest, err := snapshot(filepath.Join(root, "draft"), pack)
	if err != nil {
		return movie, err
	}
	native, err := snapshot(filepath.Join(root, "fallback"), fallback)
	if err != nil {
		return movie, err
	}
	output := filepath.Join(root, "frames")
	if err = os.Mkdir(output, 0700); err != nil {
		return movie, err
	}
	command := exec.CommandContext(ctx, binaryPath, "--text-preview-v1", manifest, native, builtin, id, output)
	subprocess.Configure(command)
	command.Dir = root
	command.Stdin = bytes.NewReader(input)
	stdout, stderr := &limitedOutput{remaining: 16384}, &limitedOutput{remaining: 65536}
	command.Stdout, command.Stderr = stdout, stderr
	command.WaitDelay = 2 * time.Second
	if err = command.Run(); err != nil {
		if ctx.Err() != nil {
			return movie, fmt.Errorf("playback cancelled or timed out: %w", ctx.Err())
		}
		return movie, fmt.Errorf("game playback failed: %s: %w", strings.TrimSpace(stderr.String()), err)
	}
	data, err := readLimited(filepath.Join(output, "report.json"), 4<<20)
	if err != nil {
		return movie, err
	}
	if err = json.Unmarshal(data, &movie); err != nil {
		return movie, fmt.Errorf("invalid playback report: %w", err)
	}
	if movie.Version != 1 || movie.Width <= 0 || movie.Width > 768 || movie.Height <= 0 || movie.Height > 768 || len(movie.Frames) == 0 || len(movie.Frames) > 2048 || movie.SheetCount > 64 || movie.SheetCount == 0 {
		return movie, fmt.Errorf("incompatible playback report")
	}
	for _, frame := range movie.Frames {
		if frame.Sheet >= movie.SheetCount || frame.Index >= 32 {
			return movie, fmt.Errorf("invalid playback frame")
		}
	}
	remaining := maxResultBytes
	for i := uint32(0); i < movie.SheetCount; i++ {
		data, err := readLimited(filepath.Join(output, fmt.Sprintf("sheet-%d.png", i)), remaining)
		if err != nil {
			return movie, err
		}
		remaining -= len(data)
		if len(data) < 8 || !bytes.Equal(data[:8], []byte{137, 80, 78, 71, 13, 10, 26, 10}) {
			return movie, fmt.Errorf("invalid playback image")
		}
		movie.Sheets = append(movie.Sheets, "data:image/png;base64,"+base64.StdEncoding.EncodeToString(data))
	}
	// The worker reports the actual selected template. Attach the immutable
	// author definitions, so a translator can explain the resulting appearance.
	effective := pack
	movie.Fallback = strings.HasPrefix(movie.Source, filepath.Join(root, "fallback")+string(filepath.Separator))
	if movie.Fallback {
		effective = fallback
	}
	movie.PackID = effective.Manifest().Metadata().ID
	message, err := effective.ResolvedMessage(id)
	if err != nil {
		return movie, err
	}
	movie.Appearance = message.Appearance
	used := map[string]bool{message.Appearance.Style.Treatment: true}
	for _, op := range message.Operations {
		used[op.Style.Treatment] = true
	}
	for _, treatment := range effective.Treatments() {
		if used[treatment.Definition.Name] {
			movie.Treatments = append(movie.Treatments, treatment)
		}
	}
	movie.Source = strings.TrimPrefix(movie.Source, filepath.Join(root, "draft")+string(filepath.Separator))
	movie.Source = strings.TrimPrefix(movie.Source, filepath.Join(root, "fallback")+string(filepath.Separator))
	return movie, nil
}
func readLimited(path string, maximum int) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, int64(maximum)+1))
	if err == nil && len(data) > maximum {
		err = fmt.Errorf("playback output exceeds its limit")
	}
	return data, err
}
