package materialize

import (
	"bytes"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/artifact"
	"github.com/DerrickGold/snesrecomp-go/internal/regen"
	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
)

func write(t *testing.T, path string, contents []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, contents, 0o600); err != nil {
		t.Fatal(err)
	}
}

func read(t *testing.T, path string) []byte {
	t.Helper()
	contents, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return contents
}

func fixture(t *testing.T) Options {
	t.Helper()
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image, []byte{0x20, 0x00, 0x81, 0x60}) // JSR $8100; RTS
	image[0x100] = 0x60
	copy(image[0x200:], []byte{0x80, 0x0e}) // BRA $8210: internal continuation
	copy(image[0x210:], []byte{0xea, 0x60})
	copy(image[0x300:], []byte{0x20, 0x00, 0x84, 0x60}) // HLE target must remain authored
	image[0x400] = 0x60
	copy(image[0x500:], []byte{0x20, 0x00, 0x86, 0x60}) // unknown/future metadata stays
	image[0x600] = 0x60
	copy(image[0x1000:], []byte{
		0xa9, 0x1f, 0x90, 0x8d, 0x45, 0x7c, // save handler-minus-one
		0xa9, 0x0f, 0x90, 0x48, 0xad, 0x45, 0x7c, 0x48, 0x60,
		0xea, 0x60, // $9010 continuation
	})
	image[0x1020] = 0x60 // handler resumes $9010
	options := Options{ROMPath: filepath.Join(root, "game.sfc"), ConfigDir: filepath.Join(root, "cfg"), OutputDir: filepath.Join(root, "bundle"), Jobs: 1, AllowStubs: true}
	write(t, options.ROMPath, image)
	write(t, filepath.Join(options.ConfigDir, "bank00.cfg"), []byte(`bank = 00
# preserve comments and names
func Root 8000 entry_mx:1,1
func bank_00_8100 8100 entry_mx:1,1 # retain this annotation
func ResumeOwner 8200 entry_mx:1,1
func bank_00_8210 8210 entry_mx:1,1
func HLEOwner 8300 entry_mx:1,1
func bank_00_8400 8400 entry_mx:1,1
hle_func 8400 HostRoutine
hle_func_if 8410 OtherRoutine ShouldReplace
hle_dispatch 8420 HostDispatch
hle_spc_upload 8430
func FutureOwner 8500 entry_mx:1,1
func bank_00_8600 8600 entry_mx:1,1 future_policy:retain
force_variant_at 008300 1 1
exit_mx_at 008400 1 1
func DispatchRoot 9000 entry_mx:0,0
rts_dispatch 900E 9020
rts_dispatch 9020 9010
`))
	write(t, filepath.Join(options.ConfigDir, "support", "authored.h"), []byte("/* custom support */\r\n"))
	write(t, filepath.Join(options.ConfigDir, "funcs.h"), []byte("/* stale generated header */\n"))
	return options
}

func TestMaterializeWritesVerifiedInputsAndPreservesPolicy(t *testing.T) {
	options := fixture(t)
	before, err := artifact.FromDir(options.ConfigDir)
	if err != nil {
		t.Fatal(err)
	}
	report, err := Run(options)
	if err != nil {
		t.Fatal(err)
	}
	want := map[uint32]string{0x8100: "func", 0x8210: "func", 0x900e: "rts_dispatch", 0x9020: "rts_dispatch"}
	got := make(map[uint32]string)
	for _, removal := range report.Removals {
		got[removal.PC] = removal.Directive
		if len(removal.Evidence) == 0 || removal.Line == 0 || removal.Original == "" {
			t.Fatalf("removal missing provenance: %+v", removal)
		}
	}
	if !reflect.DeepEqual(got, want) || report.PreservedHLEDirectives != 4 {
		t.Fatalf("removals=%v HLE=%d", got, report.PreservedHLEDirectives)
	}
	pruned := string(read(t, filepath.Join(options.OutputDir, "recomp", "bank00.cfg")))
	for _, kept := range []string{"# preserve comments and names", "# retain this annotation", "func Root ", "func bank_00_8400 ", "hle_func 8400 HostRoutine", "hle_func_if 8410 OtherRoutine ShouldReplace", "hle_dispatch 8420 HostDispatch", "hle_spc_upload 8430", "future_policy:retain", "force_variant_at 008300 1 1", "exit_mx_at 008400 1 1"} {
		if !strings.Contains(pruned, kept) {
			t.Errorf("lost authored %q", kept)
		}
	}
	for _, removed := range []string{"func bank_00_8100", "func bank_00_8210", "rts_dispatch"} {
		if strings.Contains(pruned, removed) {
			t.Errorf("retained redundant %q", removed)
		}
	}
	if !bytes.Equal(read(t, filepath.Join(options.ConfigDir, "support", "authored.h")), read(t, filepath.Join(options.OutputDir, "recomp", "support", "authored.h"))) {
		t.Fatal("changed support header")
	}
	if !strings.Contains(string(read(t, filepath.Join(options.OutputDir, "recomp", "funcs.h"))), "bank_00_8100_M1X1") {
		t.Fatal("generated header lost removed canonical declaration")
	}
	after, err := artifact.FromDir(options.ConfigDir)
	if err != nil || !reflect.DeepEqual(before, after) {
		t.Fatalf("authored input changed: %v", err)
	}
	if report.Verified != "full-vs-reduced-byte-identical" || report.GenerationMode != "proven-analysis" {
		t.Fatalf("incorrect verification claim: %+v", report)
	}
	// The written inputs must also work after staging has been deleted.
	db, err := tooling.LoadStaticAnalysisDatabaseFile(filepath.Join(options.OutputDir, "analysis-db.json"), options.ROMPath)
	if err != nil {
		t.Fatal(err)
	}
	fresh := filepath.Join(t.TempDir(), "fresh-gen")
	rebuilt, err := regen.Run(regen.Options{
		ROMPath: options.ROMPath, ConfigDir: filepath.Join(options.OutputDir, "recomp"), OutputDir: fresh, Jobs: 2, AllowStubs: true,
		ProvenDispatchFacts: db.DispatchFacts, ProvenEntryFacts: db.EntryFacts, ProvenEntryTemplates: db.EntryTemplates,
		AllowMatchingAuthoredFacts: true, ExperimentalExactDirectCallMX: true,
	})
	if err != nil || rebuilt.SemanticSourceSHA256 != report.SemanticSourceSHA256 {
		t.Fatalf("rebuild failed: %v", err)
	}
	a, err := artifact.FromDir(fresh)
	if err != nil {
		t.Fatal(err)
	}
	b, err := artifact.FromDir(filepath.Join(options.OutputDir, "gen"))
	if err != nil || !reflect.DeepEqual(a, b) {
		t.Fatalf("published bundle does not reproduce: %v", err)
	}
}

func TestMaterializationIsDeterministicAcrossWorkersAndLocations(t *testing.T) {
	options := fixture(t)
	if _, err := Run(options); err != nil {
		t.Fatal(err)
	}
	first, err := artifact.FromDir(options.OutputDir)
	if err != nil {
		t.Fatal(err)
	}
	options.OutputDir = filepath.Join(t.TempDir(), "other-bundle")
	options.Jobs = 8
	if _, err := Run(options); err != nil {
		t.Fatal(err)
	}
	second, err := artifact.FromDir(options.OutputDir)
	if err != nil || !reflect.DeepEqual(first, second) {
		t.Fatalf("nondeterministic bundle: %v", err)
	}
}

func TestMaterializeRefusesUnsafeDestinationsAndSymlinks(t *testing.T) {
	for _, scenario := range []string{"empty", "in-place", "child", "parent", "src-gen", "existing-empty", "existing-file", "symlink-file", "symlink-directory"} {
		t.Run(scenario, func(t *testing.T) {
			options := fixture(t)
			switch scenario {
			case "empty":
				options.OutputDir = ""
			case "in-place":
				options.OutputDir = options.ConfigDir
			case "child":
				options.OutputDir = filepath.Join(options.ConfigDir, "child")
			case "parent":
				options.OutputDir = filepath.Dir(options.ConfigDir)
			case "src-gen":
				parent := filepath.Join(filepath.Dir(options.OutputDir), "src")
				if err := os.Mkdir(parent, 0o755); err != nil {
					t.Fatal(err)
				}
				options.OutputDir = filepath.Join(parent, "gen")
			case "existing-empty":
				if err := os.Mkdir(options.OutputDir, 0o755); err != nil {
					t.Fatal(err)
				}
			case "existing-file":
				write(t, options.OutputDir, []byte("do not replace"))
			case "symlink-file", "symlink-directory":
				target := options.ROMPath
				if scenario == "symlink-directory" {
					target = filepath.Dir(target)
				}
				if err := os.Symlink(target, filepath.Join(options.ConfigDir, "link")); err != nil {
					t.Skipf("symlink unavailable: %v", err)
				}
			}
			called := false
			_, err := run(options, func(regen.Options) (regen.Report, error) { called = true; return regen.Report{}, nil })
			if err == nil || called {
				t.Fatalf("unsafe input reached generation: called=%v err=%v", called, err)
			}
			if scenario == "existing-file" && string(read(t, options.OutputDir)) != "do not replace" {
				t.Fatal("overwrote existing file")
			}
		})
	}
}

func TestMaterializeRejectsConflictsWithoutPublishing(t *testing.T) {
	options := fixture(t)
	path := filepath.Join(options.ConfigDir, "bank00.cfg")
	write(t, path, bytes.Replace(read(t, path), []byte("rts_dispatch 900E 9020"), []byte("rts_dispatch 900E 9030"), 1))
	if _, err := Run(options); err == nil || !strings.Contains(err.Error(), "conflict") {
		t.Fatalf("expected conflict: %v", err)
	}
	if _, err := os.Lstat(options.OutputDir); !os.IsNotExist(err) {
		t.Fatalf("published conflicting output: %v", err)
	}
}

func TestMaterializeRejectsAnyGeneratedDifference(t *testing.T) {
	for _, kind := range []string{"code", "header", "extra-file", "summary"} {
		t.Run(kind, func(t *testing.T) {
			options := fixture(t)
			calls := 0
			_, err := run(options, func(opts regen.Options) (regen.Report, error) {
				report, err := regen.Run(opts)
				calls++
				if err != nil || calls != 2 {
					return report, err
				}
				switch kind {
				case "code":
					write(t, filepath.Join(opts.OutputDir, "bank00_v2.c"), []byte("/* injected mismatch */\n"))
				case "header":
					// Changes funcs.h only; generation already completed.
					path := filepath.Join(opts.ConfigDir, "bank00.cfg")
					write(t, path, append(read(t, path), []byte("name 018100 HeaderOnly\n")...))
				case "extra-file":
					write(t, filepath.Join(opts.OutputDir, "extra.c"), []byte("/* unexpected */\n"))
				case "summary":
					report.StubHits++
				}
				return report, nil
			})
			if err == nil || !strings.Contains(err.Error(), "materialization rejected") {
				t.Fatalf("expected rejection: %v", err)
			}
			if _, err := os.Lstat(options.OutputDir); !os.IsNotExist(err) {
				t.Fatalf("published mismatching candidate: %v", err)
			}
		})
	}
}

func TestPruneKeepsNonSelectedGuardsAndLineEndings(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "bank00.cfg")
	source := "bank = 00\r\nfunc bank_00_8100 8100 entry_mx:1,0\r\nrts_dispatch 8120 8130 8140 # broader guard\r\nindirect_dispatch 8200 2 idx:X tables:8300\r\n# end"
	write(t, path, []byte(source))
	db := tooling.StaticAnalysisDatabase{EntryFacts: []analysis.EntryFact{{
		PC: 0x8100, EntryMX: analysis.MXState{M: 1, X: 0}, Kind: analysis.EntryRoutine, TemplateFree: true,
	}}}
	removals, _, err := prune(root, db)
	if err != nil || len(removals) != 1 {
		t.Fatalf("removals=%v err=%v", removals, err)
	}
	want := strings.Replace(source, "func bank_00_8100 8100 entry_mx:1,0\r\n", "", 1)
	if string(read(t, path)) != want {
		t.Fatal("changed unselected guards, CRLF, or final unterminated comment")
	}
}

func TestCommandResolvesAllPathsRelativeToRoot(t *testing.T) {
	options := fixture(t)
	var output bytes.Buffer
	if err := RunCommand([]string{"--root", filepath.Dir(options.ROMPath), "--rom", "game.sfc", "--cfg-dir", "cfg", "--out-dir", "bundle", "--allow-stubs", "--jobs", "2"}, &output); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(options.OutputDir, "materialization.json")); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{"removed 2 func, 2 rts_dispatch", "preserved 4 HLE directives", "NOT a comparison with default generation", "keep analysis-db.json"} {
		if !strings.Contains(output.String(), fragment) {
			t.Errorf("missing CLI guidance %q", fragment)
		}
	}
	for _, args := range [][]string{nil, {"--out-dir", "new", "--jobs", "0"}, {"--out-dir", "new", "positional"}, {"--out-dir", "new", "--cfg-dir", ""}} {
		if err := RunCommand(args, &output); err == nil {
			t.Errorf("accepted invalid args %v", args)
		}
	}
}

func TestMaterializeDoesNotReplaceConcurrentDestination(t *testing.T) {
	options := fixture(t)
	_, err := run(options, func(opts regen.Options) (regen.Report, error) {
		report, err := regen.Run(opts)
		if _, statErr := os.Stat(options.OutputDir); os.IsNotExist(statErr) {
			write(t, filepath.Join(options.OutputDir, "someone-elses-file"), []byte("preserve"))
		}
		return report, err
	})
	if err == nil || !strings.Contains(err.Error(), "reserve new output") {
		t.Fatalf("expected exclusive publication failure: %v", err)
	}
	if string(read(t, filepath.Join(options.OutputDir, "someone-elses-file"))) != "preserve" {
		t.Fatal("overwrote concurrently created directory")
	}
	if _, err := os.Stat(filepath.Join(options.OutputDir, "materialization.json")); !os.IsNotExist(err) {
		t.Fatalf("published into concurrent directory: %v", err)
	}
}

func TestMaterializeReplacesExactSelfDelimitedIndirectTable(t *testing.T) {
	options := fixture(t)
	image := read(t, options.ROMPath)
	// A packed handler-minus-one table ends exactly at its earliest handler.
	// This is structural table proof, not a guessed executable-byte scan.
	copy(image[0x2000:], []byte{0xbd, 0x20, 0xa0, 0xa0, 0x08, 0xa0, 0x5a, 0x48, 0x60, 0x60}) // push continuation, then handler
	copy(image[0x2020:], []byte{0x23, 0xa0, 0x24, 0xa0, 0x60, 0x60})
	write(t, options.ROMPath, image)
	path := filepath.Join(options.ConfigDir, "bank00.cfg")
	write(t, path, append(read(t, path), []byte("func PackedDispatcher A000 entry_mx:0,0\nindirect_dispatch A007 2 idx:A tables:A020 ret:A009 transfer:call\n")...))
	report, err := Run(options)
	if err != nil {
		t.Fatal(err)
	}
	for _, removal := range report.Removals {
		if removal.PC == 0xa007 && removal.Directive == "indirect_dispatch" {
			return
		}
	}
	t.Fatalf("did not replace exact table: %+v", report.Removals)
}

func TestMaterializeRetainsCompatibleRTSGuard(t *testing.T) {
	options := fixture(t)
	path := filepath.Join(options.ConfigDir, "bank00.cfg")
	write(t, path, bytes.Replace(read(t, path), []byte("rts_dispatch 900E 9020"), []byte("rts_dispatch 900E 9020 9010 # safety guard"), 1))
	report, err := Run(options)
	if err != nil {
		t.Fatal(err)
	}
	for _, removal := range report.Removals {
		if removal.PC == 0x900e {
			t.Fatal("removed broader compatible guard")
		}
	}
	if !strings.Contains(string(read(t, filepath.Join(options.OutputDir, "recomp", "bank00.cfg"))), "rts_dispatch 900E 9020 9010 # safety guard") {
		t.Fatal("lost guard or annotation")
	}
}

func TestMaterializeKeepsNamedAndSpecialEntryTemplates(t *testing.T) {
	for _, metadata := range []string{"name 008100 Alias\n", "end:8101", "exit_mx:1,1", "entry_s_offset:2"} {
		t.Run(strings.TrimSpace(metadata), func(t *testing.T) {
			options := fixture(t)
			path := filepath.Join(options.ConfigDir, "bank00.cfg")
			source := read(t, path)
			if strings.HasPrefix(metadata, "name ") {
				source = append(source, []byte(metadata)...)
			} else {
				source = bytes.Replace(source, []byte("func bank_00_8100 8100 entry_mx:1,1"), []byte("func bank_00_8100 8100 entry_mx:1,1 "+metadata), 1)
			}
			write(t, path, source)
			report, err := Run(options)
			if err != nil {
				t.Fatal(err)
			}
			for _, removal := range report.Removals {
				if removal.PC == 0x8100 {
					t.Fatalf("removed special template: %+v", removal)
				}
			}
			if !strings.Contains(string(read(t, filepath.Join(options.OutputDir, "recomp", "bank00.cfg"))), strings.TrimSpace(metadata)) {
				t.Fatal("lost authored metadata")
			}
		})
	}
}
