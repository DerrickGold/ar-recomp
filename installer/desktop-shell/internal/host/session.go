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
	return prepareSession(ctx, payload, workspace, progress, nil)
}

func prepareSession(ctx context.Context, payload, workspace string, progress func(string), verified *VerifiedPayload) (string, error) {
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
	inputID := fmt.Sprintf("%x", sha256.Sum256(data))
	if verified != nil {
		directory, err := filepath.Abs(payload)
		if err != nil {
			return "", err
		}
		if directory != verified.directory || inputID != verified.inputID {
			return "", fmt.Errorf("payload does not match this launch's verification")
		}
	} else if err := verifySessionFiles(ctx, payload, manifest, progress); err != nil {
		return "", err
	}
	if err := ctx.Err(); err != nil {
		return "", err
	}
	if err := buildworkspace.Prepare(workspace); err != nil {
		return "", err
	}
	return inputID, nil
}

func verifySessionFiles(ctx context.Context, payload string, manifest Manifest, progress func(string)) error {
	_, err := checkSessionFiles(ctx, payload, manifest, progress, nil)
	return err
}

func checkSessionFiles(ctx context.Context, payload string, manifest Manifest, progress func(string), previous map[string]FileStamp) (map[string]FileStamp, error) {
	root, err := os.OpenRoot(payload)
	if err != nil {
		return nil, err
	}
	defer root.Close()
	current := make(map[string]FileStamp, len(manifest.Files))
	hashing := false
	for i, entry := range manifest.Files {
		if i%250 == 0 && progress != nil {
			progress(fmt.Sprintf("Checking bundled inputs: %d / %d files (no workspace copy)", i, len(manifest.Files)))
		}
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		info, err := root.Lstat(filepath.FromSlash(entry.Path))
		if err != nil {
			return nil, err
		}
		if !info.Mode().IsRegular() || info.Size() != entry.Size {
			return nil, fmt.Errorf("invalid bundled file: %s", entry.Path)
		}
		stamp := StampFile(info)
		current[entry.Path] = stamp
		if old, ok := previous[entry.Path]; ok && old == stamp {
			continue
		}
		if !hashing && progress != nil {
			progress("Verifying bundled file contents…")
		}
		hashing = true
		f, err := root.Open(filepath.FromSlash(entry.Path))
		if err != nil {
			return nil, err
		}
		hash := sha256.New()
		n, readErr := io.Copy(hash, io.LimitReader(contextReader{ctx, f}, entry.Size+1))
		after, statErr := f.Stat()
		closeErr := f.Close()
		if readErr != nil {
			return nil, readErr
		}
		if closeErr != nil {
			return nil, closeErr
		}
		if statErr != nil {
			return nil, statErr
		}
		if StampFile(after) != stamp {
			return nil, fmt.Errorf("bundled file changed during verification: %s", entry.Path)
		}
		if n != entry.Size || hex.EncodeToString(hash.Sum(nil)) != entry.SHA256 {
			return nil, fmt.Errorf("bundled checksum mismatch: %s", entry.Path)
		}
	}
	return current, ctx.Err()
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
