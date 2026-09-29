package host

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"maps"
	"os"
	"path/filepath"
	"runtime"

	"github.com/DerrickGold/ar-recomp/installer/internal/buildworkspace"
)

// FileStamp is a portable integrity-cache hint, not evidence against deliberate
// tampering that also restores timestamps. Full verification ignores these hints.
type FileStamp struct {
	Size     int64  `json:"size"`
	Modified int64  `json:"modified"`
	Mode     uint32 `json:"mode"`
}

func StampFile(info fs.FileInfo) FileStamp {
	return FileStamp{info.Size(), info.ModTime().UnixNano(), uint32(info.Mode())}
}

type payloadReceipt struct {
	Schema  int                  `json:"schema"`
	InputID string               `json:"inputId"`
	Files   map[string]FileStamp `json:"files"`
}

// PrepareCachedSession is for packaged macOS/Linux payloads. Explicit development
// trees retain PrepareSession's full verification. The manifest digest survives
// portable moves and AppImage remounts; absolute mount paths are not cache keys.
func PrepareCachedSession(ctx context.Context, payload, workspace string, force bool, progress func(string)) (string, error) {
	if err := ctx.Err(); err != nil {
		return "", err
	}
	if err := buildworkspace.Separate(payload, workspace); err != nil {
		return "", err
	}
	data, err := os.ReadFile(filepath.Join(payload, manifestName))
	if err != nil {
		return "", err
	}
	var manifest Manifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return "", err
	}
	if err := validateManifest(manifest); err != nil {
		return "", err
	}
	if manifest.OS != runtime.GOOS || manifest.Arch != runtime.GOARCH {
		return "", fmt.Errorf("payload is %s/%s, host is %s/%s", manifest.OS, manifest.Arch, runtime.GOOS, runtime.GOARCH)
	}
	id := fmt.Sprintf("%x", sha256.Sum256(data))
	name := filepath.Join(workspace, ".builder-verification.json")
	var previous map[string]FileStamp
	if !force {
		previous = readPayloadReceipt(name, id)
	}
	files, err := checkSessionFiles(ctx, payload, manifest, progress, previous)
	if err != nil {
		return "", err
	}
	if err := buildworkspace.Prepare(workspace); err != nil {
		return "", err
	}
	if previous == nil || !maps.Equal(previous, files) {
		if err := savePayloadReceipt(ctx, name, payloadReceipt{1, id, files}); err != nil && progress != nil {
			progress("Startup verification could not be saved; it will run again next time")
		}
	}
	return id, ctx.Err()
}

func readPayloadReceipt(name, id string) map[string]FileStamp {
	info, err := os.Lstat(name)
	if err != nil || !info.Mode().IsRegular() || info.Size() > 32<<20 {
		return nil
	}
	f, err := os.Open(name)
	if err != nil {
		return nil
	}
	defer f.Close()
	var receipt payloadReceipt
	d := json.NewDecoder(io.LimitReader(f, (32<<20)+1))
	if d.Decode(&receipt) != nil || d.Decode(new(any)) != io.EOF || receipt.Schema != 1 || receipt.InputID != id {
		return nil
	}
	return receipt.Files
}

func savePayloadReceipt(ctx context.Context, name string, receipt payloadReceipt) error {
	f, err := os.CreateTemp(filepath.Dir(name), ".verification-")
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
	return os.Rename(f.Name(), name)
}
