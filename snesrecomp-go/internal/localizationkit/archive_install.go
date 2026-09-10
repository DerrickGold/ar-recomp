package localizationkit

import (
	"bytes"
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
	"strings"
)

const archiveCacheDirectory = ".arlang-cache"
const archiveStateDirectory = ".arlang-state"
const ArchiveCatalogHeader = "actraiser-language-catalog\t1\n"

func IsLanguageArchiveName(key string) bool {
	return !strings.HasPrefix(key, ".") && PortablePackPath(key) && !strings.Contains(key, "/") && strings.HasSuffix(strings.ToLower(key), ".arlang")
}

func requirePublication(index *archiveIndex) error {
	m := index.manifest.Metadata()
	if index.header.Kind != "publication" || m.Target != "us-runtime" || m.SourceProfile != "us" || strings.EqualFold(m.ID, "native-us") {
		return fmt.Errorf("only community US-runtime publication archives can be installed; private backups and regional extracts are references")
	}
	return nil
}

func archiveDisabledPath(root, id string) string {
	return filepath.Join(root, archiveStateDirectory, strings.ToLower(id)+".disabled")
}

func archiveEnabled(root, id string) (bool, error) {
	state := filepath.Join(root, archiveStateDirectory)
	if info, err := os.Lstat(state); err == nil {
		if !info.IsDir() {
			return false, fmt.Errorf("archive state is not a regular directory")
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return false, err
	}
	info, err := os.Lstat(archiveDisabledPath(root, id))
	if errors.Is(err, os.ErrNotExist) {
		return true, nil
	}
	if err != nil {
		return false, err
	}
	if !info.Mode().IsRegular() || info.Size() != 0 {
		return false, fmt.Errorf("invalid archive disable marker")
	}
	return false, nil
}

func readInstalledArchive(root, key string) (InstalledPackSummary, error) {
	row := InstalledPackSummary{Key: key, Archive: true}
	f, err := openRegularAuthorFile(filepath.Join(root, key))
	if err != nil {
		return row, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return row, err
	}
	index, err := inspectAuthorArchive(f, info.Size())
	if err != nil {
		return row, err
	}
	if err = requirePublication(index); err != nil {
		return row, err
	}
	row.Metadata = index.manifest.Metadata()
	row.Enabled, err = archiveEnabled(root, row.Metadata.ID)
	row.Revision = fmt.Sprintf("%s:%t", index.revision, row.Enabled)
	return row, err
}

func setArchiveEnabled(root, key, id, expected string, enabled bool) error {
	unlock, err := authorLock(root)
	if err != nil {
		return err
	}
	defer unlock()
	row, err := readInstalledArchive(root, key)
	if err != nil {
		return err
	}
	if expected == "" || row.Revision != expected || row.Metadata.ID != id {
		return ErrProjectConflict
	}
	if row.Enabled == enabled {
		return nil
	}
	if enabled {
		p, err := OpenAuthorInput(filepath.Join(root, key))
		if err != nil {
			return err
		}
		if _, _, err = p.Installation(); err != nil {
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
			return fmt.Errorf("only %d enabled packages are supported", MaximumEnabledPacks)
		}
		return os.Remove(archiveDisabledPath(root, id))
	}
	if err := makeAuthorDirectory(filepath.Join(root, archiveStateDirectory)); err != nil {
		return err
	}
	return atomicAuthorFile(archiveDisabledPath(root, id), func(io.Writer) error { return nil })
}

func uninstallArchive(root, key, id, expected string) (string, error) {
	unlock, err := authorLock(root)
	if err != nil {
		return "", err
	}
	defer unlock()
	row, err := readInstalledArchive(root, key)
	if err != nil {
		return "", err
	}
	if expected == "" || row.Metadata.ID != id || row.Revision != expected {
		return "", ErrProjectConflict
	}
	recovery := filepath.Join(root, ".uninstalled")
	if err = makeAuthorDirectory(recovery); err != nil {
		return "", err
	}
	f, err := os.CreateTemp(recovery, "archive-*.arlang")
	if err != nil {
		return "", err
	}
	backup := f.Name()
	if err = f.Close(); err == nil {
		err = os.Rename(filepath.Join(root, key), backup)
	}
	if err != nil {
		os.Remove(backup)
		return "", err
	}
	return backup, nil
}

// InstallLanguageArchive copies a complete publication, never extracts over a
// working directory or grants publishing rights. The game checks fonts on use.
func InstallLanguageArchive(root, source string, replace bool) (string, error) {
	f, err := openRegularAuthorFile(source)
	if err != nil {
		return "", err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return "", err
	}
	index, err := inspectAuthorArchive(f, info.Size())
	if err != nil {
		return "", err
	}
	if err = requirePublication(index); err != nil {
		return "", err
	}
	data, err := io.ReadAll(io.LimitReader(f, MaxAuthorArchiveBytes+1))
	if err != nil {
		return "", err
	}
	if int64(len(data)) != info.Size() {
		return "", fmt.Errorf("archive changed while reading")
	}
	index, err = inspectAuthorArchive(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return "", err
	}
	if err = requirePublication(index); err != nil {
		return "", err
	}
	p, err := ReadAuthorArchive(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return "", err
	}
	if _, _, err = p.Installation(); err != nil {
		return "", err
	}
	if err = makeAuthorDirectory(root); err != nil {
		return "", err
	}
	unlock, err := authorLock(root)
	if err != nil {
		return "", err
	}
	defer unlock()
	id := p.Pack().Manifest().Metadata().ID
	key := id + ".arlang"
	rows, err := ListInstalledPacks(root)
	if err != nil {
		return "", err
	}
	active := 0
	found := false
	for _, other := range rows {
		if other.Enabled {
			active++
		}
		if strings.EqualFold(other.Metadata.ID, id) {
			if found || !replace || !other.Archive {
				return "", fmt.Errorf("package ID %s is already installed; remove conflicting copies or explicitly replace its archive", id)
			}
			key, found = other.Key, true
		}
	}
	if enabled, err := archiveEnabled(root, id); err != nil {
		return "", err
	} else if enabled && !found && active >= MaximumEnabledPacks {
		return "", fmt.Errorf("only %d enabled packages are supported", MaximumEnabledPacks)
	}
	path := filepath.Join(root, key)
	if !found {
		if _, err := os.Lstat(path); !errors.Is(err, os.ErrNotExist) {
			return "", fmt.Errorf("installation target already exists or cannot be inspected")
		}
	}
	if err = atomicAuthorFile(path, func(w io.Writer) error { _, err := w.Write(data); return err }); err != nil {
		return "", err
	}
	return path, nil
}

func cacheVersionValid(token string) bool {
	return strings.HasPrefix(token, "v-") && len(token) > 2 && len(token) <= 32 && strings.Trim(token[2:], "0123456789") == ""
}

// Cache files are immutable while a game may be reading them. A missing or
// damaged snapshot gets a new generation; neither updates nor removals delete
// an older generation. Players may discard the whole cache with the game shut.
func validArchiveCache(dir string, index *archiveIndex) bool {
	r, err := os.OpenRoot(dir)
	if err != nil {
		return false
	}
	defer r.Close()
	fsys := rootedPackFS{r}
	manifest, err := readPackFile(fsys, "pack.ini", MaxPackManifestBytes)
	if err != nil || string(manifest) != index.manifest.Text() {
		return false
	}
	for name, member := range index.files {
		if name == "package.json" {
			continue
		}
		f, err := fsys.Open(name)
		if err != nil {
			return false
		}
		info, err := f.Stat()
		if err != nil || info.Size() != int64(member.UncompressedSize64) {
			f.Close()
			return false
		}
		checksum := crc32.NewIEEE()
		n, readErr := io.Copy(checksum, io.LimitReader(f, info.Size()+1))
		closeErr := f.Close()
		if readErr != nil || closeErr != nil || n != info.Size() || checksum.Sum32() != member.CRC32 {
			return false
		}
	}
	return true
}

func prepareArchive(root, key string) (string, string, error) {
	f, err := openRegularAuthorFile(filepath.Join(root, key))
	if err != nil {
		return "", "", err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return "", "", err
	}
	index, err := inspectAuthorArchive(f, info.Size())
	if err != nil {
		return "", "", err
	}
	if err = requirePublication(index); err != nil {
		return "", "", err
	}
	h := sha256.New()
	n, err := io.Copy(h, io.LimitReader(f, MaxAuthorArchiveBytes+1))
	if err != nil || n != info.Size() {
		return "", "", fmt.Errorf("archive changed while hashing")
	}
	digest := fmt.Sprintf("%x", h.Sum(nil))
	object := filepath.Join(root, archiveCacheDirectory, digest)
	if err := makeAuthorDirectory(object); err != nil {
		return "", "", err
	}
	unlock, err := authorLock(object)
	if err != nil {
		return "", "", err
	}
	defer unlock()
	// The pointer is bounded and cannot redirect us outside this hash object.
	r, err := os.OpenRoot(object)
	if err != nil {
		return "", "", err
	}
	current, currentErr := readPackFile(rootedPackFS{r}, "current", 64)
	r.Close()
	token := strings.TrimSpace(string(current))
	if currentErr == nil && cacheVersionValid(token) {
		if info, err := os.Lstat(filepath.Join(object, token)); err == nil && info.IsDir() && validArchiveCache(filepath.Join(object, token), index) {
			return index.manifest.Metadata().ID, digest + "/" + token, nil
		}
	}
	if _, err = f.Seek(0, io.SeekStart); err != nil {
		return "", "", err
	}
	data, err := io.ReadAll(io.LimitReader(f, MaxAuthorArchiveBytes+1))
	if err != nil {
		return "", "", err
	}
	if fmt.Sprintf("%x", sha256.Sum256(data)) != digest {
		return "", "", fmt.Errorf("archive changed while preparing")
	}
	p, err := ReadAuthorArchive(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return "", "", err
	}
	if _, _, err = p.Installation(); err != nil {
		return "", "", err
	}
	version, err := os.MkdirTemp(object, "v-")
	if err != nil {
		return "", "", err
	}
	committed := false
	defer func() {
		if !committed {
			os.RemoveAll(version)
		}
	}()
	for name, data := range p.projectFiles(false) {
		path := filepath.Join(version, filepath.FromSlash(name))
		if err := makeAuthorDirectory(filepath.Dir(path)); err != nil {
			return "", "", err
		}
		if err := atomicAuthorFile(path, func(w io.Writer) error { _, err := w.Write(data); return err }); err != nil {
			return "", "", err
		}
	}
	if !validArchiveCache(version, index) {
		return "", "", fmt.Errorf("prepared archive failed dependency verification")
	}
	token = filepath.Base(version)
	if err := atomicAuthorFile(filepath.Join(object, "current"), func(w io.Writer) error { _, err := io.WriteString(w, token+"\n"); return err }); err != nil {
		return "", "", err
	}
	committed = true
	return p.Pack().Manifest().Metadata().ID, digest + "/" + token, nil
}

// PrepareLanguageArchives is the versioned desktop startup protocol. It writes
// an atomic bounded index of safe cache tokens, never arbitrary paths or code.
// Diagnostics go to the caller's log. Bad packs cannot leave stale index rows.
func PrepareLanguageArchives(ctx context.Context, root string, log io.Writer) error {
	rows, err := ListInstalledPacks(root)
	if err != nil {
		return err
	}
	cache := filepath.Join(root, archiveCacheDirectory)
	if err = makeAuthorDirectory(cache); err != nil {
		return err
	}
	var index strings.Builder
	index.WriteString(ArchiveCatalogHeader)
	counts := map[string]int{}
	for _, row := range rows {
		if row.Enabled && row.Metadata.ID != "" {
			counts[strings.ToLower(row.Metadata.ID)]++
		}
	}
	conflicts := map[string]bool{}
	for _, row := range rows {
		if err := ctx.Err(); err != nil {
			return err
		}
		id := strings.ToLower(row.Metadata.ID)
		if row.Enabled && counts[id] > 1 {
			if !conflicts[id] {
				fmt.Fprintf(&index, "conflict\t%s\n", id)
				conflicts[id] = true
			}
			fmt.Fprintf(log, "[localization] conflicting package ID %s: %s\n", id, row.Key)
			continue
		}
		if row.Error != "" {
			fmt.Fprintf(log, "[localization] skipping %s: %s\n", row.Key, row.Error)
			continue
		}
		if !row.Archive || !row.Enabled {
			continue
		}
		actualID, token, err := prepareArchive(root, row.Key)
		if err != nil {
			fmt.Fprintf(log, "[localization] skipping %s: %s\n", row.Key, err)
			continue
		}
		if actualID != row.Metadata.ID {
			return fmt.Errorf("archive identity changed; retry after copying completes")
		}
		fmt.Fprintf(&index, "pack\t%s\t%s\n", actualID, token)
	}
	return atomicAuthorFile(filepath.Join(cache, "catalog-v1.tsv"), func(w io.Writer) error { _, err := io.WriteString(w, index.String()); return err })
}
