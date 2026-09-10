package localizationkit

import (
	"crypto/sha256"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// InstalledPackSummary describes discovery, not font coverage or translation
// completeness. Key is a single directory name below the caller's packs root.
type InstalledPackSummary struct {
	Key      string       `json:"key"`
	Metadata PackMetadata `json:"metadata"`
	Revision string       `json:"revision"`
	Enabled  bool         `json:"enabled"`
	Archive  bool         `json:"archive,omitempty"`
	Error    string       `json:"error,omitempty"`
}

const disabledPackManifest = "disabled-pack.ini"

func installedManifest(dir string) (string, bool, error) {
	_, activeErr := os.Lstat(filepath.Join(dir, "pack.ini"))
	_, disabledErr := os.Lstat(filepath.Join(dir, disabledPackManifest))
	if activeErr != nil && !errors.Is(activeErr, os.ErrNotExist) {
		return "", false, activeErr
	}
	if disabledErr != nil && !errors.Is(disabledErr, os.ErrNotExist) {
		return "", false, disabledErr
	}
	if activeErr == nil && disabledErr == nil {
		return "", false, fmt.Errorf("both enabled and disabled manifests exist; restore one explicitly")
	}
	if activeErr == nil {
		return "pack.ini", true, nil
	}
	if disabledErr == nil {
		return disabledPackManifest, false, nil
	}
	return "", false, os.ErrNotExist
}

type installedPackFS struct {
	fs.FS
	manifest string
}

func (f installedPackFS) Open(name string) (fs.File, error) {
	if name == "pack.ini" {
		name = f.manifest
	}
	return f.FS.Open(name)
}

func openInstalledAuthorPack(dir, manifest string) (*AuthorPack, error) {
	root, err := os.OpenRoot(dir)
	if err != nil {
		return nil, err
	}
	defer root.Close()
	return LoadAuthorPack(installedPackFS{rootedPackFS{root}, manifest})
}

// InspectInstalledPack includes disabled packages, without loading scripts.
func InspectInstalledPack(root, key string) (InstalledPackSummary, error) {
	return readInstalledSummary(root, key)
}

func installedDirectory(root, key string) (string, error) {
	if !PortablePackPath(key) || strings.ContainsAny(key, `/\`) {
		return "", fmt.Errorf("invalid installed package directory")
	}
	for _, path := range []string{root, filepath.Join(root, key)} {
		info, err := os.Lstat(path)
		if err != nil {
			return "", err
		}
		if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return "", fmt.Errorf("installed package path is not a regular directory")
		}
	}
	return filepath.Join(root, key), nil
}

func readInstalledSummary(root, key string) (InstalledPackSummary, error) {
	row := InstalledPackSummary{Key: key}
	if info, err := os.Lstat(root); err != nil {
		return row, err
	} else if !info.IsDir() {
		return row, fmt.Errorf("installed packages root is not a regular directory")
	}
	if IsLanguageArchiveName(key) {
		return readInstalledArchive(root, key)
	}
	dir, err := installedDirectory(root, key)
	if err != nil {
		return row, err
	}
	name, enabled, err := installedManifest(dir)
	if err != nil {
		return row, err
	}
	row.Enabled = enabled
	f, err := openRegularAuthorFile(filepath.Join(dir, name))
	if err != nil {
		return row, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, MaxPackManifestBytes+1))
	if err != nil {
		return row, err
	}
	if len(data) > MaxPackManifestBytes {
		return row, fmt.Errorf("installed manifest too large")
	}
	manifest, err := ParsePackManifest(string(data), "pack.ini")
	if err != nil {
		return row, err
	}
	row.Metadata = manifest.Metadata()
	if row.Metadata.ID != key || row.Metadata.Target != "us-runtime" || row.Metadata.SourceProfile != "us" || strings.EqualFold(key, "native-us") {
		return row, fmt.Errorf("folder name must match a community US-runtime package ID")
	}
	row.Revision = fmt.Sprintf("%x", sha256.Sum256(append([]byte(name+"\x00"), data...)))
	return row, nil
}

// MaximumEnabledPacks is the game's supported number of enabled packages
// (kArLanguagePackCatalogMaximum in src/localization/pack_discovery.h). Enabling
// more would install packages the player cannot select, so the builder refuses
// instead of pretending they are available.
const MaximumEnabledPacks = 128

// Workshop installations retain every draft message as unpacked snapshots.
// Never silently shadow a distributed archive with an editor's local draft.
func InstallProjectInLibrary(root string, project *AuthorProject, replace bool) (string, error) {
	if project == nil {
		return "", fmt.Errorf("project is required")
	}
	if err := makeAuthorDirectory(root); err != nil {
		return "", err
	}
	unlock, err := authorLock(root)
	if err != nil {
		return "", err
	}
	defer unlock()
	rows, err := ListInstalledPacks(root)
	if err != nil {
		return "", err
	}
	id := project.Pack().Manifest().Metadata().ID
	active, exists := 0, false
	for _, row := range rows {
		if row.Enabled {
			active++
		}
		if strings.EqualFold(row.Metadata.ID, id) {
			if row.Archive || row.Key != id {
				return "", fmt.Errorf("package %s is installed as %s; uninstall that copy first, or export a reviewed .arlang and replace its archive", id, row.Key)
			}
			exists = true
		}
	}
	if !exists && active >= MaximumEnabledPacks {
		return "", fmt.Errorf("only %d enabled packages are supported", MaximumEnabledPacks)
	}
	return InstallAuthorProject(filepath.Join(root, id), project, replace)
}

// SetLanguagePackEnabled toggles discovery by renaming only the manifest.
// Versioned script/font paths remain stable for running games; re-enabling
// validates the declared files.
func SetLanguagePackEnabled(root, key, id, expected string, enabled bool) error {
	if IsLanguageArchiveName(key) {
		return setArchiveEnabled(root, key, id, expected, enabled)
	}
	dir, err := installedDirectory(root, key)
	if err != nil {
		return err
	}
	unlock, err := authorLock(dir)
	if err != nil {
		return err
	}
	defer unlock()
	row, err := readInstalledSummary(root, key)
	if err != nil {
		return err
	}
	if expected == "" || row.Revision != expected || row.Metadata.ID != id {
		return ErrProjectConflict
	}
	if row.Enabled == enabled {
		return nil
	}
	source, target := "pack.ini", disabledPackManifest
	if enabled {
		source, target = target, source
		if _, err := openInstalledAuthorPack(dir, source); err != nil {
			return err
		}
		rows, err := ListInstalledPacks(root)
		if err != nil {
			return err
		}
		active := 0
		for _, other := range rows {
			if other.Enabled {
				active++
				if strings.EqualFold(other.Metadata.ID, id) {
					return fmt.Errorf("another enabled installation has package ID %s", id)
				}
			}
		}
		if active >= MaximumEnabledPacks {
			return fmt.Errorf("the game offers %d enabled language packages; disable one before enabling another", MaximumEnabledPacks)
		}
	}
	if _, err := os.Lstat(filepath.Join(dir, target)); !errors.Is(err, os.ErrNotExist) {
		if err != nil {
			return err
		}
		return ErrProjectConflict
	}
	return os.Rename(filepath.Join(dir, source), filepath.Join(dir, target))
}

// Scan only manifests; loading/decoding every script on a library visit would
// turn a small directory view into an expensive validation pass.
func ListInstalledPacks(root string) ([]InstalledPackSummary, error) {
	rows := []InstalledPackSummary{}
	info, err := os.Lstat(root)
	if errors.Is(err, os.ErrNotExist) {
		return rows, nil
	}
	if err != nil {
		return nil, err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return nil, fmt.Errorf("installed packages root is not a regular directory")
	}
	entries, err := os.ReadDir(root)
	if err != nil {
		return nil, err
	}
	if len(entries) > 1024 {
		return nil, fmt.Errorf("too many installed package entries")
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".") {
			continue
		}
		if !entry.IsDir() && entry.Type()&os.ModeSymlink == 0 && !IsLanguageArchiveName(entry.Name()) {
			continue
		}
		row, err := readInstalledSummary(root, entry.Name())
		if errors.Is(err, os.ErrNotExist) {
			continue // Previously uninstalled, or not a pack directory.
		}
		if err != nil {
			row.Error = err.Error()
		}
		rows = append(rows, row)
	}
	counts := make(map[string]int, len(rows))
	for _, row := range rows {
		if row.Enabled && row.Metadata.ID != "" {
			counts[strings.ToLower(row.Metadata.ID)]++
		}
	}
	for i := range rows {
		if rows[i].Enabled && counts[strings.ToLower(rows[i].Metadata.ID)] > 1 {
			rows[i].Error = "Conflicting enabled package ID; disable or uninstall a duplicate."
		}
	}
	return rows, nil
}

// Uninstall disables only the discovery manifest. Version files, fonts and a
// recoverable manifest remain in place, so a running game isn't broken and no
// author project is touched. Reinstallation publishes a new pack.ini normally.
func UninstallLanguagePack(root, key, id, expected string) (string, error) {
	if IsLanguageArchiveName(key) {
		return uninstallArchive(root, key, id, expected)
	}
	dir, err := installedDirectory(root, key)
	if err != nil {
		return "", err
	}
	unlock, err := authorLock(dir)
	if err != nil {
		return "", err
	}
	defer unlock()
	row, err := readInstalledSummary(root, key)
	if err != nil {
		return "", err
	}
	if expected == "" || row.Revision != expected || row.Metadata.ID != id {
		return "", ErrProjectConflict
	}
	f, err := os.CreateTemp(dir, "uninstalled-*.ini")
	if err != nil {
		return "", err
	}
	backup := f.Name()
	if err = f.Close(); err != nil {
		_ = os.Remove(backup)
		return "", err
	}
	manifest := "pack.ini"
	if !row.Enabled {
		manifest = disabledPackManifest
	}
	if err = os.Rename(filepath.Join(dir, manifest), backup); err != nil {
		_ = os.Remove(backup) // Empty temporary file owned by this operation.
		return "", err
	}
	return backup, nil
}
