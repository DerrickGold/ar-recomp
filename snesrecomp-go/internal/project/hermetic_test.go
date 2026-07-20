package project

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestObjectNameMangling(t *testing.T) {
	root := filepath.FromSlash("/proj")
	inside := objectName(root, filepath.FromSlash("/proj/src/gen/bank00_part00_v2.c"))
	if inside != "src_gen_bank00_part00_v2.c.o" {
		t.Fatalf("inside: %s", inside)
	}
	outside := objectName(root, filepath.FromSlash("/elsewhere/runtime/src/a.c"))
	if !strings.HasPrefix(outside, "ext_") || !strings.HasSuffix(outside, "a.c.o") {
		t.Fatalf("outside: %s", outside)
	}
	// Distinct outside paths with the same basename must not collide.
	other := objectName(root, filepath.FromSlash("/elsewhere2/runtime/src/a.c"))
	if other == outside {
		t.Fatalf("collision: %s", other)
	}
}

func TestObjectFresh(t *testing.T) {
	directory := t.TempDir()
	source := filepath.Join(directory, "a.c")
	object := filepath.Join(directory, "a.o")
	writeTestFile(t, source, "int x;\n")
	writeTestFile(t, object, "obj")

	old := time.Now().Add(-time.Hour)
	newer := time.Now().Add(time.Hour)
	if err := os.Chtimes(source, old, old); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(object, newer, newer); err != nil {
		t.Fatal(err)
	}
	if !objectFresh(source, object, time.Time{}) {
		t.Fatal("object newer than source should be fresh")
	}
	if objectFresh(source, object, time.Now().Add(2*time.Hour)) {
		t.Fatal("newer header must invalidate the object")
	}
	if err := os.Chtimes(source, newer.Add(time.Hour), newer.Add(time.Hour)); err != nil {
		t.Fatal(err)
	}
	if objectFresh(source, object, time.Time{}) {
		t.Fatal("newer source must invalidate the object")
	}
	if objectFresh(filepath.Join(directory, "missing.c"), object, time.Time{}) {
		t.Fatal("missing source must not count as fresh")
	}
	if objectFresh(source, filepath.Join(directory, "missing.o"), time.Time{}) {
		t.Fatal("missing object must not count as fresh")
	}
}

func TestNewestHeaderTime(t *testing.T) {
	directory := t.TempDir()
	writeTestFile(t, filepath.Join(directory, "a.h"), "x")
	writeTestFile(t, filepath.Join(directory, "b.c"), "x")
	nested := filepath.Join(directory, "nested")
	writeTestFile(t, filepath.Join(nested, "deep.h"), "x")

	when := time.Now().Add(30 * time.Minute)
	if err := os.Chtimes(filepath.Join(directory, "a.h"), when, when); err != nil {
		t.Fatal(err)
	}
	newest := newestHeaderTime([]string{directory, filepath.Join(directory, "missing")})
	if !newest.Equal(when.Truncate(time.Second)) && !newest.After(when.Add(-time.Second)) {
		t.Fatalf("newest: %v want ~%v", newest, when)
	}
	// Non-recursive by contract: nested headers only count via their own dir.
	nestedOnly := newestHeaderTime([]string{nested})
	if nestedOnly.IsZero() {
		t.Fatal("nested dir scan found nothing")
	}
}

func TestFirstFlagValue(t *testing.T) {
	// SDL3's sdl3.pc reports the parent include dir (the game includes
	// <SDL3/SDL.h>), so the first -I is taken verbatim.
	if got := firstFlagValue("-I/opt/homebrew/include -D_THREAD_SAFE", "-I"); got != "/opt/homebrew/include" {
		t.Fatalf("got %q", got)
	}
	if got := firstFlagValue("", "-I"); got != "" {
		t.Fatalf("got %q", got)
	}
	if got := firstFlagValue("-I", "-I"); got != "" {
		t.Fatalf("bare prefix: %q", got)
	}
}
