package project

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

// Per-object dependency tracking for the hermetic incremental build.
//
// Every compile runs with -MD and leaves the compiler's Make-format depfile
// beside its object. After a successful compile that depfile becomes a
// dependency record: the identity (size and mtime) of the object, the digest
// of the flags it was built under, and every file the compiler read, each with
// the size and mtime it had when the compile finished. An object is reused
// only while all of that still matches exactly.
//
// Anything less invalidates that one object, never the whole build: a
// missing, unreadable or foreign record; a dependency that is gone or changed
// in either direction (restoring an older copy moves its mtime backwards); an
// object that is no longer the one its record describes. The object and its
// records are removed before each compile, so a compile that fails or is
// interrupted leaves nothing behind that could be reused.
//
// Inputs under the immutable root of a verified bundle (InputID) are covered
// by that digest, which is already part of the flags digest. They are recorded
// root-relative and not stamped, so a relocated bundle keeps reusing its
// objects exactly as it did before records existed.
//
// Known limit, shared with CMake, Ninja and Make: a header created where it
// would shadow one found later on the include path is not seen, because a
// depfile names only the files the compiler actually opened.
// Mutable files are checked by size and mtime, not by content hash: changing
// contents while preserving both attributes is not detected. Clean the affected
// outputs after such edits. Verified immutable bundle inputs use InputID instead.

const dependencyRecordVersion = 1

// bundledDependencyPrefix marks a dependency covered by the immutable input
// digest rather than by its own stamp; the remainder is root-relative.
const bundledDependencyPrefix = "<bundled-inputs>/"

// noDependencyRecord is the ordinary reason for a first or clean build, so the
// build log does not report it once per object.
const noDependencyRecord = "no dependency record"

type dependencyStamp struct {
	Path    string `json:"path"`
	Size    int64  `json:"size,omitempty"`
	ModTime int64  `json:"mtime_ns,omitempty"`
}

type dependencyRecord struct {
	Version       int               `json:"version"`
	Flags         string            `json:"flags"`
	ObjectSize    int64             `json:"object_size"`
	ObjectModTime int64             `json:"object_mtime_ns"`
	Dependencies  []dependencyStamp `json:"dependencies"`
}

// compilerDepfilePath is where the compiler writes its -MF output.
func compilerDepfilePath(object string) string { return object + ".d" }

// dependencyRecordPath holds the validated record derived from that depfile.
func dependencyRecordPath(object string) string { return object + ".deps" }

// depfileArgs asks the compiler for every file it reads, system headers
// included, so a toolchain or SDK update rebuilds what it affects. They are
// per-object arguments and deliberately not part of the flags digest: they do
// not change the object, and an object without a record is rebuilt anyway.
func depfileArgs(object string) []string {
	return []string{"-MD", "-MF", compilerDepfilePath(object)}
}

// removeObjectOutputs deletes an object together with everything describing
// it. The record goes first, so a failure part-way can never leave a record
// that still describes an object.
func removeObjectOutputs(object string) error {
	for _, path := range []string{dependencyRecordPath(object), compilerDepfilePath(object), object} {
		if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
			return err
		}
	}
	return nil
}

// recordObjectDependencies turns a successful compile's depfile into the
// object's dependency record. started is when that compile was launched: a
// dependency modified after it may not be the version the compiler read, so no
// record is written and the object is rebuilt next time. workDir resolves any
// relative path the compiler printed; compiles inherit this process's working
// directory.
func recordObjectDependencies(options HermeticOptions, object, source, flags string, started time.Time, workDir string) error {
	depfile := compilerDepfilePath(object)
	data, err := os.ReadFile(depfile)
	if err != nil {
		return fmt.Errorf("no depfile: %w", err)
	}
	listed, err := parseMakeDepfile(data)
	if err != nil {
		return fmt.Errorf("unreadable depfile %s: %w", depfile, err)
	}
	objectInfo, err := os.Stat(object)
	if err != nil {
		return err
	}
	if objectInfo.Size() == 0 {
		return errors.New("the compiler left an empty object")
	}
	record := dependencyRecord{
		Version:       dependencyRecordVersion,
		Flags:         flags,
		ObjectSize:    objectInfo.Size(),
		ObjectModTime: objectInfo.ModTime().UnixNano(),
	}
	source = filepath.Clean(source)
	namesSource := false
	seen := make(map[string]bool, len(listed))
	for _, path := range listed {
		if !filepath.IsAbs(path) {
			path = filepath.Join(workDir, path)
		}
		path = filepath.Clean(path)
		if seen[path] {
			continue
		}
		seen[path] = true
		if sameDependencyPath(path, source) {
			namesSource = true
		}
		if options.InputID != "" && underImmutableRoot(options.Root, path) {
			relative, relErr := filepath.Rel(options.Root, path)
			if relErr != nil {
				return relErr
			}
			record.Dependencies = append(record.Dependencies,
				dependencyStamp{Path: bundledDependencyPrefix + filepath.ToSlash(relative)})
			continue
		}
		info, statErr := os.Stat(path)
		if statErr != nil {
			return fmt.Errorf("dependency %s: %w", path, statErr)
		}
		if info.ModTime().After(started) {
			return fmt.Errorf("dependency %s changed while it was being compiled", path)
		}
		record.Dependencies = append(record.Dependencies, dependencyStamp{
			Path: path, Size: info.Size(), ModTime: info.ModTime().UnixNano(),
		})
	}
	if !namesSource {
		return fmt.Errorf("depfile %s does not name %s", depfile, source)
	}
	encoded, err := json.Marshal(record)
	if err != nil {
		return err
	}
	return writeFileAtomically(dependencyRecordPath(object), encoded)
}

// objectDependenciesCurrent reports whether object may be reused under flags.
// When it may not, reason says why, for the verbose build log.
func objectDependenciesCurrent(options HermeticOptions, object, flags string) (current bool, reason string) {
	data, err := os.ReadFile(dependencyRecordPath(object))
	if err != nil {
		return false, noDependencyRecord
	}
	var record dependencyRecord
	if err := json.Unmarshal(data, &record); err != nil {
		return false, "its dependency record is unreadable"
	}
	switch {
	case record.Version != dependencyRecordVersion:
		return false, "its dependency record has another format version"
	case record.Flags != flags:
		return false, "it was built with other flags"
	case len(record.Dependencies) == 0:
		return false, "its dependency record names no inputs"
	}
	info, err := os.Stat(object)
	if err != nil || info.Size() == 0 {
		return false, "the object is missing"
	}
	if info.Size() != record.ObjectSize || info.ModTime().UnixNano() != record.ObjectModTime {
		return false, "the object is not the one its record describes"
	}
	for _, dependency := range record.Dependencies {
		if strings.HasPrefix(dependency.Path, bundledDependencyPrefix) {
			if options.InputID == "" {
				return false, "its record relies on a verified bundle digest"
			}
			continue
		}
		info, err := os.Stat(dependency.Path)
		if err != nil {
			return false, dependency.Path + " is gone"
		}
		if info.Size() != dependency.Size || info.ModTime().UnixNano() != dependency.ModTime {
			return false, dependency.Path + " changed"
		}
	}
	return true, ""
}

// sameDependencyPath compares cleaned paths the way the host does: Windows
// paths are case-insensitive, and a compiler may respell a drive letter.
func sameDependencyPath(a, b string) bool {
	if runtime.GOOS == "windows" {
		return strings.EqualFold(a, b)
	}
	return a == b
}

// writeFileAtomically publishes data at path only once it is complete, so an
// interrupted write cannot leave a truncated record behind.
func writeFileAtomically(path string, data []byte) error {
	temporary, err := os.CreateTemp(filepath.Dir(path), "."+filepath.Base(path)+".*")
	if err != nil {
		return err
	}
	name := temporary.Name()
	_, writeErr := temporary.Write(data)
	if closeErr := temporary.Close(); writeErr == nil {
		writeErr = closeErr
	}
	if writeErr == nil {
		writeErr = os.Rename(name, path)
	}
	if writeErr != nil {
		_ = os.Remove(name)
	}
	return writeErr
}

// parseMakeDepfile returns every prerequisite named by a Make-format depfile
// written by clang or zig cc, in order and without duplicates. It accepts both
// spellings clang can produce: GNU Make escapes (backslash-escaped spaces and
// '#', "$$", backslash-newline continuations, other backslashes literal so
// Windows paths survive) and NMake/Jom quoting (-MV). Phony header rules add
// nothing. Anything else is an error: a record must never be built from a
// depfile this parser does not fully understand.
func parseMakeDepfile(data []byte) ([]string, error) {
	var (
		prerequisites []string
		seen          = make(map[string]bool)
		rules         int
		lineTokens    int
		inRule        bool
	)
	endLine := func() error {
		if lineTokens > 0 && !inRule {
			return errors.New("text outside a rule")
		}
		lineTokens, inRule = 0, false
		return nil
	}
	separate := func() error {
		if inRule {
			return errors.New("a second ':' in one rule")
		}
		if lineTokens == 0 {
			return errors.New("a rule without a target")
		}
		inRule = true
		rules++
		return nil
	}
	for i := 0; i < len(data); {
		switch c := data[i]; {
		case c == '\\' && i+1 < len(data) && data[i+1] == '\n':
			i += 2 // continuation
		case c == '\\' && i+2 < len(data) && data[i+1] == '\r' && data[i+2] == '\n':
			i += 3
		case c == '\n':
			if err := endLine(); err != nil {
				return nil, err
			}
			i++
		case c == ' ' || c == '\t' || c == '\r':
			i++
		case c == ':' && (i+1 == len(data) || isDepfileSpace(data[i+1])):
			if err := separate(); err != nil {
				return nil, err
			}
			i++
		default:
			token, next, endsTarget, err := readDepfileToken(data, i)
			if err != nil {
				return nil, err
			}
			i = next
			if inRule && !seen[token] {
				seen[token] = true
				prerequisites = append(prerequisites, token)
			}
			lineTokens++
			if endsTarget {
				if err := separate(); err != nil {
					return nil, err
				}
			}
		}
	}
	if err := endLine(); err != nil {
		return nil, err
	}
	if rules == 0 || len(prerequisites) == 0 {
		return nil, errors.New("no rule names any prerequisite")
	}
	return prerequisites, nil
}

// readDepfileToken reads the file name starting at data[start]. endsTarget
// reports a rule separator written against the name ("a.o: ...").
func readDepfileToken(data []byte, start int) (token string, next int, endsTarget bool, err error) {
	i := start
	if data[i] == '"' {
		end := bytes.IndexByte(data[i+1:], '"')
		if end < 0 {
			return "", 0, false, errors.New("unterminated quoted file name")
		}
		token, i = string(data[i+1:i+1+end]), i+2+end
		if i < len(data) && data[i] == ':' && (i+1 == len(data) || isDepfileSpace(data[i+1])) {
			return token, i + 1, true, nil
		}
		if i < len(data) && !isDepfileSpace(data[i]) {
			return "", 0, false, errors.New("text directly after a quoted file name")
		}
		if token == "" {
			return "", 0, false, errors.New("empty quoted file name")
		}
		return token, i, false, nil
	}
	var name []byte
scan:
	for i < len(data) {
		c := data[i]
		switch {
		case isDepfileSpace(c):
			break scan
		case c == '\\':
			run := 0
			for i+run < len(data) && data[i+run] == '\\' {
				run++
			}
			var after byte
			if i+run < len(data) {
				after = data[i+run]
			}
			switch {
			case after == ' ' && run%2 == 1:
				// 2N+1 backslashes and a space: N backslashes, then a space
				// that belongs to the name.
				name = append(name, bytes.Repeat([]byte{'\\'}, run/2)...)
				name = append(name, ' ')
				i += run + 1
			case after == '#':
				// Clang escapes '#' with one backslash; any before it are
				// literal path separators.
				name = append(name, bytes.Repeat([]byte{'\\'}, run-1)...)
				name = append(name, '#')
				i += run + 1
			case after == '\n' || (after == '\r' && i+run+1 < len(data) && data[i+run+1] == '\n'):
				// The last backslash continues the line and ends this name;
				// the caller consumes it.
				name = append(name, bytes.Repeat([]byte{'\\'}, run-1)...)
				i += run - 1
				break scan
			default:
				// Literal backslashes: Windows separators, and 2N backslashes
				// before a separating space.
				name = append(name, bytes.Repeat([]byte{'\\'}, run)...)
				i += run
			}
		case c == '$' && i+1 < len(data) && data[i+1] == '$':
			name = append(name, '$')
			i += 2
		case c == ':' && (i+1 == len(data) || isDepfileSpace(data[i+1])):
			if len(name) == 0 {
				return "", 0, false, errors.New("a rule without a target")
			}
			return string(name), i + 1, true, nil
		default:
			name = append(name, c)
			i++
		}
	}
	if len(name) == 0 {
		return "", 0, false, errors.New("empty file name")
	}
	return string(name), i, false, nil
}

func isDepfileSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n'
}
