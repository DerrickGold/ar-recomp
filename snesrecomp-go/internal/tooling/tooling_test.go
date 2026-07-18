package tooling

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func writeTestFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestSyncFuncs(t *testing.T) {
	root := t.TempDir()
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nfunc First 8000 entry_mx:1,1\nname 018100 Alias\n")
	writeTestFile(t, filepath.Join(cfgDir, "bank01.cfg"), "bank = 01\nfunc Second 9000 entry_mx:1,1\n")
	output := filepath.Join(root, "funcs.h")
	count, err := SyncFuncs(cfgDir, output)
	if err != nil {
		t.Fatal(err)
	}
	if count != 3 {
		t.Fatalf("count = %d, want 3", count)
	}
	data, err := os.ReadFile(output)
	if err != nil {
		t.Fatal(err)
	}
	source := string(data)
	for _, wanted := range []string{
		"void First(CpuState *cpu);  /* $00:8000 alias */",
		"RecompReturn Alias_M0X1(CpuState *cpu);",
		"void Second(CpuState *cpu);  /* $01:9000 alias */",
	} {
		if !strings.Contains(source, wanted) {
			t.Errorf("header missing %q", wanted)
		}
	}
}

func TestGenerateMetadata(t *testing.T) {
	root := t.TempDir()
	genDir, cfgDir := filepath.Join(root, "gen"), filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(genDir, "bank00_v2.c"), "RecompReturn bank_00_8000_M1X1(CpuState *cpu) {\n  L_8000_M1X1:\n  return RECOMP_RETURN_NORMAL; /* tail-call past end: into bank_00_8010_M1X1 at $8010 */\n}\n")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nfunc bank_00_8000 8000\nrts_dispatch 8005 8010\n")
	output := filepath.Join(root, "meta.json")
	now := time.Date(2026, 7, 18, 1, 2, 3, 0, time.Local)
	report, err := GenerateMetadata(genDir, cfgDir, output, now)
	if err != nil {
		t.Fatal(err)
	}
	if report.Functions != 1 || report.Labels != 1 || report.TailCalls != 1 || report.CFGCounts["func"] != 1 {
		t.Fatalf("unexpected report: %+v", report)
	}
	var metadata GeneratedMetadata
	data, err := os.ReadFile(output)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(data, &metadata); err != nil {
		t.Fatal(err)
	}
	if metadata.GeneratedAt != "2026-07-18 01:02:03" || metadata.Functions["008000"][0] != "_M1X1" || metadata.TailCalls[0].TargetPC != "008010" {
		t.Fatalf("unexpected metadata: %+v", metadata)
	}
}

func TestCensusRTSWebs(t *testing.T) {
	root := t.TempDir()
	image := make([]byte, 0x8000)
	image[0], image[1], image[2], image[3] = 0xA9, 0x0F, 0x80, 0x48
	image[0x10], image[0x11], image[0x12] = 0xEA, 0xEA, 0x60
	image[0x20], image[0x21] = 0x48, 0x60
	romPath := filepath.Join(root, "test.sfc")
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\n")
	var output bytes.Buffer
	report, err := CensusRTSWebs(RTSCensusOptions{ROMPath: romPath, CFGDir: cfgDir, Output: &output})
	if err != nil {
		t.Fatal(err)
	}
	if report.UncoveredPushes != 1 || report.UncoveredSites != 1 {
		t.Fatalf("unexpected report: %+v\n%s", report, output.String())
	}
	if !strings.Contains(output.String(), "push @00:8000") || !strings.Contains(output.String(), "PHA;RTS dispatch @00:8021") {
		t.Fatalf("unexpected output:\n%s", output.String())
	}
}

func TestCensusStubsCollapsesVariants(t *testing.T) {
	root := t.TempDir()
	writeTestFile(t, filepath.Join(root, "bank00_v2.c"), "return cpu_trace_unresolved_goto_trap(cpu, 0x008000, 0x008100, \"Fn_M0X0\", \"L\");\nreturn cpu_trace_unresolved_goto_trap(cpu, 0x008000, 0x008100, \"Fn_M1X1\", \"L\");\nreturn cpu_trace_dispatch_oob(cpu, 0x008200, 0xffff);\n")
	var output bytes.Buffer
	report, err := CensusStubs(root, true, &output)
	if err != nil {
		t.Fatal(err)
	}
	if report.LogicalGotos != 1 || report.GotoEmissions != 2 || report.LogicalDispatches != 1 || report.DispatchEmissions != 1 {
		t.Fatalf("unexpected report: %+v", report)
	}
}
