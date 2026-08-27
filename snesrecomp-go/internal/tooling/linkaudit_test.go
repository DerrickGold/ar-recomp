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
