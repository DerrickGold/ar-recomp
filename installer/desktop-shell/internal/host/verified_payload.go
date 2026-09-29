package host

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
)

// VerifiedPayload carries a completed integrity check between startup stages in
// this process. It is bound to one directory and manifest. The persistent
// metadata cache is checked before creating this result on a later launch.
type VerifiedPayload struct {
	directory string
	inputID   string
}

// PayloadFromVerifiedFiles accepts an inventory the caller has verified directly
// or via unchanged-file metadata, including builder-payload.json. Windows uses
// its extraction/cache verification here. Compare both manifests so an inconsistent inner manifest
// cannot skip a check, even when the outer bundle itself is internally valid.
func PayloadFromVerifiedFiles(ctx context.Context, payload string, files []File) (*VerifiedPayload, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	directory, err := filepath.Abs(payload)
	if err != nil {
		return nil, err
	}
	data, err := os.ReadFile(filepath.Join(directory, manifestName))
	if err != nil {
		return nil, err
	}
	id := sha256.Sum256(data)
	inputID := hex.EncodeToString(id[:])
	checked := make(map[string]File, len(files))
	for _, file := range files {
		if _, exists := checked[file.Path]; exists {
			return nil, fmt.Errorf("duplicate verified payload file: %s", file.Path)
		}
		checked[file.Path] = file
	}
	entry, ok := checked[manifestName]
	if !ok || entry.Size != int64(len(data)) || entry.SHA256 != inputID {
		return nil, fmt.Errorf("payload manifest differs from verified bundle")
	}
	var manifest Manifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return nil, err
	}
	if err := validateManifest(manifest); err != nil {
		return nil, err
	}
	if len(checked) != len(manifest.Files)+1 {
		return nil, fmt.Errorf("payload inventory differs from verified bundle")
	}
	for _, file := range manifest.Files {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		if entry, ok := checked[file.Path]; !ok || entry != file {
			return nil, fmt.Errorf("payload entry differs from verified bundle: %s", file.Path)
		}
	}
	return &VerifiedPayload{directory: directory, inputID: inputID}, nil
}

// PrepareSession reuses this launch's verification. The small manifest is read
// again to confirm identity; platform, path separation, cancellation and workspace
// ownership checks remain the same as for a fully scanned payload.
func (v *VerifiedPayload) PrepareSession(ctx context.Context, payload, workspace string) (string, error) {
	if v == nil || v.directory == "" || v.inputID == "" {
		return "", fmt.Errorf("missing payload verification")
	}
	return prepareSession(ctx, payload, workspace, nil, v)
}
