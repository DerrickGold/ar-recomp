package project

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
)

func TestPathsResolveRelativeToRoot(t *testing.T) {
	root := t.TempDir()
	paths, err := DefaultPaths(root).Resolve()
	if err != nil {
		t.Fatal(err)
	}
	if paths.ROM != filepath.Join(root, "game.sfc") || paths.GeneratedDir != filepath.Join(root, "src", "gen") {
		t.Fatalf("unexpected resolved paths: %+v", paths)
	}
}

func TestRefreshRTSReportFindsNewUncoveredLines(t *testing.T) {
	root := t.TempDir()
	paths := DefaultPaths(root)
	paths.ROM = "test.sfc"
	image := make([]byte, 0x8000)
	image[0], image[1], image[2], image[3] = 0xA9, 0x0F, 0x80, 0x48
	image[0x10], image[0x11], image[0x12] = 0xEA, 0xEA, 0x60
	if err := os.WriteFile(filepath.Join(root, paths.ROM), image, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(root, paths.ConfigDir), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, paths.ConfigDir, "bank00.cfg"), []byte("bank = 00\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	resolved, err := paths.Resolve()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(resolved.RTSPrevious), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(resolved.RTSPrevious, []byte("old census\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	lines, err := refreshRTSReport(resolved, &output)
	if err != nil {
		t.Fatal(err)
	}
	if len(lines) != 1 || !strings.Contains(lines[0], "push @00:8000") {
		t.Fatalf("unexpected delta: %q\n%s", lines, output.String())
	}
	for _, path := range []string{resolved.RTSReport, resolved.RTSPrevious} {
		if _, err := os.Stat(path); err != nil {
			t.Fatalf("missing report %s: %v", path, err)
		}
	}
}

func TestStubGateError(t *testing.T) {
	err := (&StubGateError{RawMarkers: 3, LogicalSites: 2}).Error()
	if !strings.Contains(err, "3 raw") || !strings.Contains(err, "2 logical") {
		t.Fatalf("unexpected error: %s", err)
	}
}

func TestRegenerateSyntheticProject(t *testing.T) {
	root := t.TempDir()
	paths := DefaultPaths(root)
	image := make([]byte, 0x8000)
	image[0] = 0x60 // RTS at $00:8000.
	if err := os.WriteFile(filepath.Join(root, "game.sfc"), image, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(root, "recomp"), 0o755); err != nil {
		t.Fatal(err)
	}
	config := "bank = 00\nfunc bank_00_8000 8000 entry_mx:1,1\n"
	if err := os.WriteFile(filepath.Join(root, "recomp", "bank00.cfg"), []byte(config), 0o644); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	report, err := Regenerate(RegenOptions{Paths: paths, Jobs: 2, Stdout: &output, Stderr: &output})
	if err != nil {
		t.Fatalf("regenerate: %v\n%s", err, output.String())
	}
	if report.Generation.Banks != 1 || report.Generation.FinalEntries != 1 {
		t.Fatalf("unexpected generation report: %+v", report.Generation)
	}
	for _, path := range []string{
		filepath.Join(root, "src", "gen", "bank00_v2.c"),
		filepath.Join(root, "recomp", "funcs.h"),
		filepath.Join(root, "saves", "gen_meta.json"),
		filepath.Join(root, "saves", "rts_webs.txt"),
	} {
		if _, err := os.Stat(path); err != nil {
			t.Fatalf("missing generated output %s: %v", path, err)
		}
	}
}

func TestRegenerateConsumesAnalysisDatabaseAfterCanonicalEntryPruned(t *testing.T) {
	root := t.TempDir()
	paths := DefaultPaths(root)
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x80, 0x0E}) // $8000: BRA $8010
	copy(image[0x0010:], []byte{0xEA, 0x60}) // $8010: NOP; RTS
	if err := os.WriteFile(filepath.Join(root, "game.sfc"), image, 0o644); err != nil {
		t.Fatal(err)
	}
	configDir := filepath.Join(root, "recomp")
	if err := os.MkdirAll(configDir, 0o755); err != nil {
		t.Fatal(err)
	}
	configPath := filepath.Join(configDir, "bank00.cfg")
	if err := os.WriteFile(configPath, []byte(
		"bank = 00\nfunc Root 8000 entry_mx:1,1\nfunc bank_00_8010 8010 entry_mx:1,1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	shadow, err := tooling.AnalyzeAuthoredShadow(tooling.ShadowAnalysisOptions{
		ROMPath: filepath.Join(root, "game.sfc"), CFGDir: configDir, Jobs: 1,
	})
	if err != nil {
		t.Fatal(err)
	}
	database, err := tooling.BuildStaticAnalysisDatabase(shadow)
	if err != nil {
		t.Fatal(err)
	}
	if len(database.EntryFacts) != 1 {
		t.Fatalf("analysis database entry facts = %+v, want one continuation", database.EntryFacts)
	}
	databasePath := filepath.Join(root, "saves", "static-analysis.json")
	if err := tooling.WriteStaticAnalysisDatabaseFile(databasePath, database); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(configPath, []byte("bank = 00\nfunc Root 8000 entry_mx:1,1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	report, err := Regenerate(RegenOptions{
		Paths: paths, AnalysisDBPath: "saves/static-analysis.json", Jobs: 1,
		Stdout: &output, Stderr: &output,
	})
	if err != nil {
		t.Fatalf("regenerate with analysis database: %v\n%s", err, output.String())
	}
	if report.Generation.AnalysisEntryTemplatesSeeded != 1 || report.Generation.FinalEntries != 2 {
		t.Fatalf("analysis templates/final entries = %d/%d, want 1/2",
			report.Generation.AnalysisEntryTemplatesSeeded, report.Generation.FinalEntries)
	}
	funcs, err := os.ReadFile(filepath.Join(root, "recomp", "funcs.h"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(funcs), "bank_00_8010_M1X1") {
		t.Fatalf("funcs.h omitted database-supplied continuation:\n%s", funcs)
	}
}

func TestRegenerateZeroFactDatabaseStillEnablesExactStaticDiscovery(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	copy(image[0x0000:], []byte{0x20, 0x00, 0x81, 0x60}) // JSR $8100; RTS.
	image[0x0100] = 0x60                                 // Callee RTS.
	romPath := filepath.Join(root, "game.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	configDir := filepath.Join(root, "recomp")
	if err := os.MkdirAll(configDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(configDir, "bank00.cfg"),
		[]byte("bank = 00\nfunc Root 8000 entry_mx:1,1\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	shadow, err := tooling.AnalyzeAuthoredShadow(tooling.ShadowAnalysisOptions{
		ROMPath: romPath, CFGDir: configDir, Jobs: 1,
	})
	if err != nil {
		t.Fatal(err)
	}
	database, err := tooling.BuildStaticAnalysisDatabase(shadow)
	if err != nil {
		t.Fatal(err)
	}
	if len(database.DispatchFacts) != 0 || len(database.EntryFacts) != 0 ||
		len(database.EntryTemplates) != 0 {
		t.Fatalf("fixture database is not empty: %+v", database)
	}
	databasePath := filepath.Join(root, "saves", "static-analysis.json")
	if err := tooling.WriteStaticAnalysisDatabaseFile(databasePath, database); err != nil {
		t.Fatal(err)
	}

	controlPaths := DefaultPaths(root)
	controlPaths.GeneratedDir = "build/gen-control"
	controlPaths.FuncsHeader = "build/control-funcs.h"
	controlPaths.Metadata = "build/control-meta.json"
	controlPaths.RTSReport = "build/control-rts.txt"
	controlPaths.RTSPrevious = "build/control-rts.prev.txt"
	var controlOutput bytes.Buffer
	control, err := Regenerate(RegenOptions{
		Paths: controlPaths, Jobs: 1, AllowStubs: true,
		Stdout: &controlOutput, Stderr: &controlOutput,
	})
	if err != nil {
		t.Fatalf("control regeneration: %v\n%s", err, controlOutput.String())
	}

	databasePaths := DefaultPaths(root)
	databasePaths.GeneratedDir = "build/gen-database"
	databasePaths.FuncsHeader = "build/database-funcs.h"
	databasePaths.Metadata = "build/database-meta.json"
	databasePaths.RTSReport = "build/database-rts.txt"
	databasePaths.RTSPrevious = "build/database-rts.prev.txt"
	var databaseOutput bytes.Buffer
	candidate, err := Regenerate(RegenOptions{
		Paths: databasePaths, AnalysisDBPath: "saves/static-analysis.json",
		Jobs: 1, AllowStubs: true, Stdout: &databaseOutput, Stderr: &databaseOutput,
	})
	if err != nil {
		t.Fatalf("database regeneration: %v\n%s", err, databaseOutput.String())
	}
	if candidate.Generation.FinalEntries >= control.Generation.FinalEntries {
		t.Fatalf("zero-fact database variants = %d, control = %d; exact static mode was not enabled",
			candidate.Generation.FinalEntries, control.Generation.FinalEntries)
	}
	if candidate.Generation.SemanticSourceSHA256 ==
		control.Generation.SemanticSourceSHA256 {
		t.Fatal("zero-fact database unexpectedly preserved the conservative semantic source hash")
	}
	if !strings.Contains(databaseOutput.String(),
		"analysis-db: loaded 0 dispatch and 0 entry fact(s)") {
		t.Fatalf("zero-fact mode was not explicit in output:\n%s", databaseOutput.String())
	}
}
