package project

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// RuntimeArchiveOptions builds the source runner into the same target-keyed
// static-library artifact consumed by source-free hermetic distributions.
type RuntimeArchiveOptions struct {
	RuntimeDir string
	OutputPath string
	ZigPath    string
	Target     string
	Optimize   string
	Jobs       int
	SIMD       bool
	Verbose    bool
	Stdout     io.Writer
}

// runtimeCompileArgs is deliberately independent of a game's manifest. A
// vended runner cannot inherit game-specific defines or include directories;
// its public ABI and private implementation configuration stand on their own.
func runtimeCompileArgs(runtimeDir, zigPath, target, optimize string, simd bool,
	manifest RunnerManifest) []string {
	if optimize == "" {
		optimize = "-O2"
	}
	args := []string{"cc"}
	if target != "" {
		args = append(args, "-target", target)
	}
	simdValue := "0"
	if simd {
		simdValue = "1"
	}
	args = append(args,
		"-std=gnu11", optimize, "-g", "-gno-record-command-line",
		"-fno-common", "-w",
		"-Wno-implicit-function-declaration",
		"-DSNESRECOMP_ENABLE_SIMD="+simdValue,
		"-DSNESRECOMP_TRACE=0",
		"-DSNESRECOMP_WATCHDOG=0",
		"-DSNESRECOMP_REVERSE_DEBUG=0",
		"-fdebug-compilation-dir=snesrecomp-runtime")
	if TargetOS(target) == "windows" {
		// CodeView embeds Clang's expanded command line, including absolute
		// source and toolchain paths that prefix maps cannot rewrite. Suppress
		// only that record; the CodeView symbols and source mappings remain.
		args = append(args, "-gno-codeview-command-line")
	}
	// Keep full symbols in the vended library without exposing the packaging
	// checkout or Zig installation. Mapping their roots also normalizes system
	// header paths recorded through Zig's target libc.
	prefixes := [][2]string{{filepath.Dir(runtimeDir), "snesrecomp-sdk"}}
	if absoluteZig, err := filepath.Abs(zigPath); err == nil {
		prefixes = append(prefixes,
			[2]string{filepath.Dir(absoluteZig), "zig-toolchain"})
	}
	for _, prefix := range prefixes {
		args = append(args,
			"-ffile-prefix-map="+prefix[0]+"="+prefix[1],
			"-fdebug-prefix-map="+prefix[0]+"="+prefix[1])
	}
	for _, include := range manifest.PublicIncludes {
		args = append(args, "-I"+include)
	}
	for _, include := range manifest.PrivateIncludes {
		args = append(args, "-I"+include)
	}
	return args
}

// BuildRuntimeArchive creates a deterministic, reusable runner library. It
// retains an object cache beside the archive so packaging several times does
// not need to rebuild unchanged translation units.
func BuildRuntimeArchive(options RuntimeArchiveOptions) (string, error) {
	if options.RuntimeDir == "" {
		return "", fmt.Errorf("runtime archive build requires a runtime directory")
	}
	runtimeDir, err := filepath.Abs(options.RuntimeDir)
	if err != nil {
		return "", err
	}
	if options.ZigPath == "" {
		return "", fmt.Errorf("runtime archive build requires a Zig toolchain")
	}
	if !filepath.IsAbs(options.ZigPath) {
		options.ZigPath, err = filepath.Abs(options.ZigPath)
		if err != nil {
			return "", err
		}
	}
	if options.Optimize == "" {
		options.Optimize = "-O2"
	}
	if options.Jobs <= 0 {
		options.Jobs = runtime.NumCPU()
	}
	if options.Stdout == nil {
		options.Stdout = io.Discard
	}
	manifest, err := LoadRunnerManifest(runtimeDir)
	if err != nil {
		return "", err
	}
	archiveTarget, err := RuntimeArchiveTarget(options.Target)
	if err != nil {
		return "", err
	}
	outputPath := options.OutputPath
	if outputPath == "" {
		outputPath, err = VendedRuntimeArchivePath(runtimeDir, options.Target)
		if err != nil {
			return "", err
		}
	} else if outputPath, err = filepath.Abs(outputPath); err != nil {
		return "", err
	}
	if err := os.MkdirAll(filepath.Dir(outputPath), 0o755); err != nil {
		return "", err
	}

	compileArgs := runtimeCompileArgs(
		runtimeDir, options.ZigPath, options.Target, options.Optimize,
		options.SIMD, manifest)
	flagsDigest := sha256.Sum256([]byte(options.ZigPath + "\x00" +
		strings.Join(compileArgs, "\x00")))
	flagsHash := hex.EncodeToString(flagsDigest[:])
	objectDir := filepath.Join(filepath.Dir(outputPath), ".runtime-obj")
	if err := os.MkdirAll(objectDir, 0o755); err != nil {
		return "", err
	}
	flagsPath := filepath.Join(objectDir, "flags.sha256")
	previousFlags, _ := os.ReadFile(flagsPath)
	flagsChanged := strings.TrimSpace(string(previousFlags)) != flagsHash
	headerDirs := append([]string(nil), manifest.PublicIncludes...)
	headerDirs = append(headerDirs, manifest.PrivateIncludes...)
	newestHeader := newestHeaderTime(headerDirs, "")

	type compileJob struct {
		source string
		object string
	}
	objects := make([]string, 0, len(manifest.Sources))
	jobs := make([]compileJob, 0, len(manifest.Sources))
	for _, source := range manifest.Sources {
		if _, err := os.Stat(source); err != nil {
			return "", fmt.Errorf("missing runner source %s: %w", source, err)
		}
		object := filepath.Join(objectDir, objectName(runtimeDir, source))
		objects = append(objects, object)
		if flagsChanged || !objectFresh(source, object, newestHeader) {
			jobs = append(jobs, compileJob{source: source, object: object})
		}
	}
	fmt.Fprintf(options.Stdout,
		"runtime archive: target %s, %d translation units (%d cached, %d to compile)\n",
		archiveTarget, len(objects), len(objects)-len(jobs), len(jobs))

	started := time.Now()
	var failed atomic.Bool
	var firstError error
	var errorOnce sync.Once
	semaphore := make(chan struct{}, options.Jobs)
	var waitGroup sync.WaitGroup
	for _, item := range jobs {
		if failed.Load() {
			break
		}
		waitGroup.Add(1)
		semaphore <- struct{}{}
		go func(item compileJob) {
			defer waitGroup.Done()
			defer func() { <-semaphore }()
			if failed.Load() {
				return
			}
			if options.Verbose {
				fmt.Fprintf(options.Stdout, "  cc %s\n", item.source)
			}
			args := append(append([]string(nil), compileArgs...),
				"-c", item.source, "-o", item.object)
			command := exec.Command(options.ZigPath, args...)
			command.Dir = runtimeDir
			output, err := command.CombinedOutput()
			if err != nil {
				failed.Store(true)
				errorOnce.Do(func() {
					firstError = fmt.Errorf("compile %s: %w\n%s", item.source,
						err, strings.TrimSpace(string(output)))
				})
			}
		}(item)
	}
	waitGroup.Wait()
	if firstError != nil {
		return "", firstError
	}
	if err := os.WriteFile(flagsPath, []byte(flagsHash+"\n"), 0o644); err != nil {
		return "", err
	}
	if err := writeObjectArchive(options.ZigPath, TargetOS(options.Target),
		outputPath, objects); err != nil {
		return "", err
	}
	fmt.Fprintf(options.Stdout, "runtime archive: built %s in %.1fs\n",
		outputPath, time.Since(started).Seconds())
	return outputPath, nil
}
