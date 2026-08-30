package tooling

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestTraceInspectionFiltersAndDiagnosesContinuations(t *testing.T) {
	root := t.TempDir()
	tracePath := filepath.Join(root, "trace.jsonl")
	trace := strings.Join([]string{
		`{"seq":1,"hf":10,"gf":2,"ch":"func","fn":"bank_00_8000_M1X1","pc":"008000","m":1,"x":1,"em":1,"ex":1}`,
		`{"seq":2,"hf":10,"gf":2,"ch":"vram","fn":"bank_00_8000_M1X1","va":"0010","val":"22","path":"dma"}`,
		`{"seq":3,"hf":10,"gf":2,"ch":"call","fn":"bank_00_8000_M1X1","site":"008010","m":0,"x":1,"em":1,"ex":1,"leak":1}`,
		`{"seq":4,"hf":10,"gf":2,"ch":"dispmiss","fn":"bank_00_8000_M1X1","from":"008020","to":"008030","mnow":1,"xnow":1,"S":"01ff"}`,
		`not json`,
	}, "\n")
	if err := os.WriteFile(tracePath, []byte(trace), 0o644); err != nil {
		t.Fatal(err)
	}
	metadataPath := filepath.Join(root, "meta.json")
	metadata := GeneratedMetadata{
		Functions: map[string][]string{}, Labels: map[string][]string{},
		CFG: map[string][]MetadataDirective{"indirect_dispatch": {{Bank: "00", Line: 7, Text: "indirect_dispatch 8020 4 ret:8030"}}},
	}
	encoded, err := json.Marshal(metadata)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(metadataPath, encoded, 0o644); err != nil {
		t.Fatal(err)
	}
	rangeValue := TraceRange{Low: 0x10, High: 0x10}
	report, err := BuildTraceInspection(TraceInspectOptions{
		TracePath: tracePath, MetadataPath: metadataPath, Diagnose: true,
		VRAM: &rangeValue, Limit: 20,
	})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.Events != 4 || report.Summary.InvalidLines != 1 || report.Summary.Leaks != 1 || report.Summary.DispatchMiss != 1 {
		t.Fatalf("summary = %+v", report.Summary)
	}
	if len(report.Events) != 1 || traceString(report.Events[0], "ch") != "vram" {
		t.Fatalf("filtered events = %+v", report.Events)
	}
	if len(report.Findings) != 2 || report.Findings[0].Kind != "active_dispatch_continuation" || report.Findings[0].Suggestion != "" {
		t.Fatalf("findings = %+v", report.Findings)
	}
	var output bytes.Buffer
	if err := WriteTraceInspection(&output, report, "text"); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{"4 events", "1 invalid", "active_dispatch_continuation", "do not register", "va=$0010"} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("trace output missing %q:\n%s", fragment, output.String())
		}
	}
}

func TestParseTraceRange(t *testing.T) {
	got, err := ParseTraceRange("$0010-0x0020")
	if err != nil || got.Low != 0x10 || got.High != 0x20 {
		t.Fatalf("ParseTraceRange = %+v, %v", got, err)
	}
}

func TestTraceInspectionAcceptsLegacyDispatchDump(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "dispatch.json")
	contents := `{"dispatch_log":{"events":[{"pc24":"008123","source_pc24":"008000","mx":3,"found":false}]}}`
	if err := os.WriteFile(path, []byte(contents), 0o644); err != nil {
		t.Fatal(err)
	}
	report, err := BuildTraceInspection(TraceInspectOptions{TracePath: path, Diagnose: true, Limit: 10})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.Events != 1 || report.Summary.DispatchMiss != 1 || len(report.Findings) != 1 || report.Findings[0].Kind != "missing_dispatch_target" {
		t.Fatalf("legacy dispatch report = %+v", report)
	}
}
