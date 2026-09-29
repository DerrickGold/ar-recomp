package winbundle

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"maps"
	"os"
	"path/filepath"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
)

// This is a local performance cache, not a security boundary against someone
// who can alter the user's files and restore timestamps. Windows execution
// policy still applies; explicit verification always rehashes all contents.
type fileStamp = host.FileStamp

func stampFile(info fs.FileInfo) fileStamp {
	return host.StampFile(info)
}

type verificationReceipt struct {
	Schema         int                  `json:"schema"`
	BundleID       string               `json:"bundleId"`
	ManifestSHA256 string               `json:"manifestSha256"`
	Package        fileStamp            `json:"package"`
	Files          map[string]fileStamp `json:"files"`
}

func readVerificationReceipt(name, id string, source fileStamp) *verificationReceipt {
	info, err := os.Lstat(name)
	if err != nil || !info.Mode().IsRegular() || info.Size() > 32<<20 {
		return nil
	}
	f, err := os.Open(name)
	if err != nil {
		return nil
	}
	defer f.Close()
	var receipt verificationReceipt
	d := json.NewDecoder(io.LimitReader(f, (32<<20)+1))
	if d.Decode(&receipt) != nil || d.Decode(new(any)) != io.EOF || receipt.Schema != 1 ||
		receipt.BundleID != id || receipt.Package != source || len(receipt.Files) == 0 || len(receipt.ManifestSHA256) != 64 {
		return nil
	}
	return &receipt
}

func (a *Archive) saveVerificationReceipt(ctx context.Context, files map[string]fileStamp) error {
	if a.receiptPath == "" {
		return nil
	}
	// Do not certify a package that changed while startup was reading it.
	info, err := a.file.Stat()
	if err != nil {
		return err
	}
	if stampFile(info) != a.packageStamp {
		return errors.New("Builder package changed during verification")
	}
	if a.receipt != nil && maps.Equal(a.receipt.Files, files) {
		return nil
	}
	receipt := verificationReceipt{1, a.ID, a.manifestSHA256, a.packageStamp, files}
	f, err := os.CreateTemp(filepath.Dir(a.receiptPath), ".verification-")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	err = json.NewEncoder(f).Encode(receipt)
	closeErr := f.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	return os.Rename(f.Name(), a.receiptPath)
}

// Walk directory metadata once without opening each file. On Windows, DirEntry
// supplies size and timestamps from directory enumeration. Hash only files
// whose metadata differs from a prior successful check (or all files when the
// receipt is absent). Fresh extraction already checked every file's contents.
func (a *Archive) checkDirectory(ctx context.Context, directory string, previous map[string]fileStamp, extracted bool, progress ProgressFunc) (map[string]fileStamp, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	newMeter(ctx, progress, "Checking prepared file metadata", 0)
	info, err := os.Lstat(directory)
	if err != nil {
		return nil, err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return nil, errors.New("runtime cache must be a real directory")
	}
	root, err := os.OpenRoot(directory)
	if err != nil {
		return nil, err
	}
	defer root.Close()
	id, err := root.ReadFile(".bundle-id")
	if err != nil || string(id) != a.ID+"\n" {
		return nil, errors.New("unmanaged or mismatched runtime cache")
	}
	expected := make(map[string]int64, len(a.Manifest.Files))
	for _, entry := range a.Manifest.Files {
		expected[entry.Path] = entry.Size
	}
	current := make(map[string]fileStamp, len(expected))
	err = fs.WalkDir(root.FS(), ".", func(name string, d fs.DirEntry, err error) error {
		if err := ctx.Err(); err != nil {
			return err
		}
		if err != nil {
			return err
		}
		if d.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("unexpected cache entry %s", name)
		}
		if d.IsDir() {
			return nil
		}
		info, err := d.Info()
		if err != nil {
			return err
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("invalid cached file %s", name)
		}
		if name == ".bundle-id" {
			return nil
		}
		size, ok := expected[name]
		if !ok {
			return fmt.Errorf("unexpected cache entry %s", name)
		}
		if info.Size() != size {
			return fmt.Errorf("invalid cached file %s", name)
		}
		current[name] = stampFile(info)
		return nil
	})
	if err != nil {
		return nil, err
	}
	var total int64
	for _, entry := range a.Manifest.Files {
		stamp, ok := current[entry.Path]
		if !ok {
			return nil, fmt.Errorf("missing cached file %s", entry.Path)
		}
		old, known := previous[entry.Path]
		if !extracted && (!known || old != stamp) {
			total += entry.Size
		}
	}
	if !extracted {
		stage := "Verifying prepared files"
		if previous != nil {
			stage = "Verifying changed files"
		}
		var m *meter
		for _, entry := range a.Manifest.Files {
			if err := ctx.Err(); err != nil {
				return nil, err
			}
			if old, known := previous[entry.Path]; known && old == current[entry.Path] {
				continue
			}
			if m == nil {
				m = newMeter(ctx, progress, stage, total)
			}
			f, err := root.Open(filepath.FromSlash(entry.Path))
			if err != nil {
				return nil, err
			}
			h := sha256.New()
			n, readErr := m.copy(h, io.LimitReader(f, entry.Size+1))
			info, statErr := f.Stat()
			closeErr := f.Close()
			if err := errors.Join(readErr, statErr, closeErr); err != nil {
				return nil, err
			}
			if n != entry.Size || hex.EncodeToString(h.Sum(nil)) != entry.SHA256 || stampFile(info) != current[entry.Path] {
				return nil, fmt.Errorf("modified runtime cache file: %s", entry.Path)
			}
		}
		if m != nil {
			m.report(true)
		}
	}
	return current, ctx.Err()
}
