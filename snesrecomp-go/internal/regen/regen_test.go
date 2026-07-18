package regen

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLintStubsIgnoresMarkerNameInHeaderComment(t *testing.T) {
	directory := t.TempDir()
	path := filepath.Join(directory, "unresolved_stubs_v2.c")
	header := "/* each stub chains into cpu_trace_unresolved_stub_trap */\n"
	if err := os.WriteFile(path, []byte(header), 0o644); err != nil {
		t.Fatal(err)
	}
	hits, err := lintStubs(directory)
	if err != nil {
		t.Fatal(err)
	}
	if hits != 0 {
		t.Fatalf("header-only marker hits = %d, want 0", hits)
	}
	call := header + "return cpu_trace_unresolved_stub_trap(cpu, 0, \"test\");\n"
	if err := os.WriteFile(path, []byte(call), 0o644); err != nil {
		t.Fatal(err)
	}
	hits, err = lintStubs(directory)
	if err != nil {
		t.Fatal(err)
	}
	if hits != 1 {
		t.Fatalf("call marker hits = %d, want 1", hits)
	}
}
