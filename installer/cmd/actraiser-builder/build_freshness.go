package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"github.com/DerrickGold/ar-recomp/installer/internal/builder"
	"github.com/DerrickGold/ar-recomp/installer/internal/buildworkspace"
)

const buildRecordName = ".actraiser-build.json"
const buildInputsName = "build-inputs.sha256"

// Content IDs, not version labels or timestamps, determine a match. In
// particular two builds labelled "dev" or "dirty" need not have the same code.
type gameBuildRecord struct {
	Schema  int    `json:"schema"`
	InputID string `json:"inputID"`
	Version string `json:"builderVersion"`
	Binary  string `json:"binary"` // relative to the game folder, so relocation works
	SHA256  string `json:"sha256"`
}

func bundledBuildID(root, verifiedID string) string {
	if buildworkspace.ValidID(verifiedID) {
		return verifiedID
	}
	// Generic Linux archives have no desktop host. Packaging supplies this
	// content stamp; do not hash the compiler/source tree at GUI startup/polling.
	f, err := os.Open(filepath.Join(root, buildInputsName))
	if err != nil {
		return ""
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, 128))
	id := strings.TrimSpace(string(data))
	if err != nil || !buildworkspace.ValidID(id) {
		return ""
	}
	return id
}

// Record only the installed artifact after the entire build/publication has
// succeeded. Failure or cancellation must never label an old game as current.
func recordGameBuild(ctx context.Context, outputDir, binary, inputID, label string) error {
	if inputID == "" {
		return nil
	} // Developer/old generic bundles are unverifiable.
	if !buildworkspace.ValidID(inputID) {
		return errors.New("invalid game build input identity")
	}
	rel, err := relativeGameBinary(outputDir, binary)
	if err != nil || !filepath.IsLocal(rel) || rel == "." {
		return errors.New("built game must be inside its output folder")
	}
	root, err := os.OpenRoot(outputDir)
	if err != nil {
		return err
	}
	defer root.Close()
	digest, err := hashInstalledGame(ctx, root, rel)
	if err != nil {
		return err
	}
	record := gameBuildRecord{Schema: 1, InputID: inputID, Version: label, Binary: filepath.ToSlash(rel), SHA256: digest}
	data, err := json.MarshalIndent(record, "", "  ")
	if err != nil {
		return err
	}
	// Never follow a receipt redirect or remove a previous record to retry a
	// failed rename. Settings, saves, ROM and user assets are not touched.
	if info, err := root.Lstat(buildRecordName); err == nil && !info.Mode().IsRegular() {
		return errors.New("game build record must be a regular file")
	} else if err != nil && !os.IsNotExist(err) {
		return err
	}
	f, err := os.CreateTemp(outputDir, ".actraiser-build-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	defer f.Close()
	if _, err := f.Write(append(data, '\n')); err != nil {
		return err
	}
	if err := f.Sync(); err != nil {
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	return root.Rename(filepath.Base(f.Name()), buildRecordName)
}

func hashInstalledGame(ctx context.Context, root *os.Root, relative string) (string, error) {
	info, err := root.Lstat(relative)
	if err != nil {
		return "", err
	}
	if !info.Mode().IsRegular() {
		return "", errors.New("game build identity requires a regular executable")
	}
	f, err := root.Open(relative)
	if err != nil {
		return "", err
	}
	defer f.Close()
	hash := sha256.New()
	buffer := make([]byte, 128<<10)
	for {
		if err := ctx.Err(); err != nil {
			return "", err
		}
		n, err := f.Read(buffer)
		hash.Write(buffer[:n])
		if err == io.EOF {
			break
		}
		if err != nil {
			return "", err
		}
	}
	after, err := f.Stat()
	if err != nil {
		return "", err
	}
	if !sameBuildFile(info, after) {
		return "", errors.New("game changed while checking its build identity")
	}
	// An atomic replacement leaves the open handle unchanged. Check that its
	// path still names the file we hashed before trusting or recording it.
	current, err := root.Lstat(relative)
	if err != nil {
		return "", err
	}
	if !sameBuildFile(after, current) {
		return "", errors.New("game replaced while checking its build identity")
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func sameBuildFile(a, b os.FileInfo) bool {
	return a != nil && b != nil && os.SameFile(a, b) && a.Size() == b.Size() && a.ModTime() == b.ModTime() && a.Mode() == b.Mode()
}

func relativeGameBinary(outputDir, binary string) (string, error) {
	// Discovery resolves application paths (e.g. /var -> /private/var on
	// macOS). Normalize ancestors without following a redirected executable.
	root, err := filepath.EvalSymlinks(outputDir)
	if err != nil {
		return "", err
	}
	parent, err := filepath.EvalSymlinks(filepath.Dir(binary))
	if err != nil {
		return "", err
	}
	return filepath.Rel(root, filepath.Join(parent, filepath.Base(binary)))
}

// Status polls read only a tiny receipt and stat the executable. Hash it once
// per session/change, not every 500 ms. This is freshness metadata, not a
// security boundary against a player deliberately forging their local files.
type gameBuildChecker struct {
	mu                          sync.Mutex
	outputDir, inputID, version string
	cachedRecord, cachedBinary  string
	cachedInfo                  os.FileInfo
	cached                      builder.BuildFreshness
}

func (c *gameBuildChecker) Check(binary string) builder.BuildFreshness {
	c.mu.Lock()
	defer c.mu.Unlock()
	state := builder.BuildFreshness{State: "unknown", Reason: "missing_record", BuilderVersion: c.version}
	if !buildworkspace.ValidID(c.inputID) {
		state.State, state.Reason = "unavailable", "builder_identity_unavailable"
		return state
	}
	rel, err := relativeGameBinary(c.outputDir, binary)
	if err != nil || !filepath.IsLocal(rel) || rel == "." {
		return state
	}
	root, err := os.OpenRoot(c.outputDir)
	if err != nil {
		return state
	}
	defer root.Close()
	ri, err := root.Lstat(buildRecordName)
	if err != nil {
		return state
	}
	state.Reason = "invalid_record"
	if !ri.Mode().IsRegular() || ri.Size() > 8192 {
		return state
	}
	f, err := root.Open(buildRecordName)
	if err != nil {
		return state
	}
	data, err := io.ReadAll(io.LimitReader(f, 8193))
	f.Close()
	if err != nil || len(data) > 8192 {
		return state
	}
	var record gameBuildRecord
	if json.Unmarshal(data, &record) != nil || record.Schema != 1 || !buildworkspace.ValidID(record.InputID) || !buildworkspace.ValidID(record.SHA256) || len(record.Version) > 256 {
		return state
	}
	state.BuiltVersion = record.Version
	state.Reason = "game_changed"
	if record.Binary != filepath.ToSlash(rel) {
		return state
	}
	info, err := root.Lstat(rel)
	if err != nil || !info.Mode().IsRegular() {
		return state
	}
	if c.cachedRecord == string(data) && c.cachedBinary == rel && sameBuildFile(c.cachedInfo, info) {
		return c.cached
	}
	digest, err := hashInstalledGame(context.Background(), root, rel)
	if err == nil && digest == record.SHA256 {
		state.State, state.Reason = "rebuild", "different_inputs"
		if record.InputID == c.inputID {
			state.State, state.Reason = "current", "matching_inputs"
		}
	}
	// Do not cache transient read failures; a later status request can recover.
	if err == nil {
		c.cachedRecord, c.cachedBinary, c.cachedInfo, c.cached = string(data), rel, info, state
	}
	return state
}

func completeGameBuild(ctx context.Context, root, outputDir, inputID string, result builder.Result, buildErr error) (builder.Result, error) {
	if buildErr != nil {
		return result, buildErr
	}
	if err := recordGameBuild(ctx, outputDir, result.BinaryPath, bundledBuildID(root, inputID), version); err != nil {
		return result, fmt.Errorf("the game was built, but its version record could not be saved; check that the game folder is writable and rebuild: %w", err)
	}
	return result, nil
}
