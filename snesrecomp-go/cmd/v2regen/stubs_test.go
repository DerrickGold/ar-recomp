package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestCensusJSONDoesNotWaiveStrictGateOrOverwriteBaseline(t *testing.T) {
	dir := t.TempDir()
	source := "RecompReturn bank_00_8000_M0X0(CpuState *cpu) {\n  L_8000_M0X0:\nreturn cpu_trace_unresolved_indirect_jump(cpu, 0x008000);\n}\n"
	if err := os.WriteFile(filepath.Join(dir, "bank00_v2.c"), []byte(source), 0600); err != nil {
		t.Fatal(err)
	}
	baseline := filepath.Join(dir, "baseline.json")
	if err := censusStubs([]string{"--gen-dir", dir, "--json", baseline}); err == nil {
		t.Fatal("JSON export waived strict stub gate")
	}
	before, err := os.ReadFile(baseline)
	if err != nil {
		t.Fatal(err)
	}
	if err := censusStubs([]string{"--gen-dir", dir, "--baseline", baseline}); err != nil {
		t.Fatal(err)
	}
	if err := censusStubs([]string{"--gen-dir", dir, "--baseline", baseline, "--json", baseline}); err == nil {
		t.Fatal("allowed overwriting comparison baseline")
	}
	after, err := os.ReadFile(baseline)
	if err != nil || string(before) != string(after) {
		t.Fatal("baseline mutated")
	}
}
