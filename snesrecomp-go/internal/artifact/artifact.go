// Package artifact captures and compares deterministic regeneration outputs.
package artifact

import (
	"archive/tar"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const archiveRoot = "src/gen"

type File struct {
	Path   string `json:"path"`
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

type Manifest struct {
	Version int    `json:"version"`
	Root    string `json:"root"`
	Files   []File `json:"files"`
}

type Difference struct {
	Path     string
	Expected *File
	Actual   *File
}

// FromDir builds a stable, relative-path-sorted manifest of regular files.
func FromDir(root string) (Manifest, error) {
	manifest := Manifest{Version: 1, Root: archiveRoot}
	err := filepath.WalkDir(root, func(filePath string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			return nil
		}
		if entry.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("artifact tree contains symlink %s", filePath)
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("artifact tree contains non-regular file %s", filePath)
		}
		relative, err := filepath.Rel(root, filePath)
		if err != nil {
			return err
		}
		digest, err := hashFile(filePath)
		if err != nil {
			return err
		}
		manifest.Files = append(manifest.Files, File{
			Path:   filepath.ToSlash(relative),
			Size:   info.Size(),
			SHA256: digest,
		})
		return nil
	})
	if err != nil {
		return Manifest{}, fmt.Errorf("walk artifact tree: %w", err)
	}
	sort.Slice(manifest.Files, func(i, j int) bool { return manifest.Files[i].Path < manifest.Files[j].Path })
	return manifest, nil
}

// Capture writes a byte-stable tar.gz archive and returns its manifest.
func Capture(sourceDir, archivePath string) (Manifest, error) {
	manifest, err := FromDir(sourceDir)
	if err != nil {
		return Manifest{}, err
	}
	output, err := os.Create(archivePath)
	if err != nil {
		return Manifest{}, fmt.Errorf("create baseline archive: %w", err)
	}
	ok := false
	defer func() {
		_ = output.Close()
		if !ok {
			_ = os.Remove(archivePath)
		}
	}()

	gz, err := gzip.NewWriterLevel(output, gzip.BestCompression)
	if err != nil {
		return Manifest{}, fmt.Errorf("create gzip stream: %w", err)
	}
	gz.Header.ModTime = time.Unix(0, 0)
	gz.Header.OS = 255
	tw := tar.NewWriter(gz)
	for _, item := range manifest.Files {
		filePath := filepath.Join(sourceDir, filepath.FromSlash(item.Path))
		header := &tar.Header{
			Name:     path.Join(archiveRoot, item.Path),
			Mode:     0o644,
			Size:     item.Size,
			ModTime:  time.Unix(0, 0),
			Typeflag: tar.TypeReg,
			Format:   tar.FormatPAX,
		}
		if err := tw.WriteHeader(header); err != nil {
			return Manifest{}, fmt.Errorf("write tar header for %s: %w", item.Path, err)
		}
		input, err := os.Open(filePath)
		if err != nil {
			return Manifest{}, fmt.Errorf("open %s: %w", item.Path, err)
		}
		_, copyErr := io.Copy(tw, input)
		closeErr := input.Close()
		if copyErr != nil {
			return Manifest{}, fmt.Errorf("archive %s: %w", item.Path, copyErr)
		}
		if closeErr != nil {
			return Manifest{}, fmt.Errorf("close %s: %w", item.Path, closeErr)
		}
	}
	if err := tw.Close(); err != nil {
		return Manifest{}, fmt.Errorf("close tar stream: %w", err)
	}
	if err := gz.Close(); err != nil {
		return Manifest{}, fmt.Errorf("close gzip stream: %w", err)
	}
	if err := output.Close(); err != nil {
		return Manifest{}, fmt.Errorf("close baseline archive: %w", err)
	}
	ok = true
	return manifest, nil
}

// FromArchive hashes an artifact archive without extracting it.
func FromArchive(archivePath string) (Manifest, error) {
	input, err := os.Open(archivePath)
	if err != nil {
		return Manifest{}, fmt.Errorf("open baseline archive: %w", err)
	}
	defer input.Close()
	gz, err := gzip.NewReader(input)
	if err != nil {
		return Manifest{}, fmt.Errorf("open gzip stream: %w", err)
	}
	defer gz.Close()

	manifest := Manifest{Version: 1, Root: archiveRoot}
	tr := tar.NewReader(gz)
	for {
		header, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return Manifest{}, fmt.Errorf("read baseline archive: %w", err)
		}
		if header.Typeflag == tar.TypeDir {
			continue
		}
		if header.Typeflag != tar.TypeReg && header.Typeflag != tar.TypeRegA {
			return Manifest{}, fmt.Errorf("baseline archive contains non-regular entry %s", header.Name)
		}
		clean := path.Clean(header.Name)
		prefix := archiveRoot + "/"
		if !strings.HasPrefix(clean, prefix) || clean == prefix || strings.Contains(clean, "../") {
			return Manifest{}, fmt.Errorf("unsafe or unexpected archive path %s", header.Name)
		}
		digest := sha256.New()
		written, err := io.Copy(digest, tr)
		if err != nil {
			return Manifest{}, fmt.Errorf("hash archived %s: %w", header.Name, err)
		}
		if written != header.Size {
			return Manifest{}, fmt.Errorf("archived %s size mismatch: header=%d read=%d", header.Name, header.Size, written)
		}
		manifest.Files = append(manifest.Files, File{
			Path:   strings.TrimPrefix(clean, prefix),
			Size:   written,
			SHA256: hex.EncodeToString(digest.Sum(nil)),
		})
	}
	sort.Slice(manifest.Files, func(i, j int) bool { return manifest.Files[i].Path < manifest.Files[j].Path })
	return manifest, nil
}

func Compare(expected, actual Manifest) []Difference {
	expectedByPath := make(map[string]File, len(expected.Files))
	actualByPath := make(map[string]File, len(actual.Files))
	paths := make(map[string]struct{}, len(expected.Files)+len(actual.Files))
	for _, item := range expected.Files {
		expectedByPath[item.Path] = item
		paths[item.Path] = struct{}{}
	}
	for _, item := range actual.Files {
		actualByPath[item.Path] = item
		paths[item.Path] = struct{}{}
	}
	ordered := make([]string, 0, len(paths))
	for itemPath := range paths {
		ordered = append(ordered, itemPath)
	}
	sort.Strings(ordered)
	var differences []Difference
	for _, itemPath := range ordered {
		expectedItem, expectedFound := expectedByPath[itemPath]
		actualItem, actualFound := actualByPath[itemPath]
		if expectedFound && actualFound && expectedItem.Size == actualItem.Size && expectedItem.SHA256 == actualItem.SHA256 {
			continue
		}
		difference := Difference{Path: itemPath}
		if expectedFound {
			expectedCopy := expectedItem
			difference.Expected = &expectedCopy
		}
		if actualFound {
			actualCopy := actualItem
			difference.Actual = &actualCopy
		}
		differences = append(differences, difference)
	}
	return differences
}

func hashFile(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	digest := sha256.New()
	if _, err := io.Copy(digest, file); err != nil {
		return "", err
	}
	return hex.EncodeToString(digest.Sum(nil)), nil
}
