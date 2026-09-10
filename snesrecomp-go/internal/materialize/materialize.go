// Package materialize turns proven analysis into an isolated, verified build
// input. It never edits the authored configuration or changes generation policy.
package materialize

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/artifact"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/regen"
	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
)

type Options struct {
	ROMPath, ConfigDir, OutputDir string
	Jobs                          int
	AllowStubs                    bool
	Progress                      func(string, ...any)
}

type Removal struct {
	File      string              `json:"file"`
	Line      int                 `json:"line"`
	Directive string              `json:"directive"`
	Original  string              `json:"original"`
	PC        uint32              `json:"pc"`
	EntryMX   *analysis.MXState   `json:"entry_mx,omitempty"`
	EntryKind analysis.EntryKind  `json:"entry_kind,omitempty"`
	Evidence  []analysis.Evidence `json:"evidence"`
}

// Manifest is also the completion marker, published last. Paths are relative
// and all hashes describe contents, not capture times or build-machine paths.
type Manifest struct {
	Version                int               `json:"version"`
	Provenance             string            `json:"provenance"`
	ROM                    tooling.ShadowROM `json:"rom"`
	GenerationMode         string            `json:"generation_mode"`
	Verified               string            `json:"verified"`
	InputFiles             []artifact.File   `json:"input_files"`
	ReducedFiles           []artifact.File   `json:"reduced_files"`
	GeneratedFiles         []artifact.File   `json:"generated_files"`
	DatabaseFile           artifact.File     `json:"database_file"`
	Removals               []Removal         `json:"removals"`
	PreservedHLEDirectives int               `json:"preserved_hle_directives"`
	FinalVariants          int               `json:"final_variants"`
	UnresolvedIndirects    int               `json:"unresolved_indirects"`
	StubMarkers            int               `json:"stub_markers"`
	SemanticSourceSHA256   string            `json:"semantic_source_sha256"`
}

func Run(options Options) (Manifest, error) {
	return run(options, regen.Run)
}

// The generator seam lets tests exercise a failed equivalence gate without
// weakening the production gate or adding a user-visible bypass.
func run(options Options, generate func(regen.Options) (regen.Report, error)) (Manifest, error) {
	var result Manifest
	input, output, err := isolatedPaths(options.ConfigDir, options.OutputDir)
	if err != nil {
		return result, err
	}
	stage, err := os.MkdirTemp(filepath.Dir(output), ".snesrecomp-materialize-")
	if err != nil {
		return result, err
	}
	defer os.RemoveAll(stage) // Only this invocation's freshly allocated staging directory.
	progress := func(message string, values ...any) {
		if options.Progress != nil {
			options.Progress(message, values...)
		}
	}
	controlCfg := filepath.Join(stage, "authored")
	reducedCfg := filepath.Join(stage, "recomp")
	romPath := filepath.Join(stage, "input.sfc")
	if err := copyTree(input, controlCfg); err != nil {
		return result, fmt.Errorf("snapshot configuration: %w", err)
	}
	if err := copyFile(options.ROMPath, romPath); err != nil {
		return result, fmt.Errorf("snapshot ROM: %w", err)
	}
	inputFiles, err := artifact.FromDir(controlCfg)
	if err != nil {
		return result, err
	}
	progress("analyzing immutable input snapshot")
	shadow, err := tooling.AnalyzeAuthoredShadow(tooling.ShadowAnalysisOptions{
		ROMPath: romPath, CFGDir: controlCfg, Jobs: options.Jobs,
	})
	if err != nil {
		return result, err
	}
	database, err := tooling.BuildStaticAnalysisDatabase(shadow)
	if err != nil {
		return result, err
	}
	dbDir := filepath.Join(stage, "database")
	dbPath := filepath.Join(dbDir, "analysis-db.json")
	if err := tooling.WriteStaticAnalysisDatabaseFile(dbPath, database); err != nil {
		return result, err
	}
	// Exercise the persisted schema and ROM check used by subsequent builds.
	database, err = tooling.LoadStaticAnalysisDatabaseFile(dbPath, romPath)
	if err != nil {
		return result, err
	}
	if err := copyTree(controlCfg, reducedCfg); err != nil {
		return result, err
	}
	removals, hleCount, err := prune(reducedCfg, database)
	if err != nil {
		return result, err
	}
	progress("selected %d redundant declaration(s); preserving %d HLE directive(s)", len(removals), hleCount)
	generateOne := func(cfg, out string) (regen.Report, error) {
		report, err := generate(regen.Options{
			ROMPath: romPath, ConfigDir: cfg, OutputDir: out, Jobs: options.Jobs,
			AllowStubs: options.AllowStubs, ProvenDispatchFacts: database.DispatchFacts,
			ProvenEntryFacts: database.EntryFacts, ProvenEntryTemplates: database.EntryTemplates,
			AllowMatchingAuthoredFacts: true, ExperimentalExactDirectCallMX: true,
		})
		if err != nil {
			return report, err
		}
		_, err = tooling.SyncFuncsWithEntryFacts(cfg, filepath.Join(out, "funcs.h"), database.EntryFacts)
		return report, err
	}
	controlGen := filepath.Join(stage, "control-gen")
	candidateGen := filepath.Join(stage, "gen")
	progress("generating full authored control with the proven database")
	control, err := generateOne(controlCfg, controlGen)
	if err != nil {
		return result, fmt.Errorf("control generation: %w", err)
	}
	progress("generating reduced candidate with the same proven database")
	candidate, err := generateOne(reducedCfg, candidateGen)
	if err != nil {
		return result, fmt.Errorf("reduced generation: %w", err)
	}
	generated, err := verify(controlGen, candidateGen, control, candidate)
	if err != nil {
		return result, err
	}
	// funcs.h belongs with the copied configuration as in ordinary projects.
	// It was compared above along with every generated C/header file.
	if err := copyFile(filepath.Join(candidateGen, "funcs.h"), filepath.Join(reducedCfg, "funcs.h")); err != nil {
		return result, err
	}
	if err := os.Remove(filepath.Join(candidateGen, "funcs.h")); err != nil {
		return result, err
	}
	for i := range generated {
		if generated[i].Path == "funcs.h" {
			generated[i].Path = "recomp/funcs.h"
		} else {
			generated[i].Path = "gen/" + generated[i].Path
		}
	}
	sort.Slice(generated, func(i, j int) bool { return generated[i].Path < generated[j].Path })
	reduced, err := artifact.FromDir(reducedCfg)
	if err != nil {
		return result, err
	}
	dbManifest, err := artifact.FromDir(dbDir)
	if err != nil {
		return result, err
	}
	result = Manifest{
		Version: 1, Provenance: "snesrecomp-verified-materialization-v1", ROM: database.ROM,
		GenerationMode: "proven-analysis", Verified: "full-vs-reduced-byte-identical",
		InputFiles: inputFiles.Files, ReducedFiles: reduced.Files, GeneratedFiles: generated,
		DatabaseFile: dbManifest.Files[0], Removals: removals, PreservedHLEDirectives: hleCount,
		FinalVariants: candidate.FinalEntries, UnresolvedIndirects: candidate.UnresolvedIndirects,
		StubMarkers: candidate.StubHits, SemanticSourceSHA256: candidate.SemanticSourceSHA256,
	}
	manifestBytes, err := json.MarshalIndent(result, "", "  ")
	if err != nil {
		return Manifest{}, err
	}
	manifestPath := filepath.Join(stage, "materialization.json")
	if err := os.WriteFile(manifestPath, append(manifestBytes, '\n'), 0o644); err != nil {
		return Manifest{}, err
	}
	// Reserve with exclusive mkdir after verification. Never rename over an
	// existing directory (even an empty one), including a concurrent creator.
	if err := os.Mkdir(output, 0o755); err != nil {
		return Manifest{}, fmt.Errorf("reserve new output directory: %w", err)
	}
	for _, item := range []struct{ source, name string }{
		{reducedCfg, "recomp"}, {candidateGen, "gen"}, {dbPath, "analysis-db.json"},
		{manifestPath, "materialization.json"}, // Atomic completion marker, always last.
	} {
		if err := os.Rename(item.source, filepath.Join(output, item.name)); err != nil {
			return Manifest{}, fmt.Errorf("publish to %s (incomplete: no materialization.json): %w", output, err)
		}
	}
	progress("verified every generated file and funcs.h; wrote %s", output)
	return result, nil
}

func verify(controlDir, candidateDir string, control, candidate regen.Report) ([]artifact.File, error) {
	left, err := artifact.FromDir(controlDir)
	if err != nil {
		return nil, err
	}
	right, err := artifact.FromDir(candidateDir)
	if err != nil {
		return nil, err
	}
	if differences := artifact.Compare(left, right); len(differences) != 0 {
		return nil, fmt.Errorf("materialization rejected: %d generated file(s) differ, first: %s; no output published", len(differences), differences[0].Path)
	}
	// Hash manifests are useful evidence; also compare actual bytes at the gate.
	for _, file := range left.Files {
		a, err := os.ReadFile(filepath.Join(controlDir, filepath.FromSlash(file.Path)))
		if err != nil {
			return nil, err
		}
		b, err := os.ReadFile(filepath.Join(candidateDir, filepath.FromSlash(file.Path)))
		if err != nil {
			return nil, err
		}
		if !bytes.Equal(a, b) {
			return nil, fmt.Errorf("materialization rejected: generated %s differs; no output published", file.Path)
		}
	}
	if control.SemanticSourceSHA256 == "" || control.SemanticSourceSHA256 != candidate.SemanticSourceSHA256 ||
		control.FinalEntries != candidate.FinalEntries || control.UnresolvedIndirects != candidate.UnresolvedIndirects || control.StubHits != candidate.StubHits {
		return nil, fmt.Errorf("materialization rejected: generation summaries differ; no output published")
	}
	return right.Files, nil
}

func isolatedPaths(input, output string) (string, string, error) {
	if strings.TrimSpace(input) == "" || strings.TrimSpace(output) == "" {
		return "", "", fmt.Errorf("materialize requires --cfg-dir and an explicit new --out-dir")
	}
	input, err := filepath.Abs(input)
	if err != nil {
		return "", "", err
	}
	input, err = filepath.EvalSymlinks(input)
	if err != nil {
		return "", "", err
	}
	output, err = filepath.Abs(output)
	if err != nil {
		return "", "", err
	}
	parent, err := filepath.EvalSymlinks(filepath.Dir(output))
	if err != nil {
		return "", "", fmt.Errorf("output parent must already exist: %w", err)
	}
	output = filepath.Join(parent, filepath.Base(output))
	if strings.EqualFold(filepath.Base(output), "gen") && strings.EqualFold(filepath.Base(parent), "src") {
		return "", "", fmt.Errorf("materialize requires an isolated bundle directory; refusing src/gen")
	}
	within := func(root, path string) bool {
		rel, err := filepath.Rel(root, path)
		return err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
	}
	if within(input, output) || within(output, input) {
		return "", "", fmt.Errorf("materialize input and output directories must not overlap")
	}
	if _, err := os.Lstat(output); err == nil {
		return "", "", fmt.Errorf("materialize refuses existing output %s", output)
	} else if !os.IsNotExist(err) {
		return "", "", err
	}
	return input, output, nil
}

func copyTree(source, destination string) error {
	return filepath.WalkDir(source, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		relative, err := filepath.Rel(source, path)
		if err != nil {
			return err
		}
		target := filepath.Join(destination, relative)
		if entry.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		return copyFile(path, target)
	})
}

func copyFile(source, destination string) error {
	info, err := os.Lstat(source)
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("refusing symlink or special file %s", source)
	}
	input, err := os.Open(source)
	if err != nil {
		return err
	}
	defer input.Close()
	output, err := os.OpenFile(destination, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(output, input)
	closeErr := output.Close()
	if copyErr != nil {
		return copyErr
	}
	return closeErr
}

func prune(directory string, database tooling.StaticAnalysisDatabase) ([]Removal, int, error) {
	entries := make(map[analysis.EntryVariant]analysis.EntryFact)
	for _, fact := range database.EntryFacts {
		if fact.TemplateFree && (fact.Kind == analysis.EntryRoutine || fact.Kind == analysis.EntryContinuation) {
			entries[analysis.EntryVariant{PC: fact.PC, EntryMX: fact.EntryMX}] = fact
		}
	}
	dispatches := make(map[uint32]analysis.DispatchFact)
	for _, fact := range database.DispatchFacts {
		dispatches[fact.SitePC] = fact
	}
	paths, err := filepath.Glob(filepath.Join(directory, "bank*.cfg"))
	if err != nil {
		return nil, 0, err
	}
	removals := make([]Removal, 0)
	hleCount := 0
	for _, path := range paths {
		cfg, err := config.Load(path)
		if err != nil {
			return nil, 0, err
		}
		namedAddresses := make(map[uint32]bool, len(cfg.Names))
		for _, name := range cfg.Names {
			namedAddresses[name.Address] = true
		}
		source, err := os.ReadFile(path)
		if err != nil {
			return nil, 0, err
		}
		var out strings.Builder
		entryIndex, rtsIndex, indirectIndex := 0, 0, 0
		for lineIndex, line := range strings.SplitAfter(string(source), "\n") {
			code, comment, hasComment := strings.Cut(line, "#")
			fields := strings.Fields(code)
			var removal *Removal
			if len(fields) != 0 {
				if strings.HasPrefix(fields[0], "hle_") {
					hleCount++
				}
				var pc uint32
				switch fields[0] {
				case "func":
					if len(fields) >= 3 {
						entry := cfg.Entries[entryIndex]
						entryIndex++
						pc = uint32(cfg.Bank)<<16 | uint32(entry.Start)
						mx := analysis.MXState{M: entry.EntryMX.M, X: entry.EntryMX.X}
						fact, found := entries[analysis.EntryVariant{PC: pc, EntryMX: mx}]
						// Unknown/future options and even currently inert metadata stay
						// authored. A schema extension must explicitly teach this writer.
						// Removing a func can make a name directive auto-promote
						// a different template/order. Keep that authored boundary.
						plain := !namedAddresses[pc] && entry.Name == fmt.Sprintf("bank_%02X_%04X", cfg.Bank, entry.Start)
						for _, option := range fields[3:] {
							plain = plain && (option == "entry_mx:0,0" || option == "entry_mx:0,1" || option == "entry_mx:1,0" || option == "entry_mx:1,1")
						}
						if found && plain {
							removal = &Removal{PC: pc, EntryMX: &mx, EntryKind: fact.Kind, Evidence: fact.Evidence}
						}
					}
				case "rts_dispatch":
					pc = uint32(cfg.Bank)<<16 | uint32(cfg.RTSDispatch[rtsIndex].SitePC)
					rtsIndex++
				case "indirect_dispatch":
					pc = uint32(cfg.Bank)<<16 | uint32(cfg.IndirectDispatch[indirectIndex].SitePC)
					indirectIndex++
				}
				if fields[0] == "rts_dispatch" || fields[0] == "indirect_dispatch" {
					if fact, found := dispatches[pc]; found {
						// The database selector accepts authored sites only on exact
						// independent matches; generation revalidates both forms.
						removal = &Removal{PC: pc, Evidence: fact.Evidence}
					}
				}
			}
			if removal == nil {
				out.WriteString(line)
				continue
			}
			removal.File, removal.Line = filepath.Base(path), lineIndex+1
			removal.Directive, removal.Original = fields[0], strings.TrimRight(line, "\r\n")
			removals = append(removals, *removal)
			if hasComment {
				out.WriteString(code[:len(code)-len(strings.TrimLeft(code, " \t"))])
				out.WriteString("#" + comment)
			}
		}
		if out.String() != string(source) {
			if err := os.WriteFile(path, []byte(out.String()), 0o644); err != nil {
				return nil, 0, err
			}
		}
	}
	return removals, hleCount, nil
}
