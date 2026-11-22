// Package modconfig reads the activation metadata embedded in the game's mod
// overlay. The Builder generates the runtime sections from validated archives.
package modconfig

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
)

const OverlayName = "mods-active.ini"
const LauncherDirectory = "../mods"
const maxOverlayBytes = 4 << 20
const maxDirectories = 16
const stateLine = "; state "

type State struct {
	Directories         []string            `json:"directories,omitempty"`
	RelativeDirectories []string            `json:"relativeDirectories,omitempty"`
	Disabled            map[string]bool     `json:"disabled"`
	ExcludedTargets     map[string][]string `json:"excludedTargets,omitempty"`
	Preferred           map[string][]string `json:"preferred"`
	ReviewedSources     map[string][]string `json:"reviewedSources,omitempty"` // eligible pack IDs when the player chose a source for a target
	Automatic           map[string]string   `json:"automatic,omitempty"`
	LanguageProjects    map[string]string   `json:"languageProjects,omitempty"`
	LanguageChoice      map[string]string   `json:"languageChoice,omitempty"`
	BundledTitles       map[string]bool     `json:"bundledTitles,omitempty"`
}

func Read(path string) (State, error) {
	state := State{Disabled: map[string]bool{}, Preferred: map[string][]string{}}
	fi, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return state, nil
	}
	if err != nil {
		return state, err
	}
	if !fi.Mode().IsRegular() || fi.Size() > maxOverlayBytes {
		return state, fmt.Errorf("invalid mod activation file: %s", path)
	}
	file, err := os.Open(path)
	if err != nil {
		return state, err
	}
	defer file.Close()
	raw, err := io.ReadAll(io.LimitReader(file, maxOverlayBytes+1))
	if err != nil {
		return state, err
	}
	if len(raw) > maxOverlayBytes {
		return state, fmt.Errorf("mod activation file exceeds its size limit")
	}
	var metadata bytes.Buffer
	for line := range bytes.Lines(raw) {
		if rest, ok := bytes.CutPrefix(bytes.TrimRight(line, "\r\n"), []byte(stateLine)); ok {
			metadata.Write(rest)
		}
	}
	if metadata.Len() == 0 {
		return state, fmt.Errorf("missing mod state in %s", path)
	}
	if err := json.Unmarshal(metadata.Bytes(), &state); err != nil {
		return state, fmt.Errorf("read mod settings: %w", err)
	}
	if state.Disabled == nil || state.Preferred == nil {
		return state, fmt.Errorf("invalid mod settings")
	}
	if len(state.Directories)+len(state.RelativeDirectories) > maxDirectories {
		return state, fmt.Errorf("too many external mod directories")
	}
	for _, directory := range state.RelativeDirectories {
		if directory != LauncherDirectory && !fs.ValidPath(directory) {
			return state, fmt.Errorf("invalid portable mod directory")
		}
	}
	return state, nil
}

func localDirectory(portable string) (string, error) {
	if portable == LauncherDirectory {
		return filepath.FromSlash(portable), nil
	}
	return filepath.Localize(portable)
}

func (s State) ResolveDirectories(root string) []string {
	// Keep registrations from other platforms in the state if a portable
	// installation is moved, but only open paths valid on this host.
	resolved := slices.DeleteFunc(slices.Clone(s.Directories), func(directory string) bool {
		return !filepath.IsAbs(directory)
	})
	for _, directory := range s.RelativeDirectories {
		if local, err := localDirectory(directory); err == nil {
			full := filepath.Join(root, local)
			if !slices.Contains(resolved, full) {
				resolved = append(resolved, full)
			}
		}
	}
	return resolved
}

// Register stores directories inside the game and the launcher's sibling
// mods directory relative to the game root. Other mounts remain explicit.
func (s *State) Register(root, full string) error {
	relative, err := filepath.Rel(root, full)
	portable := filepath.ToSlash(relative)
	_, localErr := localDirectory(portable)
	if err == nil && localErr == nil && (filepath.IsLocal(relative) || portable == LauncherDirectory) {
		s.Directories = slices.DeleteFunc(s.Directories, func(directory string) bool { return directory == full })
		if !slices.Contains(s.RelativeDirectories, portable) {
			s.RelativeDirectories = append(s.RelativeDirectories, portable)
		}
	} else if !slices.Contains(s.Directories, full) {
		s.Directories = append(s.Directories, full)
	}
	if len(s.Directories)+len(s.RelativeDirectories) > maxDirectories {
		return fmt.Errorf("too many external mod directories")
	}
	return nil
}

func (s State) HasChoices() bool {
	return len(s.Disabled)+len(s.ExcludedTargets)+len(s.Preferred)+len(s.Automatic)+len(s.LanguageChoice)+len(s.LanguageProjects)+len(s.BundledTitles) != 0
}

func (s State) Header() ([]byte, error) {
	data, err := json.Marshal(s)
	if err != nil {
		return nil, err
	}
	var out bytes.Buffer
	out.WriteString("; ActRaiser mod activation and generated asset overlay\n")
	// Each comment must fit the runtime's line buffer.
	for len(data) > 0 {
		n := min(len(data), 800)
		out.WriteString(stateLine)
		out.Write(data[:n])
		out.WriteByte('\n')
		data = data[n:]
	}
	return out.Bytes(), nil
}
