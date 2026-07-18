package project

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/DerrickGold/snesrecomp-go/internal/regen"
	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
)

type RegenOptions struct {
	Paths
	Jobs       int
	AllowStubs bool
	RunTests   bool
	GoCommand  string
	Stdout     io.Writer
	Stderr     io.Writer
}

type RegenReport struct {
	Generation  regen.Report
	Stubs       tooling.StubCensusReport
	NewRTSLines []string
}

type StubGateError struct {
	RawMarkers   int
	LogicalSites int
}

func (err *StubGateError) Error() string {
	return fmt.Sprintf("hard stub gate failed: %d raw marker(s), %d logical trap site(s)", err.RawMarkers, err.LogicalSites)
}

func Regenerate(options RegenOptions) (RegenReport, error) {
	paths, err := options.Paths.Resolve()
	if err != nil {
		return RegenReport{}, err
	}
	options.Paths = paths
	if options.Jobs <= 0 {
		options.Jobs = runtime.NumCPU()
	}
	if options.GoCommand == "" {
		options.GoCommand = "go"
	}
	stdout := options.Stdout
	if stdout == nil {
		stdout = io.Discard
	}
	stderr := options.Stderr
	if stderr == nil {
		stderr = io.Discard
	}
	if info, statErr := os.Stat(paths.ROM); statErr != nil || info.IsDir() {
		if statErr == nil {
			statErr = errors.New("path is a directory")
		}
		return RegenReport{}, fmt.Errorf("ROM %s is unavailable: %w", paths.ROM, statErr)
	}

	step(stdout, fmt.Sprintf("Regenerating banks (%d workers)", options.Jobs))
	generation, err := regen.Run(regen.Options{
		ROMPath: paths.ROM, ConfigDir: paths.ConfigDir, OutputDir: paths.GeneratedDir,
		Jobs: options.Jobs, AllowStubs: true,
		Progress: func(format string, values ...any) {
			fmt.Fprintf(stdout, "v2regen: "+format+"\n", values...)
		},
	})
	fmt.Fprintf(stdout, "v2regen: %d banks, %d -> %d variants, %d files (%d changed), %s\n",
		generation.Banks, generation.InitialEntries, generation.FinalEntries,
		generation.Files, generation.ChangedFiles, generation.Elapsed.Round(time.Millisecond))
	if generation.UnresolvedIndirects > 0 {
		fmt.Fprintf(stdout, "v2regen: %d unresolved indirect sites emitted as traps\n", generation.UnresolvedIndirects)
	}
	if generation.StubHits > 0 {
		fmt.Fprintf(stdout, "v2regen: STUB LINT: %d marker(s)\n", generation.StubHits)
	}
	if err != nil {
		return RegenReport{Generation: generation}, err
	}

	step(stdout, "Syncing funcs.h")
	functionCount, err := tooling.SyncFuncs(paths.ConfigDir, paths.FuncsHeader)
	if err != nil {
		return RegenReport{Generation: generation}, err
	}
	fmt.Fprintf(stdout, "sync-funcs: wrote %d function declarations to %s\n", functionCount, displayPath(paths.Root, paths.FuncsHeader))

	step(stdout, "Refreshing generated-code metadata")
	metadataStarted := time.Now()
	metadata, err := tooling.GenerateMetadata(paths.GeneratedDir, paths.ConfigDir, paths.Metadata, metadataStarted)
	if err != nil {
		return RegenReport{Generation: generation}, err
	}
	fmt.Fprintln(stdout, tooling.FormatMetadataReport(metadata, displayPath(paths.Root, paths.Metadata), time.Since(metadataStarted)))

	step(stdout, "RTS-web census")
	newRTSLines, err := refreshRTSReport(paths, stdout)
	if err != nil {
		return RegenReport{Generation: generation}, err
	}

	step(stdout, "Hard stub census")
	stubs, err := tooling.CensusStubs(paths.GeneratedDir, false, stdout)
	if err != nil {
		return RegenReport{Generation: generation, NewRTSLines: newRTSLines}, err
	}
	report := RegenReport{Generation: generation, Stubs: stubs, NewRTSLines: newRTSLines}

	if options.RunTests {
		step(stdout, "Go recomp toolchain tests")
		if err := runGoTests(options.GoCommand, paths.ToolchainDir, stdout, stderr); err != nil {
			return report, err
		}
	}

	logicalSites := stubs.LogicalGotos + stubs.LogicalDispatches
	if !options.AllowStubs && (generation.StubHits > 0 || logicalSites > 0) {
		return report, &StubGateError{RawMarkers: generation.StubHits, LogicalSites: logicalSites}
	}
	step(stdout, "Regeneration complete")
	return report, nil
}

func refreshRTSReport(paths Paths, output io.Writer) ([]string, error) {
	var current bytes.Buffer
	if _, err := tooling.CensusRTSWebs(tooling.RTSCensusOptions{
		ROMPath: paths.ROM, CFGDir: paths.ConfigDir, Output: &current,
	}); err != nil {
		return nil, err
	}
	if err := writeFile(paths.RTSReport, current.Bytes()); err != nil {
		return nil, fmt.Errorf("write RTS census: %w", err)
	}
	currentLines := uncoveredLines(current.Bytes())
	previousData, err := os.ReadFile(paths.RTSPrevious)
	if err != nil && !os.IsNotExist(err) {
		return nil, fmt.Errorf("read previous RTS census: %w", err)
	}
	var newLines []string
	if os.IsNotExist(err) {
		fmt.Fprintf(output, "first census run — baseline written; full UNC list in %s\n", displayPath(paths.Root, paths.RTSReport))
	} else {
		seen := make(map[string]struct{})
		for _, line := range uncoveredLines(previousData) {
			seen[line] = struct{}{}
		}
		for _, line := range currentLines {
			if _, found := seen[line]; !found {
				newLines = append(newLines, line)
			}
		}
		if len(newLines) == 0 {
			fmt.Fprintln(output, "no new uncovered continuations since last regen")
		} else {
			fmt.Fprintln(output, "!! NEW uncovered continuations since last regen (triage before next play):")
			for _, line := range newLines {
				fmt.Fprintf(output, "  %s\n", line)
			}
		}
	}
	if err := writeFile(paths.RTSPrevious, current.Bytes()); err != nil {
		return nil, fmt.Errorf("write previous RTS census: %w", err)
	}
	return newLines, nil
}

func uncoveredLines(data []byte) []string {
	var lines []string
	scanner := bufio.NewScanner(bytes.NewReader(data))
	for scanner.Scan() {
		line := scanner.Text()
		if strings.Contains(line, "[UNC]") {
			lines = append(lines, line)
		}
	}
	return lines
}

func runGoTests(goCommand, toolchainDir string, stdout, stderr io.Writer) error {
	command := exec.Command(goCommand, "test", "./...")
	command.Dir = toolchainDir
	command.Stdout = stdout
	command.Stderr = stderr
	if err := command.Run(); err != nil {
		return fmt.Errorf("Go toolchain tests: %w", err)
	}
	return nil
}

func writeFile(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o644)
}

func displayPath(root, path string) string {
	relative, err := filepath.Rel(root, path)
	if err == nil && relative != "." && !strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
		return filepath.ToSlash(relative)
	}
	return path
}

func step(output io.Writer, title string) {
	fmt.Fprintf(output, "\n=== %s ===\n", title)
}
