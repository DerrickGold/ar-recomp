package winbundle

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
)

// PrepareDirectoryContext verifies new/changed files and reuses unchanged file
// checks from a successful prior launch. The caller holds the runtime-cache lock.
func (a *Archive) PrepareDirectoryContext(ctx context.Context, directory string, prepare func(string) error, progress ProgressFunc) (*host.VerifiedPayload, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	extracted := false
	if _, err := os.Lstat(directory); os.IsNotExist(err) {
		if err := a.ExtractContext(ctx, directory, prepare, progress); err != nil {
			return nil, fmt.Errorf("prepare bundled tools/WebView2: %w", err)
		}
		extracted = true
	} else if err != nil {
		return nil, err
	}
	var previous map[string]fileStamp
	if a.receipt != nil {
		previous = a.receipt.Files
	}
	stamps, err := a.checkDirectory(ctx, directory, previous, extracted, progress)
	if err != nil {
		return nil, fmt.Errorf("runtime cache is incomplete or modified; choose a new workspace (existing files were not changed): %w", err)
	}
	var files []host.File
	for _, entry := range a.Manifest.Files {
		if name, ok := strings.CutPrefix(entry.Path, "payload/"); ok {
			entry.Path = name
			files = append(files, entry)
		}
	}
	verified, err := host.PayloadFromVerifiedFiles(ctx, filepath.Join(directory, "payload"), files)
	if err != nil {
		return nil, err
	}
	if err := a.saveVerificationReceipt(ctx, stamps); err != nil && progress != nil {
		progress(Progress{Stage: "Startup verification could not be saved; it will run again next time"})
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return verified, nil
}
