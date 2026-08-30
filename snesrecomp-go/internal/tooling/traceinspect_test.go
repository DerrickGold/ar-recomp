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
	if len(report.Warnings) != 1 || !strings.Contains(report.Warnings[0], "no structured dispatch records") {
		t.Fatalf("warnings = %+v", report.Warnings)
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
	for _, fragment := range []string{"4 events", "1 invalid", "no structured dispatch records", "active_dispatch_continuation", "do not register", "va=$0010"} {
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

func TestTraceInspectionExcludesContinuationsAndUsesTerminalHits(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "dispatch.jsonl")
	trace := strings.Join([]string{
		`{"seq":1,"ch":"dispatch","site":"018000","target":"018100","m":0,"x":0,"found":0,"continuation":1,"trapped":0,"hits":1}`,
		`{"seq":2,"ch":"dispmiss","from":"018000","to":"018100","mnow":0,"xnow":0}`,
		`{"seq":3,"ch":"dispmiss","from":"018000","to":"018100","mnow":0,"xnow":0}`,
		`{"seq":4,"ch":"dispatch","site":"018000","target":"018100","m":0,"x":0,"found":0,"continuation":1,"trapped":0,"hits":8}`,
		`{"seq":5,"ch":"dispmiss","from":"018000","to":"018100","mnow":0,"xnow":0}`,
		`{"seq":6,"ch":"dispmiss","from":"018000","to":"018100","mnow":0,"xnow":0}`,
		`{"seq":7,"ch":"dispatch","site":"018200","target":"018300","m":0,"x":0,"found":0,"continuation":0,"trapped":0,"hits":1}`,
		`{"seq":8,"ch":"dispatch","site":"018200","target":"018300","m":0,"x":0,"found":0,"continuation":0,"trapped":0,"hits":2}`,
		`{"seq":9,"ch":"dispatch","site":"018200","target":"018300","m":0,"x":0,"found":0,"continuation":0,"trapped":0,"hits":4}`,
		`{"seq":10,"ch":"dispatch","site":"018200","target":"018300","m":0,"x":0,"found":0,"continuation":0,"trapped":0,"hits":9}`,
	}, "\n")
	if err := os.WriteFile(path, []byte(trace), 0o644); err != nil {
		t.Fatal(err)
	}
	report, err := BuildTraceInspection(TraceInspectOptions{
		TracePath: path, Diagnose: true, Limit: 20,
	})
	if err != nil {
		t.Fatal(err)
	}
	if report.Version != 3 || report.Summary.DispatchContinuation != 2 ||
		report.Summary.DispatchMiss != 4 {
		t.Fatalf("dispatch summary = %+v version=%d", report.Summary, report.Version)
	}
	if len(report.Findings) != 1 ||
		report.Findings[0].Kind != "missing_dispatch_target" ||
		report.Findings[0].Count != 9 ||
		report.Findings[0].SitePC == nil ||
		*report.Findings[0].SitePC != 0x018200 ||
		report.Findings[0].TargetPC == nil ||
		*report.Findings[0].TargetPC != 0x018300 {
		t.Fatalf("dispatch findings = %+v", report.Findings)
	}
	if len(report.Warnings) != 0 {
		t.Fatalf("structured dispatch trace warnings = %+v", report.Warnings)
	}
	if report.Truncated != 0 {
		t.Fatalf("diagnose-only report counted hidden raw events as truncated: %d", report.Truncated)
	}
}

func TestTraceInspectionDiagnoseDoesNotReportHiddenEventTruncation(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "clean.jsonl")
	trace := strings.Join([]string{
		`{"seq":1,"ch":"dispatch","site":"018000","target":"018100","m":0,"x":0,"found":1,"continuation":0,"trapped":0,"hits":1}`,
		`{"seq":2,"ch":"dispatch","site":"018000","target":"018100","m":0,"x":0,"found":1,"continuation":0,"trapped":0,"hits":2}`,
	}, "\n")
	if err := os.WriteFile(path, []byte(trace), 0o644); err != nil {
		t.Fatal(err)
	}
	report, err := BuildTraceInspection(TraceInspectOptions{
		TracePath: path, Summary: true, Diagnose: true, Limit: 1,
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(report.Findings) != 0 || len(report.Events) != 0 || report.Truncated != 0 {
		t.Fatalf("clean diagnose report = %+v", report)
	}
	var output bytes.Buffer
	if err := WriteTraceInspection(&output, report, "text"); err != nil {
		t.Fatal(err)
	}
	if strings.Contains(output.String(), "raise --limit") {
		t.Fatalf("clean diagnose output reports hidden raw-event truncation:\n%s", output.String())
	}
}
