package builder

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"mime/multipart"
	"os"
	"path/filepath"
	"runtime"
)

type stagedAsset struct {
	Temporary string
	Final     string
	Relative  string
}

const maxTrackBytes = 256 << 20

func bundledTitleRelativePath() string {
	digest := sha256.Sum256(titleLogoPNG)
	return filepath.ToSlash(filepath.Join("hd", "builder",
		"title-logo-"+hex.EncodeToString(digest[:8])+".png"))
}

func stageBytes(root, relative string, content []byte) (stagedAsset, error) {
	final := filepath.Join(root, "game-assets", filepath.FromSlash(relative))
	directory := filepath.Dir(final)
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return stagedAsset{}, fmt.Errorf("create asset directory: %w", err)
	}
	temporary, err := os.CreateTemp(directory, ".snesbuild-asset-*")
	if err != nil {
		return stagedAsset{}, err
	}
	temporaryPath := temporary.Name()
	failed := true
	defer func() {
		_ = temporary.Close()
		if failed {
			_ = os.Remove(temporaryPath)
		}
	}()
	if err := temporary.Chmod(0o644); err != nil {
		return stagedAsset{}, err
	}
	if _, err := temporary.Write(content); err != nil {
		return stagedAsset{}, err
	}
	if err := temporary.Sync(); err != nil {
		return stagedAsset{}, err
	}
	if err := temporary.Close(); err != nil {
		return stagedAsset{}, err
	}
	failed = false
	return stagedAsset{Temporary: temporaryPath, Final: final, Relative: relative}, nil
}

// stageAudioUpload puts an uploaded file at the path a manifest record ALREADY
// names. That is the same "drop a file with the matching name" workflow the
// manifest header documents, performed through the GUI, and it is how every
// music replacement is installed -- slots and gated variants alike.
func stageAudioUpload(root, target string,
	header *multipart.FileHeader) (stagedAsset, error) {
	content, err := readVorbisUpload(header, filepath.Base(target))
	if err != nil {
		return stagedAsset{}, err
	}
	directory := filepath.Dir(target)
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return stagedAsset{}, fmt.Errorf("create audio asset directory: %w", err)
	}
	temporary, err := os.CreateTemp(directory, ".snesbuild-variant-*")
	if err != nil {
		return stagedAsset{}, err
	}
	temporaryPath := temporary.Name()
	failed := true
	defer func() {
		_ = temporary.Close()
		if failed {
			_ = os.Remove(temporaryPath)
		}
	}()
	if err := temporary.Chmod(0o644); err != nil {
		return stagedAsset{}, err
	}
	if _, err := temporary.Write(content); err != nil {
		return stagedAsset{}, err
	}
	if err := temporary.Sync(); err != nil {
		return stagedAsset{}, err
	}
	if err := temporary.Close(); err != nil {
		return stagedAsset{}, err
	}
	failed = false
	return stagedAsset{Temporary: temporaryPath, Final: target}, nil
}

// readVorbisUpload applies the same bounded read and format sniff the slot
// uploads use. The game performs the real decode when it loads the manifest;
// this only refuses a file that is obviously not what was asked for.
func readVorbisUpload(header *multipart.FileHeader, label string) ([]byte, error) {
	input, err := header.Open()
	if err != nil {
		return nil, err
	}
	defer input.Close()
	content, err := io.ReadAll(io.LimitReader(input, maxTrackBytes+1))
	if err != nil {
		return nil, fmt.Errorf("copy %s: %w", label, err)
	}
	if len(content) == 0 {
		return nil, fmt.Errorf("%s is empty", label)
	}
	if int64(len(content)) > maxTrackBytes {
		return nil, fmt.Errorf("%s exceeds the %d MiB safety limit",
			label, maxTrackBytes>>20)
	}
	head := content[:min(4096, len(content))]
	if !bytes.HasPrefix(head, []byte("OggS")) ||
		!bytes.Contains(head, []byte{0x01, 'v', 'o', 'r', 'b', 'i', 's'}) {
		return nil, fmt.Errorf("%s must be an Ogg Vorbis file", label)
	}
	return content, nil
}

func replaceStagedFile(asset stagedAsset) error {
	if err := os.Rename(asset.Temporary, asset.Final); err == nil {
		return nil
	} else if runtime.GOOS != "windows" {
		return err
	}
	// Windows does not replace an existing file with Rename. Keep the previous
	// file as a backup until its replacement is in place.
	if _, err := os.Stat(asset.Final); err != nil {
		return os.Rename(asset.Temporary, asset.Final)
	}
	backupFile, err := os.CreateTemp(filepath.Dir(asset.Final), ".snesbuild-backup-*")
	if err != nil {
		return err
	}
	backup := backupFile.Name()
	_ = backupFile.Close()
	_ = os.Remove(backup)
	if err := os.Rename(asset.Final, backup); err != nil {
		return err
	}
	if err := os.Rename(asset.Temporary, asset.Final); err != nil {
		_ = os.Rename(backup, asset.Final)
		return err
	}
	_ = os.Remove(backup)
	return nil
}

func writeAtomicFile(path string, content []byte) error {
	mode := os.FileMode(0o644)
	if info, err := os.Stat(path); err == nil && info.Mode().IsRegular() {
		mode = info.Mode().Perm()
	}
	directory := filepath.Dir(path)
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return err
	}
	temporary, err := os.CreateTemp(directory, ".manifest-*")
	if err != nil {
		return err
	}
	asset := stagedAsset{Temporary: temporary.Name(), Final: path}
	defer func() {
		_ = temporary.Close()
		_ = os.Remove(asset.Temporary)
	}()
	if err := temporary.Chmod(mode); err != nil {
		return err
	}
	if _, err := temporary.Write(content); err != nil {
		return err
	}
	if err := temporary.Sync(); err != nil {
		return err
	}
	if err := temporary.Close(); err != nil {
		return err
	}
	return replaceStagedFile(asset)
}
