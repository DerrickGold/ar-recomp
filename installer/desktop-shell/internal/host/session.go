package host

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"

	"github.com/DerrickGold/ar-recomp/installer/internal/buildworkspace"
)

// PrepareSession verifies the read-only payload, then creates a marker-only
// workspace. It never copies compiler tools, authored source or SDK files.
func PrepareSession(ctx context.Context, payload, workspace string, progress func(string)) (string, error) {
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
	root, err := os.OpenRoot(payload)
	if err != nil {
		return "", err
	}
	defer root.Close()
	for i, entry := range manifest.Files {
		if i%250 == 0 && progress != nil {
			progress(fmt.Sprintf("Checking bundled inputs: %d / %d files (no workspace copy)", i, len(manifest.Files)))
		}
		if err := ctx.Err(); err != nil {
			return "", err
		}
		info, err := root.Lstat(filepath.FromSlash(entry.Path))
		if err != nil {
			return "", err
		}
		if !info.Mode().IsRegular() || info.Size() != entry.Size {
			return "", fmt.Errorf("invalid bundled file: %s", entry.Path)
		}
		f, err := root.Open(filepath.FromSlash(entry.Path))
		if err != nil {
			return "", err
		}
		hash := sha256.New()
		n, readErr := io.Copy(hash, io.LimitReader(contextReader{ctx, f}, entry.Size+1))
		closeErr := f.Close()
		if readErr != nil {
			return "", readErr
		}
		if closeErr != nil {
			return "", closeErr
		}
		if n != entry.Size || hex.EncodeToString(hash.Sum(nil)) != entry.SHA256 {
			return "", fmt.Errorf("bundled checksum mismatch: %s", entry.Path)
		}
	}
	if err := ctx.Err(); err != nil {
		return "", err
	}
	if err := buildworkspace.Prepare(workspace); err != nil {
		return "", err
	}
	return fmt.Sprintf("%x", sha256.Sum256(data)), nil
}

type contextReader struct {
	ctx context.Context
	in  io.Reader
}

func (r contextReader) Read(p []byte) (int, error) {
	if err := r.ctx.Err(); err != nil {
		return 0, err
	}
	return r.in.Read(p)
}
