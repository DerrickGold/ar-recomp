package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
)

func TestStaticAnalysisDatabaseRoundTripIsDeterministicAndROMBound(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	image := make([]byte, 0x8000)
	image[0] = 0x60
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	identity, err := shadowROMIdentity(romPath)
	if err != nil {
		t.Fatal(err)
	}
	database := StaticAnalysisDatabase{
		Version: staticAnalysisDatabaseVersion, Provenance: staticAnalysisDatabaseProvenance,
		ShadowReportVersion: shadowReportVersion, ROM: identity,
		DispatchFacts: []analysis.DispatchFact{{
			SitePC: 0x008100, Mnemonic: "RTS", Transfer: analysis.TransferResume,
			TargetEntryKind: analysis.EntryContinuation, Targets: []uint32{0x008200}, TargetSetClosed: true,
			Evidence: []analysis.Evidence{{Source: "static.fixture", Confidence: analysis.ConfidenceProven}},
		}},
		EntryFacts: []analysis.EntryFact{{
			PC: 0x008200, EntryMX: analysis.MXState{M: 1, X: 0}, Kind: analysis.EntryRoutine, TemplateFree: true,
			Evidence: []analysis.Evidence{{Source: "static.direct_jsr", Confidence: analysis.ConfidenceProven}},
		}},
		EntryTemplates: []analysis.EntryTemplatePlacement{{
			EntryVariant: analysis.EntryVariant{PC: 0x008200, EntryMX: analysis.MXState{M: 1, X: 0}},
			BankOrdinal:  3,
		}},
	}
	first := filepath.Join(root, "first.json")
	second := filepath.Join(root, "second.json")
	if err := WriteStaticAnalysisDatabaseFile(first, database); err != nil {
		t.Fatal(err)
	}
	if err := WriteStaticAnalysisDatabaseFile(second, database); err != nil {
		t.Fatal(err)
	}
	firstBytes, err := os.ReadFile(first)
	if err != nil {
		t.Fatal(err)
	}
	secondBytes, err := os.ReadFile(second)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(firstBytes, secondBytes) {
		t.Fatal("equal analysis databases did not produce byte-identical JSON")
	}
	loaded, err := LoadStaticAnalysisDatabaseFile(first, romPath)
	if err != nil {
		t.Fatal(err)
	}
	if len(loaded.DispatchFacts) != 1 || len(loaded.EntryFacts) != 1 || loaded.ROM != identity {
		t.Fatalf("loaded database = %+v", loaded)
	}
	image[1] = 0xEA
	if err := os.WriteFile(romPath, image, 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadStaticAnalysisDatabaseFile(first, romPath); err == nil || !strings.Contains(err.Error(), "ROM mismatch") {
		t.Fatalf("ROM mismatch error = %v", err)
	}
}

func TestStaticAnalysisDatabaseRejectsNonStaticEvidenceAndUnknownFields(t *testing.T) {
	root := t.TempDir()
	romPath := filepath.Join(root, "game.sfc")
	if err := os.WriteFile(romPath, make([]byte, 0x8000), 0o600); err != nil {
		t.Fatal(err)
	}
	identity, err := shadowROMIdentity(romPath)
	if err != nil {
		t.Fatal(err)
	}
	database := StaticAnalysisDatabase{
		Version: staticAnalysisDatabaseVersion, Provenance: staticAnalysisDatabaseProvenance,
		ShadowReportVersion: shadowReportVersion, ROM: identity,
		EntryFacts: []analysis.EntryFact{{
			PC: 0x008000, EntryMX: analysis.MXState{M: 1, X: 1}, Kind: analysis.EntryRoutine,
			Evidence: []analysis.Evidence{{Source: "replay.fixture", Confidence: analysis.ConfidenceObserved}},
		}},
	}
	if err := WriteStaticAnalysisDatabaseFile(filepath.Join(root, "bad.json"), database); err == nil || !strings.Contains(err.Error(), "not a static proof") {
		t.Fatalf("non-static evidence error = %v", err)
	}
	path := filepath.Join(root, "unknown.json")
	source := `{"version":1,"provenance":"snesrecomp-static-shadow-v1","shadow_report_version":17,"rom":{"sha256":"` + identity.SHA256 + `","size":32768,"mapper":"lorom"},"unexpected":true}`
	if err := os.WriteFile(path, []byte(source), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadStaticAnalysisDatabaseFile(path, romPath); err == nil || !strings.Contains(err.Error(), "unknown field") {
		t.Fatalf("unknown-field error = %v", err)
	}
}

func TestBuildStaticAnalysisDatabaseFailsOnAuthoredConflict(t *testing.T) {
	report := ShadowReport{
		Version: shadowReportVersion,
		ROM:     ShadowROM{SHA256: strings.Repeat("0", 64), Size: 0x8000, Mapper: "lorom"},
		Summary: ShadowSummary{ComparisonSummary: analysis.ComparisonSummary{Conflicts: 1}},
	}
	if _, err := BuildStaticAnalysisDatabase(report); err == nil || !strings.Contains(err.Error(), "1 authored conflict") {
		t.Fatalf("conflict error = %v", err)
	}
}
