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
