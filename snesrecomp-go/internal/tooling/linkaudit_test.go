package tooling

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLinkAuditScansNestedSourceTrees(t *testing.T) {
	root := t.TempDir()
	genDir := filepath.Join(root, "gen")
	sourceDir := filepath.Join(root, "source")
	runtimeDir := filepath.Join(root, "runtime")
	for _, directory := range []string{genDir, filepath.Join(sourceDir, "game"), filepath.Join(runtimeDir, "core")} {
		if err := os.MkdirAll(directory, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	write := func(path, contents string) {
		t.Helper()
		if err := os.WriteFile(path, []byte(contents), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write(filepath.Join(genDir, "bank00_v2.c"),
		"RecompReturn Nested_M0X0(CpuState *cpu) { return RECOMP_RETURN_NORMAL; }\n")
	write(filepath.Join(sourceDir, "game", "adapter.c"),
		"void call_nested(CpuState *cpu) { Nested_M0X0(cpu); }\n")
	write(filepath.Join(runtimeDir, "core", "placeholder.c"), "/* runtime */\n")

	var output bytes.Buffer
	if err := RunLinkAudit(LinkAuditOptions{
		GenDir: genDir, SourceDir: sourceDir, RuntimeDir: runtimeDir,
		Output: &output,
	}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(output.String(), "reachable (referenced): 1") {
		t.Fatalf("nested reference was not counted:\n%s", output.String())
	}
}

func TestLinkAuditRanksRepeatedTailCallPairs(t *testing.T) {
	root := t.TempDir()
	genDir := filepath.Join(root, "gen")
	sourceDir := filepath.Join(root, "source")
	runtimeDir := filepath.Join(root, "runtime")
	for _, directory := range []string{genDir, sourceDir, runtimeDir} {
		if err := os.MkdirAll(directory, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	writeTestFile(t, filepath.Join(genDir, "bank00_v2.c"), `RecompReturn bank_00_8000_M1X1(CpuState *cpu) {
  bank_00_9000_M1X1(cpu); /* tail-call past end: into bank_00_9000_M1X1 at $9000 */
  bank_00_9000_M1X1(cpu); /* tail-call past end: into bank_00_9000_M1X1 at $9000 */
}
RecompReturn bank_00_9000_M1X1(CpuState *cpu) { return RECOMP_RETURN_NORMAL; }
`)
	writeTestFile(t, filepath.Join(sourceDir, "root.c"), "void root(CpuState *cpu) { bank_00_8000_M1X1(cpu); }\n")
	writeTestFile(t, filepath.Join(runtimeDir, "placeholder.c"), "/* runtime */\n")
	var output bytes.Buffer
	if err := RunLinkAudit(LinkAuditOptions{
		GenDir: genDir, SourceDir: sourceDir, RuntimeDir: runtimeDir,
		ListTailCalls: true, TailCallMinimum: 2, Output: &output,
	}); err != nil {
		t.Fatal(err)
	}
	for _, fragment := range []string{
		"tail-call-past-end : 2 site(s), 1 repeated source/target suspect(s)",
		"2x  bank_00_8000_M1X1 -> bank_00_9000_M1X1 ($9000)",
	} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("link audit output missing %q:\n%s", fragment, output.String())
		}
	}
}

func TestLinkAuditCountsEveryReachableTrapHelper(t *testing.T) {
	root := t.TempDir()
	genDir := filepath.Join(root, "gen")
	sourceDir := filepath.Join(root, "source")
	runtimeDir := filepath.Join(root, "runtime")
	for _, directory := range []string{genDir, sourceDir, runtimeDir} {
		if err := os.MkdirAll(directory, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	writeTestFile(t, filepath.Join(genDir, "bank00_v2.c"), `RecompReturn Goto_M1X1(CpuState *cpu) {
  return cpu_trace_unresolved_goto_trap(cpu, 0x008000, 0x008100, "Goto_M1X1", "L_8100_M1X1");
}
RecompReturn Dispatch_M1X1(CpuState *cpu) {
  return cpu_trace_dispatch_oob(cpu, 0x008010, 4);
}
RecompReturn Indirect_M1X1(CpuState *cpu) {
  return cpu_trace_unresolved_indirect_jump(cpu, 0x008020);
}
`)
	writeTestFile(t, filepath.Join(genDir, "unresolved_stubs_v2.c"), `RecompReturn Target_M1X1(CpuState *cpu) {
  return cpu_trace_unresolved_stub_trap(cpu, 0x008030, "Target_M1X1");
}
`)
	writeTestFile(t, filepath.Join(sourceDir, "root.c"), `void root(CpuState *cpu) {
  Goto_M1X1(cpu);
  Dispatch_M1X1(cpu);
  Indirect_M1X1(cpu);
  Target_M1X1(cpu);
}
`)
	writeTestFile(t, filepath.Join(runtimeDir, "placeholder.c"), "/* runtime */\n")

	var output bytes.Buffer
	err := RunLinkAudit(LinkAuditOptions{
		GenDir: genDir, SourceDir: sourceDir, RuntimeDir: runtimeDir,
		Output: &output,
	})
	if err == nil || err.Error() != "link audit found live traps" {
		t.Fatalf("link audit error = %v, want live-trap failure", err)
	}
	for _, fragment := range []string{
		"functions with traps : 4  (orphan/garbage: 0, LIVE/must-fix: 4)",
		"Goto_M1X1: goto",
		"Dispatch_M1X1: dispatch",
		"Indirect_M1X1: indirect",
		"Target_M1X1: target",
	} {
		if !strings.Contains(output.String(), fragment) {
			t.Fatalf("link audit output missing %q:\n%s", fragment, output.String())
		}
	}
}
