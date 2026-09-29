package project

import (
	"bytes"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestParseMakeDepfile(t *testing.T) {
	for _, test := range []struct {
		name, input string
		want        []string
	}{
		{"clang, continued", "/b/obj/a.o: /s/src/a.c /s/include/a.h \\\n  /s/include/b.h\n",
			[]string{"/s/src/a.c", "/s/include/a.h", "/s/include/b.h"}},
		{"make escapes", "/b/build\\ dir/a.o: /s/src\\ dir/a.c /s/x\\#y.h /s/cost$$.h /s/odd\\\\\\ name.h\n",
			[]string{"/s/src dir/a.c", "/s/x#y.h", "/s/cost$.h", `/s/odd\ name.h`}},
		{"windows, make", "C:\\b\\obj\\a.o: C:\\s\\a.c C:\\Program\\ Files\\zig\\lib\\stdio.h \\\r\n  C:\\s\\inc\\a.h\r\n",
			[]string{`C:\s\a.c`, `C:\Program Files\zig\lib\stdio.h`, `C:\s\inc\a.h`}},
		{"windows, nmake quoting", "\"C:\\build dir\\a.o\": \"C:\\src dir\\a.c\" C:\\inc\\a.h\n",
			[]string{`C:\src dir\a.c`, `C:\inc\a.h`}},
		{"phony header rules and duplicates", "a.o: a.c a.h a.h\n\na.h:\n", []string{"a.c", "a.h"}},
		{"spaced separator, two targets", "a.o b.o : a.c\n", []string{"a.c"}},
		{"continuation against a name", "a.o: a.c\\\n b.h\n", []string{"a.c", "b.h"}},
		{"no final newline", "a.o: a.c", []string{"a.c"}},
	} {
		got, err := parseMakeDepfile([]byte(test.input))
		if err != nil {
			t.Errorf("%s: %v", test.name, err)
			continue
		}
		if !reflect.DeepEqual(got, test.want) {
			t.Errorf("%s: got %q, want %q", test.name, got, test.want)
		}
	}
	// A record must never be built from a depfile the parser does not fully
	// understand: no rule, no prerequisites, stray text or broken quoting.
	for _, bad := range []string{"", "\n", "a.o a.c a.h\n", "a.o:\n", "\"a.o: a.c\n",
		": a.c\n", "a.o: b: c\n", "\"a.o\"x: a.c\n"} {
		if got, err := parseMakeDepfile([]byte(bad)); err == nil {
			t.Errorf("accepted %q as %q", bad, got)
		}
	}
}

// writeCompiledObject leaves what a successful compile leaves: an object and
// its Make-format depfile naming prerequisites.
func writeCompiledObject(t *testing.T, object, contents string, prerequisites ...string) {
	t.Helper()
	writeTestFile(t, object, contents)
	escape := strings.NewReplacer(" ", `\ `, "#", `\#`, "$", "$$")
	line := escape.Replace(object) + ":"
	for _, prerequisite := range prerequisites {
		line += " " + escape.Replace(prerequisite)
	}
	writeTestFile(t, compilerDepfilePath(object), line+"\n")
}

// backdate gives path a distinct mtime in the past: records compare stamps
// exactly, and a date after a compile started reads as an edit during it.
func backdate(t *testing.T, path string, age time.Duration) {
	t.Helper()
	when := time.Now().Add(-age)
	if err := os.Chtimes(path, when, when); err != nil {
		t.Fatal(err)
	}
}

func TestDependencyRecordReuseAndInvalidation(t *testing.T) {
	directory := t.TempDir()
	source := filepath.Join(directory, "src", "a.c")
	header := filepath.Join(directory, "include dir", "a#1.h")
	object := filepath.Join(directory, "obj", "a.c.o")
	writeTestFile(t, source, "#include \"a#1.h\"\n")
	writeTestFile(t, header, "#define A 1\n")
	backdate(t, source, time.Hour)
	backdate(t, header, time.Hour)
	compile := func() {
		t.Helper()
		writeCompiledObject(t, object, "object", source, header)
		if err := recordObjectDependencies(HermeticOptions{}, object, source, "flags", time.Now(), directory); err != nil {
			t.Fatal(err)
		}
	}
	reusable := func() bool {
		t.Helper()
		current, _ := objectDependenciesCurrent(HermeticOptions{}, object, "flags")
		return current
	}

	compile()
	if current, reason := objectDependenciesCurrent(HermeticOptions{}, object, "flags"); !current {
		t.Fatalf("an unchanged object was not reused: %s", reason)
	}
	if current, _ := objectDependenciesCurrent(HermeticOptions{}, object, "other flags"); current {
		t.Fatal("an object built with other flags was reused")
	}
	// Restoring an older copy moves an mtime backwards. An object newer than
	// every input would still look fresh; records compare exactly.
	backdate(t, header, 2*time.Hour)
	if reusable() {
		t.Fatal("a header restored with an older mtime reused the object")
	}
	compile()
	writeTestFile(t, source, "#include \"a#1.h\"\nint edited;\n")
	backdate(t, source, 30*time.Minute)
	if reusable() {
		t.Fatal("an edited source reused the object")
	}
	compile()
	if err := os.Remove(header); err != nil {
		t.Fatal(err)
	}
	if reusable() {
		t.Fatal("an object whose header is gone was reused")
	}
	writeTestFile(t, header, "#define A 1\n")
	backdate(t, header, 3*time.Hour)
	compile()
	// A record describes one exact object.
	writeTestFile(t, object, "a different object")
	if reusable() {
		t.Fatal("a replaced object was reused under the old record")
	}
	compile()
	for name, damage := range map[string]func(){
		"missing record": func() {
			if err := os.Remove(dependencyRecordPath(object)); err != nil {
				t.Fatal(err)
			}
		},
		"truncated record": func() {
			writeTestFile(t, dependencyRecordPath(object), "{\"version\":")
		},
		"record of another format version": func() {
			var record dependencyRecord
			data, err := os.ReadFile(dependencyRecordPath(object))
			if err != nil {
				t.Fatal(err)
			}
			if err := json.Unmarshal(data, &record); err != nil {
				t.Fatal(err)
			}
			record.Version++
			data, err = json.Marshal(record)
			if err != nil {
				t.Fatal(err)
			}
			writeTestFile(t, dependencyRecordPath(object), string(data))
		},
	} {
		damage()
		if reusable() {
			t.Fatalf("%s: the object was reused", name)
		}
		compile()
	}
}

func TestDependencyRecordRefusesUncertainInputs(t *testing.T) {
	directory := t.TempDir()
	source := filepath.Join(directory, "a.c")
	header := filepath.Join(directory, "a.h")
	object := filepath.Join(directory, "a.o")
	writeTestFile(t, source, "x\n")
	writeTestFile(t, header, "y\n")

	// Both inputs were written after this compile started, so what it read is
	// unknown. No record: the object is rebuilt next time.
	writeCompiledObject(t, object, "object", source, header)
	if err := recordObjectDependencies(HermeticOptions{}, object, source, "flags", time.Now().Add(-time.Minute), directory); err == nil {
		t.Error("recorded inputs modified while their compile ran")
	}
	backdate(t, source, time.Hour)
	backdate(t, header, time.Hour)
	// A depfile that does not name the unit's own source describes some other
	// compile.
	writeCompiledObject(t, object, "object", header)
	if err := recordObjectDependencies(HermeticOptions{}, object, source, "flags", time.Now(), directory); err == nil {
		t.Error("recorded a depfile that does not name its source")
	}
	if _, err := os.Stat(dependencyRecordPath(object)); err == nil {
		t.Error("a refused record was written anyway")
	}
	// An empty object is a failed compile's placeholder, never a result.
	writeTestFile(t, object, "")
	writeTestFile(t, compilerDepfilePath(object), "a.o: a.c a.h\n")
	if err := recordObjectDependencies(HermeticOptions{}, object, source, "flags", time.Now(), directory); err == nil {
		t.Error("recorded an empty object")
	}
	// Relative names resolve against the compiler's working directory.
	writeTestFile(t, object, "object")
	if err := recordObjectDependencies(HermeticOptions{}, object, source, "flags", time.Now(), directory); err != nil {
		t.Fatal(err)
	}
	if current, reason := objectDependenciesCurrent(HermeticOptions{}, object, "flags"); !current {
		t.Fatalf("relative depfile names were not resolved: %s", reason)
	}
	if err := removeObjectOutputs(object); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{object, compilerDepfilePath(object), dependencyRecordPath(object)} {
		if _, err := os.Stat(path); err == nil {
			t.Errorf("%s survived removeObjectOutputs", path)
		}
	}
}

func TestDependencyRecordBundledInputsSurviveRelocation(t *testing.T) {
	parent := t.TempDir()
	root := filepath.Join(parent, "bundle a")
	source := filepath.Join(root, "src", "a.c")
	bundledHeader := filepath.Join(root, "src", "a.h")
	generated := filepath.Join(parent, "generated", "funcs.h")
	object := filepath.Join(parent, "build", "a.o")
	for _, path := range []string{source, bundledHeader, generated} {
		writeTestFile(t, path, "x\n")
		backdate(t, path, time.Hour)
	}
	options := HermeticOptions{InputID: strings.Repeat("a", 64)}
	options.Root = root
	writeCompiledObject(t, object, "object", source, bundledHeader, generated)
	if err := recordObjectDependencies(options, object, source, "flags", time.Now(), parent); err != nil {
		t.Fatal(err)
	}
	moved := filepath.Join(parent, "bundle b")
	if err := os.Rename(root, moved); err != nil {
		t.Fatal(err)
	}
	options.Root = moved
	if current, reason := objectDependenciesCurrent(options, object, "flags"); !current {
		t.Fatalf("a relocated bundle lost its objects: %s", reason)
	}
	// Without a verified digest nothing vouches for the bundled inputs.
	if current, _ := objectDependenciesCurrent(HermeticOptions{}, object, "flags"); current {
		t.Fatal("a bundle-relative record was trusted without an input digest")
	}
	// Inputs outside the bundle are still stamped.
	writeTestFile(t, generated, "y\n")
	if current, _ := objectDependenciesCurrent(options, object, "flags"); current {
		t.Fatal("an edited generated header reused the object")
	}
}

func dependencyTestZig(t *testing.T) string {
	t.Helper()
	if zig := os.Getenv("SNESBUILD_ZIG"); zig != "" {
		return zig
	}
	if zig, err := filepath.Abs("../../../build/toolchain/zig-aarch64-macos-0.16.0/zig"); err == nil {
		if _, statErr := os.Stat(zig); statErr == nil {
			return zig
		}
	}
	if zig, err := exec.LookPath("zig"); err == nil {
		return zig
	}
	t.Skip("Zig unavailable")
	return ""
}

// A real zig compile: which objects a change invalidates depends on what the
// compiler actually writes into its depfile.
func TestHermeticBuildRebuildsOnlyAffectedObjects(t *testing.T) {
	zig := dependencyTestZig(t)
	scratch := t.TempDir()
	t.Setenv("ZIG_GLOBAL_CACHE_DIR", filepath.Join(scratch, "zig-global"))
	t.Setenv("ZIG_LOCAL_CACHE_DIR", filepath.Join(scratch, "zig-local"))
	root := filepath.Join(scratch, "game project")
	for relative, contents := range map[string]string{
		"snesbuild.ini":                          "[project]\nname = MyGame\nsource = src/main.c\nsource = src/a.c\nsource = src/b.c\ninclude = include\n",
		"CMakeLists.txt":                         "add_executable(MyGame\n    src/main.c\n    src/a.c\n    src/b.c\n)\n",
		"src/main.c":                             "int a_value(void);\nint b_value(void);\nint main(void) { return a_value() + b_value() == 3 ? 0 : 1; }\n",
		"src/a.c":                                "#include \"shared.h\"\nint a_value(void) { return SHARED_ONE; }\n",
		"include/shared.h":                       "#include \"deep.h\"\n#define SHARED_ONE DEEP_ONE\n",
		"include/deep.h":                         "#define DEEP_ONE 1\n",
		"src/b.c":                                "#include \"only_b.h\"\nint b_value(void) { return B_TWO; }\n",
		"include/only_b.h":                       "#define B_TWO 2\n",
		"include/unrelated.h":                    "/* included by nothing */\n",
		"src/gen/gen_unit.c":                     "int generated_symbol;\n",
		"snesrecomp-go/runtime/src/runner.c":     "int runner_symbol;\n",
		"snesrecomp-go/runtime/include/runner.h": "/* public */\n",
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
	objectDir := filepath.Join(HermeticOutputDir(filepath.Join(root, "build"), ""), "obj")
	objectFor := func(relative string) string {
		return filepath.Join(objectDir, objectName(root, filepath.Join(root, filepath.FromSlash(relative))))
	}
	run := func() (string, error) {
		var log bytes.Buffer
		_, err := HermeticBuild(HermeticOptions{Paths: paths, ZigPath: zig, Jobs: 2,
			Stdout: &log, Stderr: &log, Verbose: true})
		return log.String(), err
	}
	build := func(want string) {
		t.Helper()
		log, err := run()
		if err != nil {
			t.Fatalf("build: %v\n%s", err, log)
		}
		if !strings.Contains(log, want) {
			t.Fatalf("want %q\n%s", want, log)
		}
	}
	buildFails := func(relative string) {
		t.Helper()
		if log, err := run(); err == nil {
			t.Fatalf("a broken unit built\n%s", log)
		}
		object := objectFor(relative)
		for _, leftover := range []string{object, compilerDepfilePath(object), dependencyRecordPath(object)} {
			if _, err := os.Stat(leftover); err == nil {
				t.Fatalf("the failed compile left %s behind", leftover)
			}
		}
	}
	age := 0
	edit := func(relative, contents string) {
		t.Helper()
		age++
		path := filepath.Join(root, filepath.FromSlash(relative))
		writeTestFile(t, path, contents)
		backdate(t, path, time.Duration(age)*time.Minute)
	}

	build("0 cached, 5 to compile")
	build("5 cached, 0 to compile")
	// Nothing includes this header. A whole-include-directory scan rebuilt
	// every game object for it.
	edit("include/unrelated.h", "/* still included by nothing */\n")
	build("5 cached, 0 to compile")
	edit("include/only_b.h", "#define B_TWO (1 + 1)\n")
	build("4 cached, 1 to compile")
	// Reached only through shared.h.
	edit("include/deep.h", "#define DEEP_ONE (2 - 1)\n")
	build("4 cached, 1 to compile")
	// A missing or damaged record costs its own object and nothing else.
	if err := os.Remove(dependencyRecordPath(objectFor("src/b.c"))); err != nil {
		t.Fatal(err)
	}
	build("4 cached, 1 to compile")
	writeTestFile(t, dependencyRecordPath(objectFor("src/a.c")), "{\"version\":")
	build("4 cached, 1 to compile")
	// A deleted header rebuilds its dependent, which then fails; nothing that
	// compile touched may be reused afterwards.
	if err := os.Remove(filepath.Join(root, "include", "only_b.h")); err != nil {
		t.Fatal(err)
	}
	buildFails("src/b.c")
	edit("include/only_b.h", "#define B_TWO 2\n")
	build("4 cached, 1 to compile")
	edit("src/b.c", "#include \"only_b.h\"\nint b_value(void) { return B_TWO }\n")
	buildFails("src/b.c")
	edit("src/b.c", "#include \"only_b.h\"\nint b_value(void) { return B_TWO; }\n")
	build("4 cached, 1 to compile")
}
