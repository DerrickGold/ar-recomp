package project

import (
	"bytes"
	"context"
	"debug/elf"
	"debug/macho"
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"testing"
	"time"
)

// A real zig build of units whose sizes disagree with their source order:
// compiles start largest-first, but objects link in source order.
func TestHermeticBuildSchedulesLargestFirstAndLinksInSourceOrder(t *testing.T) {
	zig := dependencyTestZig(t)
	scratch := t.TempDir()
	t.Setenv("ZIG_GLOBAL_CACHE_DIR", filepath.Join(scratch, "zig-global"))
	t.Setenv("ZIG_LOCAL_CACHE_DIR", filepath.Join(scratch, "zig-local"))
	root := filepath.Join(scratch, "game")
	padding := func(bytes int) string {
		return "/*" + strings.Repeat(" padding", bytes/8) + " */\n"
	}
	for relative, contents := range map[string]string{
		"snesbuild.ini": "[project]\nname = MyGame\nsource = src/main.c\nsource = src/big.c\n" +
			"source = src/small.c\nsource = src/mid.c\n",
		"CMakeLists.txt": "add_executable(MyGame\n    src/main.c\n    src/big.c\n    src/small.c\n    src/mid.c\n)\n",
		"src/main.c": "int f_big(void);\nint f_small(void);\nint f_mid(void);\n" +
			"int f_main(void) { return 1; }\n" +
			"int main(void) { return f_main() + f_big() + f_small() + f_mid() == 10 ? 0 : 1; }\n",
		"src/big.c":                          padding(24000) + "int f_big(void) { return 2; }\n",
		"src/small.c":                        "int f_small(void) { return 3; }\n",
		"src/mid.c":                          padding(12000) + "int f_mid(void) { return 4; }\n",
		"src/gen/gen_unit.c":                 "int generated_symbol;\n",
		"snesrecomp-go/runtime/src/runner.c": "int runner_symbol;\n",
		"snesrecomp-go/runtime/include/r.h":  "/* public */\n",
		"snesrecomp-go/runtime/runner.cmake": `
set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/runner.c
)
set(SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/include
)
set(SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
)
`,
	} {
		path := filepath.Join(root, filepath.FromSlash(relative))
		writeTestFile(t, path, contents)
		backdate(t, path, time.Hour)
	}
	paths := DefaultPaths(root)
	build := func(options HermeticOptions) (string, string, error) {
		t.Helper()
		var log bytes.Buffer
		options.Paths, options.ZigPath, options.Verbose = paths, zig, true
		options.Stdout, options.Stderr = &log, &log
		binary, err := HermeticBuild(options)
		return binary, log.String(), err
	}
	compiled := func(log string) []string {
		var units []string
		for _, line := range strings.Split(log, "\n") {
			if source, found := strings.CutPrefix(line, "  cc "); found {
				units = append(units, filepath.Base(source))
			}
		}
		return units
	}
	clean := func() {
		t.Helper()
		if err := os.RemoveAll(filepath.Join(root, "build")); err != nil {
			t.Fatal(err)
		}
	}

	// A budget smaller than any estimate: every unit runs alone, in
	// largest-first order, and each start follows the previous finish.
	binary, log, err := build(HermeticOptions{Jobs: 4, MemoryBudget: 1})
	if err != nil {
		t.Fatalf("build: %v\n%s", err, log)
	}
	units := compiled(log)
	if len(units) != 6 || units[0] != "big.c" || units[1] != "mid.c" {
		t.Fatalf("compile order %v\n%s", units, log)
	}
	open := 0
	for _, line := range strings.Split(log, "\n") {
		switch {
		case strings.HasPrefix(line, "  cc "):
			if open++; open > 1 {
				t.Fatalf("a unit started while another was compiling\n%s", log)
			}
		case strings.HasPrefix(line, "  compiled ["):
			open--
		}
	}
	if strings.Count(log, "so it compiles alone") != 6 || !strings.Contains(log, "at most 1 compiles at once") {
		t.Fatalf("the budget was not reported\n%s", log)
	}
	if !strings.Contains(log, "archived and linked in ") {
		t.Fatalf("link time was not reported\n%s", log)
	}
	checkLinkOrder(t, binary, "f_main", "f_big", "f_small", "f_mid")

	// Cached units are not scheduled at all; an edited one is.
	if _, log, err = build(HermeticOptions{Jobs: 4, MemoryBudget: 1}); err != nil ||
		!strings.Contains(log, "6 cached, 0 to compile") || strings.Contains(log, "compile memory budget") {
		t.Fatalf("no-op rebuild: %v\n%s", err, log)
	}
	writeTestFile(t, filepath.Join(root, "src", "small.c"), "int f_small(void) { return 1 + 2; }\n")
	if _, log, err = build(HermeticOptions{Jobs: 4, MemoryBudget: 1}); err != nil ||
		!strings.Contains(log, "5 cached, 1 to compile") ||
		!equalStrings(compiled(log), []string{"small.c"}) ||
		strings.Count(log, "so it compiles alone") != 1 {
		t.Fatalf("incremental rebuild: %v\n%s", err, log)
	}

	// Without a budget --jobs alone bounds concurrency; the link order is
	// the same however the compiles overlapped.
	clean()
	binary, log, err = build(HermeticOptions{Jobs: 4, MemoryBudget: -1})
	if err != nil || !strings.Contains(log, "compile memory budget off") {
		t.Fatalf("unbudgeted build: %v\n%s", err, log)
	}
	checkLinkOrder(t, binary, "f_main", "f_big", "f_small", "f_mid")

	// The default budget comes from physical memory where it is known.
	clean()
	if _, log, err = build(HermeticOptions{Jobs: 2}); err != nil {
		t.Fatalf("default build: %v\n%s", err, log)
	}
	if _, known := physicalMemoryBytes(); known && !strings.Contains(log, "physical)") {
		t.Fatalf("the default budget does not name its source\n%s", log)
	}

	// After a failure nothing further starts: with one slot, the failing
	// largest unit is the only compile.
	clean()
	writeTestFile(t, filepath.Join(root, "src", "big.c"), padding(24000)+"int f_big(void) { return 2 }\n")
	if _, log, err = build(HermeticOptions{Jobs: 1, MemoryBudget: -1}); err == nil ||
		!strings.Contains(err.Error(), "big.c") || !equalStrings(compiled(log), []string{"big.c"}) {
		t.Fatalf("failed build: %v\n%s", err, log)
	}

	// A cancelled build starts nothing and says why it stopped.
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, log, err = build(HermeticOptions{Jobs: 4, Context: ctx}); !errors.Is(err, context.Canceled) ||
		len(compiled(log)) != 0 {
		t.Fatalf("cancelled build: %v\n%s", err, log)
	}
}

func equalStrings(left, right []string) bool {
	return strings.Join(left, "\x00") == strings.Join(right, "\x00")
}

// checkLinkOrder requires the named functions to be laid out in the given
// order, which is the order their objects reached the linker.
func checkLinkOrder(t *testing.T, binary string, names ...string) {
	t.Helper()
	addresses := make(map[string]uint64)
	switch runtime.GOOS {
	case "darwin":
		file, err := macho.Open(binary)
		if err != nil {
			t.Fatal(err)
		}
		defer file.Close()
		if file.Symtab == nil {
			t.Fatal("no symbol table")
		}
		for _, symbol := range file.Symtab.Syms {
			addresses[strings.TrimPrefix(symbol.Name, "_")] = symbol.Value
		}
	case "linux":
		file, err := elf.Open(binary)
		if err != nil {
			t.Fatal(err)
		}
		defer file.Close()
		symbols, err := file.Symbols()
		if err != nil {
			t.Fatal(err)
		}
		for _, symbol := range symbols {
			addresses[symbol.Name] = symbol.Value
		}
	default:
		t.Logf("link order not inspected on %s", runtime.GOOS)
		return
	}
	for _, name := range names {
		if addresses[name] == 0 {
			t.Fatalf("symbol %s missing from %s", name, binary)
		}
	}
	if !sort.SliceIsSorted(names, func(i, j int) bool { return addresses[names[i]] < addresses[names[j]] }) {
		layout := make([]string, 0, len(names))
		for _, name := range names {
			layout = append(layout, name)
		}
		sort.Slice(layout, func(i, j int) bool { return addresses[layout[i]] < addresses[layout[j]] })
		t.Fatalf("functions laid out as %v, want source order %v", layout, names)
	}
}
