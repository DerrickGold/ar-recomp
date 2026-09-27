package tooling

import (
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestStubCensusContextsAndStrictTotal(t *testing.T) {
	dir := t.TempDir()
	source := `RecompReturn bank_00_8000_M0X0(CpuState *cpu);
RecompReturn bank_00_8000_M0X1(CpuState *cpu) {
  L_8000_M0X1:
    return cpu_trace_unresolved_goto_trap(cpu, 0x008001, 0x001234, "bank_00_8000_M0X1", "unknown");
    cpu_trace_dispatch_oob(cpu, 0x008004, 0);
    return cpu_trace_unresolved_stub_trap(cpu, 0x002000, "bank_00_2000");
  L_8008_M1X1:
    return cpu_trace_unresolved_indirect_jump(cpu, 0x008008);
}
`
	if err := os.WriteFile(filepath.Join(dir, "bank00_v2.c"), []byte(source), 0600); err != nil {
		t.Fatal(err)
	}
	report, err := CensusStubs(dir, false, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if report.LogicalTotal() != 4 || len(report.Entries) != 4 {
		t.Fatalf("%+v", report)
	}
	for _, entry := range report.Entries {
		if len(entry.Contexts) != 1 || !strings.HasPrefix(entry.Contexts[0], "bank_00_8000_M0X1/L_") {
			t.Fatalf("prototype mistaken for owner: %+v", entry)
		}
		if strings.HasPrefix(entry.Key, "indirect:") && !strings.HasSuffix(entry.Contexts[0], "M1X1") {
			t.Fatalf("lost block-entry widths: %+v", entry)
		}
	}
}

func TestStubBaselineDetectsReplacedSitesAndWidths(t *testing.T) {
	entry := StubCensusEntry{Key: "indirect:008008", Emissions: 2,
		Contexts: []string{"bank_00_8000_M0X0/L_8008_M1X0"}}
	baseline := StubCensusReport{Version: 1, LogicalIndirects: 1, IndirectEmissions: 2,
		Entries: []StubCensusEntry{entry}}
	path := filepath.Join(t.TempDir(), "baseline.json")
	data, err := json.Marshal(baseline)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	if err := CompareStubBaseline(baseline, path, io.Discard); err != nil {
		t.Fatal(err)
	}
	if err := CompareStubBaseline(StubCensusReport{Version: 1}, path, io.Discard); err != nil {
		t.Fatal(err)
	}
	for _, change := range []StubCensusEntry{
		{Key: "indirect:009999", Emissions: 1, Contexts: entry.Contexts},
		{Key: entry.Key, Emissions: 3, Contexts: entry.Contexts},
		{Key: entry.Key, Emissions: 1, Contexts: []string{"bank_00_8000_M0X0/L_8008_M1X1"}},
	} {
		report := StubCensusReport{Version: 1, LogicalIndirects: 1, Entries: []StubCensusEntry{change}}
		if err := CompareStubBaseline(report, path, io.Discard); err == nil {
			t.Fatalf("accepted changed diagnostic: %+v", change)
		}
	}
}

func TestStubBaselineRejectsIncompleteInventory(t *testing.T) {
	path := filepath.Join(t.TempDir(), "baseline.json")
	for _, data := range []string{`{}`, `{"Version":1,"LogicalIndirects":1}`, `not JSON`} {
		if err := os.WriteFile(path, []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
		if err := CompareStubBaseline(StubCensusReport{Version: 1}, path, io.Discard); err == nil {
			t.Fatalf("accepted %s", data)
		}
	}
}

func TestStubCensusDoesNotTreatMissingGeneratedFilesAsClean(t *testing.T) {
	if _, err := CensusStubs(t.TempDir(), false, io.Discard); err == nil {
		t.Fatal("missing generated code counted as zero remaining diagnostics")
	}
}
