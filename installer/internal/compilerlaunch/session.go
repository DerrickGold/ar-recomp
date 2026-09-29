// Package compilerlaunch accommodates Windows compilers whose child-process
// launches still require a short executable path and working directory.
package compilerlaunch

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"unicode/utf16"
)

// Session belongs to one build attempt. Environment is passed only to that
// build's subprocesses; it never changes the Builder's process environment.
type Session struct {
	Directory   string
	Environment []string
	windows     bool
	temporary   string
	compiler    string
}

func New(scratch string, environment []string) (*Session, error) {
	cache, _ := os.UserCacheDir()
	return newSession(scratch, environment, runtime.GOOS == "windows", []string{os.TempDir(), cache})
}

func newSession(scratch string, environment []string, windows bool, candidates []string) (*Session, error) {
	s := &Session{Directory: scratch, Environment: append([]string(nil), environment...), windows: windows}
	if !windows {
		return s, nil
	}
	physical, err := physicalPath(scratch)
	if err != nil {
		return nil, err
	}
	s.Directory = physical
	for _, key := range []string{"ZIG_LIB_DIR", "SNESBUILD_ZIG"} {
		value := environmentValue(s.Environment, key)
		if value != "" && !filepath.IsAbs(value) && (key == "ZIG_LIB_DIR" || strings.ContainsAny(value, `/\`)) {
			s.Environment = replaceEnvironment(s.Environment, key, filepath.Join(physical, value))
		}
	}
	// Leave room for zig.exe and for CreateProcess's current-directory limit.
	if shortDirectory(physical) {
		return s, nil
	}
	var failures []error
	for _, base := range candidates {
		if base == "" {
			continue
		}
		dir, err := os.MkdirTemp(base, "ar-compiler-")
		if err != nil {
			failures = append(failures, err)
			continue
		}
		physical, err := physicalPath(dir)
		if err == nil && shortDirectory(physical) {
			s.Directory, s.temporary = physical, dir
			return s, nil
		}
		_ = os.Remove(dir)
		if err == nil {
			err = fmt.Errorf("temporary directory resolves to a path longer than the compiler supports: %s", physical)
		}
		failures = append(failures, err)
	}
	return nil, fmt.Errorf("prepare compiler launch directory (TEMP or LOCALAPPDATA must allow a short temporary path): %w", errors.Join(failures...))
}

func shortDirectory(path string) bool { return len(utf16.Encode([]rune(path))) < 240 }

func physicalPath(path string) (string, error) {
	absolute, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	return filepath.EvalSymlinks(absolute)
}

// UseToolchain stages only the executable when necessary. Zig canonicalizes
// its own path and drops the extended-length prefix before spawning Clang, so
// a prefixed invocation, symlink or directory junction does not fix the issue.
// A real copy has a short canonical path; ZIG_LIB_DIR keeps the original SDK.
// The compiler artifact comes from snesbuild's normal toolchain selection,
// including an explicit SNESBUILD_ZIG override.
func (s *Session) UseToolchain(ctx context.Context, path string) (bool, error) {
	if !s.windows {
		return false, nil
	}
	if err := ctx.Err(); err != nil {
		return false, err
	}
	if !filepath.IsAbs(path) {
		if strings.ContainsAny(path, `/\`) {
			path = filepath.Join(s.Directory, path)
		} else {
			resolved, err := exec.LookPath(path)
			if err != nil {
				return false, err
			}
			path = resolved
		}
	}
	physical, err := physicalPath(path)
	if err != nil {
		return false, err
	}
	if len(utf16.Encode([]rune(physical))) < 260 {
		return false, nil
	}
	if s.compiler != "" {
		return false, errors.New("compiler already prepared for this build")
	}
	lib := environmentValue(s.Environment, "ZIG_LIB_DIR")
	if lib == "" {
		lib = filepath.Join(filepath.Dir(physical), "lib")
	} else if !filepath.IsAbs(lib) {
		lib = filepath.Join(s.Directory, lib)
	}
	info, err := os.Stat(lib)
	if err != nil {
		return false, fmt.Errorf("locate bundled Zig libraries: %w", err)
	}
	if !info.IsDir() {
		return false, fmt.Errorf("Zig library path is not a directory: %s", lib)
	}
	staged := filepath.Join(s.Directory, "zig.exe")
	if err := copyCompiler(ctx, physical, staged); err != nil {
		return false, fmt.Errorf("prepare compiler for long Windows paths: %w", err)
	}
	s.compiler = staged
	s.Environment = replaceEnvironment(s.Environment, "SNESBUILD_ZIG", staged)
	s.Environment = replaceEnvironment(s.Environment, "ZIG_LIB_DIR", lib)
	return true, nil
}

func (s *Session) Close() error {
	var errs []error
	if s.compiler != "" {
		errs = append(errs, os.Remove(s.compiler))
		s.compiler = ""
	}
	if s.temporary != "" {
		errs = append(errs, os.RemoveAll(s.temporary))
		s.temporary = ""
	}
	return errors.Join(errs...)
}

func environmentValue(environment []string, name string) string {
	var value string
	for _, entry := range environment {
		key, candidate, _ := strings.Cut(entry, "=")
		if strings.EqualFold(key, name) {
			value = candidate
		}
	}
	return value
}

func replaceEnvironment(environment []string, name, value string) []string {
	result := make([]string, 0, len(environment)+1)
	for _, entry := range environment {
		key, _, _ := strings.Cut(entry, "=")
		if !strings.EqualFold(key, name) {
			result = append(result, entry)
		}
	}
	return append(result, name+"="+value)
}

func copyCompiler(ctx context.Context, source, destination string) (err error) {
	input, err := os.Open(source)
	if err != nil {
		return err
	}
	defer input.Close()
	output, err := os.OpenFile(destination, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0700)
	if err != nil {
		return err
	}
	defer func() {
		err = errors.Join(err, output.Close())
		if err != nil {
			_ = os.Remove(destination)
		}
	}()
	_, err = io.Copy(output, contextReader{ctx, input})
	return err
}

type contextReader struct {
	ctx context.Context
	io.Reader
}

func (r contextReader) Read(buffer []byte) (int, error) {
	if err := r.ctx.Err(); err != nil {
		return 0, err
	}
	return r.Reader.Read(buffer)
}
