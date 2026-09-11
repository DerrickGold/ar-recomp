package desktop

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

const importStateName = ".actraiser-import.json"

var importLeaves = []string{"config.ini", "settings.ini", "diorama-layers.ini", "saves", "game-assets"}

type ImportDecision struct {
	Schema  int      `json:"schema"`
	Decided bool     `json:"decided"`
	Sources []string `json:"sources"`
}

type ImportFile struct {
	Path            string `json:"path"`
	Action          string `json:"action"` // copy, replace-default, identical, or conflict
	SourceHash      string `json:"sourceHash"`
	DestinationHash string `json:"destinationHash"`
}

type ImportPlan struct {
	Source          string       `json:"source"`
	Destination     string       `json:"destination"`
	Files           []ImportFile `json:"files"`
	Copy            int          `json:"copy"`
	Conflicts       int          `json:"conflicts"`
	Identical       int          `json:"identical"`
	AlreadyImported bool         `json:"alreadyImported"`
	Revision        string       `json:"revision"`
}

// Separate from seed history: opening the Workshop must not consume the user's
// opportunity to import. A decision belongs to the output, not this executable.
func ReadImportDecision(destination string) (ImportDecision, error) {
	state := ImportDecision{Schema: 1}
	if err := safeDestination(destination, importStateName); err != nil {
		return state, err
	}
	f, err := os.Open(filepath.Join(destination, importStateName))
	if errors.Is(err, os.ErrNotExist) {
		return state, nil
	}
	if err != nil {
		return state, err
	}
	defer f.Close()
	if err = json.NewDecoder(io.LimitReader(f, 1<<20)).Decode(&state); err != nil {
		return state, err
	}
	if state.Schema != 1 {
		return state, errors.New("unsupported installation import history")
	}
	return state, nil
}

func writeImportDecision(destination string, state ImportDecision) error {
	if err := safeDestination(destination, importStateName); err != nil {
		return err
	}
	raw, err := json.Marshal(state)
	if err != nil {
		return err
	}
	return atomicWrite(filepath.Join(destination, importStateName), raw, 0600)
}

func SkipInstallImport(destination string) error {
	unlock, err := lockInitialization(destination)
	if err != nil {
		return err
	}
	defer unlock()
	state, err := ReadImportDecision(destination)
	if err != nil {
		return err
	}
	state.Decided = true
	return writeImportDecision(destination, state)
}

func recognizedData(root string) bool {
	for _, marker := range []string{"game-assets/manifest.ini", seedStateName} {
		if safeDestination(root, filepath.FromSlash(marker)) != nil {
			continue
		}
		if info, err := os.Lstat(filepath.Join(root, marker)); err == nil && info.Mode().IsRegular() {
			return true
		}
	}
	return false
}

// DiscoverInstallData probes only fixed layouts and game sidecars in the selected
// folder. It never scans the disk, guesses a user's home, or executes old tools.
func DiscoverInstallData(directory, destination string) ([]string, error) {
	if !filepath.IsAbs(directory) {
		return nil, errors.New("choose an absolute installation folder")
	}
	base, err := filepath.EvalSymlinks(directory)
	if err != nil {
		return nil, err
	}
	candidates := []string{base, filepath.Join(base, "utils")}
	entries, err := os.ReadDir(base)
	if err != nil {
		return nil, err
	}
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasPrefix(name, "ActRaiserRecomp") || strings.HasPrefix(name, "ActRaiserRecompBuilder") || !strings.HasSuffix(name, ".portable") || !entry.Type().IsRegular() {
			continue
		}
		f, err := os.Open(filepath.Join(base, name))
		if err != nil {
			continue
		}
		raw, readErr := io.ReadAll(io.LimitReader(f, 4097))
		f.Close()
		relative := strings.TrimSpace(string(raw))
		if readErr == nil && len(raw) <= 4096 && localPath(relative, true) {
			candidates = append(candidates, filepath.Join(base, relative))
		}
	}
	output, err := resolveDataPath(destination)
	if err != nil {
		return nil, err
	}
	var found []string
	seen := map[string]bool{}
	for _, candidate := range candidates {
		// Refuse symlinked data directories; the user may explicitly select the
		// real directory instead. Never traverse a sidecar out of this folder.
		physical, err := filepath.EvalSymlinks(candidate)
		if err != nil || physical != candidate || physical == output || seen[physical] {
			continue
		}
		if recognizedData(candidate) {
			found = append(found, candidate)
			seen[physical] = true
		}
	}
	return found, nil
}

func importHash(root *os.Root, leaf string) (string, error) {
	f, err := root.Open(leaf)
	if err != nil {
		return "", err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return "", err
	}
	if !info.Mode().IsRegular() {
		return "", fmt.Errorf("not a regular import file: %s", leaf)
	}
	hash := sha256.New()
	if _, err := io.Copy(hash, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

// PreviewInstallImport is read-only. Live user files win; only exact hashes of
// untouched seeded game assets may be replaced. Defaults themselves, binaries,
// ROMs, caches outside game-assets, and build tools are not imported.
func PreviewInstallImport(ctx context.Context, source, destination string) (ImportPlan, error) {
	p := ImportPlan{Files: []ImportFile{}}
	if !filepath.IsAbs(source) || !filepath.IsAbs(destination) {
		return p, errors.New("import paths must be absolute")
	}
	var err error
	p.Source, err = filepath.EvalSymlinks(source)
	if err != nil {
		return p, err
	}
	p.Destination, err = filepath.EvalSymlinks(destination)
	if err != nil {
		return p, err
	}
	if !recognizedData(p.Source) {
		return p, errors.New("no recognized ActRaiserRecomp data in that folder; select its data folder or utils directory")
	}
	if rel, err := filepath.Rel(p.Destination, p.Source); err != nil || localPath(rel, true) {
		return p, errors.New("source must be outside the game output")
	}
	for _, leaf := range importLeaves {
		if rel, err := filepath.Rel(filepath.Join(p.Source, leaf), p.Destination); err == nil && localPath(rel, true) {
			return p, errors.New("game output overlaps imported data")
		}
	}
	state, err := ReadImportDecision(p.Destination)
	if err != nil {
		return p, err
	}
	for _, previous := range state.Sources {
		if previous == p.Source {
			p.AlreadyImported = true
		}
	}
	seed := map[string]string{}
	if err = safeDestination(p.Destination, seedStateName); err != nil {
		return p, err
	}
	if raw, e := os.ReadFile(filepath.Join(p.Destination, seedStateName)); e == nil {
		if err = json.Unmarshal(raw, &seed); err != nil {
			return p, err
		}
	} else if !errors.Is(e, os.ErrNotExist) {
		return p, e
	}
	root, err := os.OpenRoot(p.Source)
	if err != nil {
		return p, err
	}
	defer root.Close()
	var total int64
	for _, leaf := range importLeaves {
		if _, err := root.Lstat(leaf); errors.Is(err, os.ErrNotExist) {
			continue
		} else if err != nil {
			return p, err
		}
		err = fs.WalkDir(root.FS(), leaf, func(path string, entry fs.DirEntry, walkErr error) error {
			if err := ctx.Err(); err != nil {
				return err
			}
			if walkErr != nil {
				return walkErr
			}
			if entry.IsDir() {
				return nil
			}
			if !entry.Type().IsRegular() {
				return fmt.Errorf("refusing symlink or special import file: %s", path)
			}
			info, err := entry.Info()
			if err != nil {
				return err
			}
			total += info.Size()
			if len(p.Files) >= 100000 || total > 20<<30 {
				return errors.New("installation exceeds import safety limit (100,000 files / 20 GiB)")
			}
			if err := safeDestination(p.Destination, filepath.FromSlash(path)); err != nil {
				return err
			}
			item := ImportFile{Path: path, Action: "copy"}
			item.SourceHash, err = importHash(root, filepath.FromSlash(path))
			if err != nil {
				return err
			}
			item.DestinationHash, err = fileHash(filepath.Join(p.Destination, path))
			if err != nil && !errors.Is(err, os.ErrNotExist) {
				return err
			}
			if err == nil {
				switch {
				case item.SourceHash == item.DestinationHash:
					item.Action = "identical"
					p.Identical++
				case strings.HasPrefix(path, "game-assets/") && seed[path] == item.DestinationHash:
					item.Action = "replace-default"
					p.Copy++
				default:
					item.Action = "conflict"
					p.Conflicts++
				}
			} else if _, seeded := seed[path]; seeded {
				// An absent file from a known seed was deliberately deleted.
				item.Action = "conflict"
				p.Conflicts++
			} else {
				p.Copy++
			}
			p.Files = append(p.Files, item)
			return nil
		})
		if err != nil {
			return p, err
		}
	}
	raw, _ := json.Marshal(p)
	hash := sha256.Sum256(raw)
	p.Revision = hex.EncodeToString(hash[:])
	return p, nil
}

// ApplyInstallImport requires the exact reviewed snapshot. Partial failures are
// retryable; successful copies compare identical on retry, and no receipt is
// published until completion. The old install is never written or removed.
func ApplyInstallImport(ctx context.Context, plan ImportPlan) (ImportPlan, error) {
	unlock, err := lockInitialization(plan.Destination)
	if err != nil {
		return plan, err
	}
	defer unlock()
	current, err := PreviewInstallImport(ctx, plan.Source, plan.Destination)
	if err != nil {
		return current, err
	}
	if current.Revision != plan.Revision {
		return current, errors.New("installation changed since preview; review it again before importing")
	}
	if current.AlreadyImported {
		return current, errors.New("this installation has already been imported into this output")
	}
	root, err := os.OpenRoot(plan.Source)
	if err != nil {
		return current, err
	}
	defer root.Close()
	for _, item := range current.Files {
		if err := ctx.Err(); err != nil {
			return current, err
		}
		if item.Action != "copy" && item.Action != "replace-default" {
			continue
		}
		if err := safeDestination(plan.Destination, filepath.FromSlash(item.Path)); err != nil {
			return current, err
		}
		path := filepath.Join(plan.Destination, item.Path)
		hash, err := fileHash(path)
		if (err != nil && !errors.Is(err, os.ErrNotExist)) || hash != item.DestinationHash {
			return current, fmt.Errorf("destination changed during import: %s", item.Path)
		}
		input, err := root.Open(filepath.FromSlash(item.Path))
		if err != nil {
			return current, err
		}
		// Validate source bytes before publication, without buffering large assets
		// in memory. Staging is on the destination filesystem for atomic rename.
		err = writeVerifiedImport(path, input, item.SourceHash)
		input.Close()
		if err != nil {
			return current, err
		}
	}
	state, err := ReadImportDecision(plan.Destination)
	if err != nil {
		return current, err
	}
	state.Decided = true
	state.Sources = append(state.Sources, plan.Source)
	return current, writeImportDecision(plan.Destination, state)
}

func writeVerifiedImport(path string, input io.Reader, expected string) error {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(filepath.Dir(path), ".actraiser-import-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	defer f.Close()
	hash := sha256.New()
	if _, err = io.Copy(io.MultiWriter(f, hash), input); err != nil {
		return err
	}
	if hex.EncodeToString(hash.Sum(nil)) != expected {
		return errors.New("source changed during import; originals remain intact; preview and retry")
	}
	if err = f.Sync(); err != nil {
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	return os.Rename(f.Name(), path)
}
