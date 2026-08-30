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
