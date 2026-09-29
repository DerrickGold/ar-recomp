package project

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/emitter"
	"github.com/DerrickGold/snesrecomp-go/internal/regen"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
)

// syntheticGeneratedROM is a two-bank LoROM image whose code reaches every
// declaration shape generated units produce: automatic vectors, a cross-bank
// JSL, named aliases, an unconditional and a conditional HLE entry, a
// multi-owner shared continuation (external helper) and a single-owner
// shared region (private helper). Each native path increments a WRAM
// counter. Every instruction has the same length at every M/X width, so the
// sibling variants the live-width call switches demand decode cleanly too.
func syntheticGeneratedROM() []byte {
	image := make([]byte, 0x10000)
	put := func(offset int, code ...byte) { copy(image[offset:], code) }
	put(0x0000, // $00:8000 reset (I_RESET, M1X1)
		0x22, 0x00, 0x80, 0x01, // JSL $01:8000 Far
		0x20, 0x20, 0x80, // JSR $8020 Leaf
		0x20, 0x30, 0x80, // JSR $8030 HostRoutineSite (hle_func)
		0x20, 0x40, 0x80, // JSR $8040 Gated (hle_func_if)
		0x20, 0x50, 0x80, // JSR $8050 RootA
		0x20, 0x60, 0x80, // JSR $8060 RootB
		0x20, 0x80, 0x80, // JSR $8080 Parent
		0x60) // RTS
	put(0x0020, 0xe6, 0x10, 0x60) // Leaf: INC $10; RTS
	put(0x0030, 0x60)             // HostRoutineSite: replaced by HostRoutine
	put(0x0040, 0xe6, 0x11, 0x60) // Gated native body: INC $11; RTS
	put(0x0050, 0x80, 0x1e)       // RootA: BRA $8070
	put(0x0060, 0x80, 0x0e)       // RootB: BRA $8070
	put(0x0070, 0xe6, 0x12, 0x60) // shared continuation: INC $12; RTS
	put(0x0080, 0x80, 0x0e)       // Parent: BRA $8090
	put(0x0090, 0xe6, 0x13, 0x60) // region continuation: INC $13; RTS
	put(0x00a0, 0x40)             // native NMI: RTI
	put(0x00b0, 0x40)             // native IRQ: RTI
	put(0x8000, 0xe6, 0x14, 0x6b) // $01:8000 Far: INC $14; RTL
	// A plainly LoROM header, so mapping cannot drift to the HiROM candidate.
	image[0x7fc0] = 'S'
	image[0x7fd5] = 0x20
	image[0x7fdc], image[0x7fdd] = 0xff, 0xff
	image[0x7fea], image[0x7feb] = 0xa0, 0x80 // native NMI
	image[0x7fee], image[0x7fef] = 0xb0, 0x80 // native IRQ
	image[0x7ffc], image[0x7ffd] = 0x00, 0x80 // reset
	return image
}

func syntheticContinuationFacts() []analysis.EntryFact {
	variant := func(pc uint32) analysis.EntryVariant {
		return analysis.EntryVariant{PC: pc, EntryMX: analysis.MXState{M: 1, X: 1}}
	}
	proven := []analysis.Evidence{{Source: "static.sibling_boundary_edge", Confidence: analysis.ConfidenceProven}}
	ownerA, ownerB, shared := variant(0x008050), variant(0x008060), variant(0x008070)
	parent, child := variant(0x008080), variant(0x008090)
	return []analysis.EntryFact{
		{
			PC: shared.PC, EntryMX: shared.EntryMX, Kind: analysis.EntryContinuation, TemplateFree: true,
			RegionOwners: []analysis.EntryVariant{ownerA, ownerB},
			ResumeEdges: []analysis.EntryEdge{
				{Source: ownerA, Target: shared, RegionOwner: &ownerA},
				{Source: ownerB, Target: shared, RegionOwner: &ownerB},
			},
			Evidence: proven,
		},
		{
			PC: child.PC, EntryMX: child.EntryMX, Kind: analysis.EntryContinuation, TemplateFree: true,
			RegionOwners: []analysis.EntryVariant{parent},
			ResumeEdges:  []analysis.EntryEdge{{Source: parent, Target: child}},
			Evidence:     proven,
		},
	}
}

const syntheticMain = `#include <stdio.h>
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "funcs.h"

extern unsigned g_host_routine_calls, g_host_gated_calls;
extern bool g_host_predicate;
extern const uint8 g_synthetic_rom[0x10000];

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static int failed(const char *what) {
  fprintf(stderr, "synthetic generated units: %s\n", what);
  return 1;
}

/* Enter a void alias from funcs.h the way a host does: a paired JSR frame
 * for a routine, a native interrupt frame for a vector. */
static int enter(void (*alias)(CpuState *), int m, int x, int interrupt) {
  CpuState cpu;
  cpu_state_init(&cpu, g_ram);
  cpu.emulation = 0;
  cpu.P = (uint8)((m ? 0x20 : 0) | (x ? 0x10 : 0));
  cpu_p_to_mirrors(&cpu);
  cpu.S = 0x1ff0;
  if (interrupt) cpu_push_interrupt_frame(&cpu);
  else cpu_push_jsr_return_frame(&cpu);
  WatchdogFrameStart();
  alias(&cpu);
  if (cpu.S != 0x1ff0) return failed("stack not balanced");
  if (g_recomp_stack_top != 0) return failed("activation stack not unwound");
  return 0;
}

int main(void) {
  g_rom = g_synthetic_rom;
  for (unsigned address = 0x10; address <= 0x14; ++address) g_ram[address] = 0;
  /* Leaf, Gated, the shared tail (via RootA and RootB), the region, Far. */
  g_host_predicate = true;
  if (enter(I_RESET, 1, 1, 0)) return 1;
  if (g_ram[0x10] != 1 || g_ram[0x12] != 2 || g_ram[0x13] != 1 || g_ram[0x14] != 1)
    return failed("native paths");
  if (g_host_routine_calls != 1 || g_host_gated_calls != 1 || g_ram[0x11] != 0)
    return failed("HLE with its predicate true");
  g_host_predicate = false;
  if (enter(I_RESET, 1, 1, 0)) return 1;
  if (g_ram[0x10] != 2 || g_ram[0x12] != 4 || g_ram[0x13] != 2 || g_ram[0x14] != 2)
    return failed("native paths, second entry");
  if (g_host_routine_calls != 2 || g_host_gated_calls != 1 || g_ram[0x11] != 1)
    return failed("HLE with its predicate false");
  if (enter(I_NMI, 0, 0, 1) || enter(I_IRQ, 0, 0, 1)) return 1;
  /* Named aliases compiled in other units, used through funcs.h. */
  void (*aliases[])(CpuState *) = { Far, Leaf, HostRoutineSite, Gated, RootA, RootB, Parent };
  (void)aliases;
  puts("synthetic generated units: PASS");
  return 0;
}
`

func syntheticHost(image []byte) string {
	var rom strings.Builder
	for index, value := range image {
		if value != 0 {
			fmt.Fprintf(&rom, "[%d]=%d,", index, value)
		}
	}
	return `#include <stdbool.h>
#include "snesrecomp/game/cpu.h"

/* Authored HLE bodies. Deliberately no funcs.h: this unit is not a consumer. */
unsigned g_host_routine_calls, g_host_gated_calls;
bool g_host_predicate;
RecompReturn HostRoutine(CpuState *cpu) { (void)cpu; ++g_host_routine_calls; return RECOMP_RETURN_NORMAL; }
bool HostPredicate(CpuState *cpu) { (void)cpu; return g_host_predicate; }
RecompReturn HostGated(CpuState *cpu) { (void)cpu; ++g_host_gated_calls; return RECOMP_RETURN_NORMAL; }
const uint8 g_synthetic_rom[0x10000] = {` + rom.String() + `};
`
}

// strictCompile compiles one unit with every missing or mismatched
// declaration as an error. The build's own flags (-w,
// -Wno-implicit-function-declaration) would let an implicit int slip through.
func strictCompile(zig, source, object string, includes ...string) ([]byte, error) {
	args := []string{"cc", "-std=gnu11", "-O0",
		"-Werror=implicit-function-declaration", "-Werror=implicit-int",
		"-Werror=incompatible-function-pointer-types", "-Werror=incompatible-pointer-types",
		"-Werror=int-conversion"}
	for _, include := range includes {
		args = append(args, "-I"+include)
	}
	args = append(args, "-c", source, "-o", object)
	return exec.Command(zig, args...).CombinedOutput()
}

var compileCountRE = regexp.MustCompile(`\((\d+) cached, (\d+) to compile`)

// dividedUnitRE names a piece of a chunk divided under the unit size limit.
var dividedUnitRE = regexp.MustCompile(`^bank[0-9a-f]{2}_part[0-9a-f]{2}_[0-9a-f]{4}(_\d{2})?_v2\.c$`)

// EmitBank's default options must retain the demands of every function, not
// discard them in the temporary contexts EmitFunction otherwise creates.
func TestStandaloneBankDefaultContextCompilesWithoutFuncsHeader(t *testing.T) {
	if testing.Short() {
		t.Skip("strict native compilation of standalone bank output")
	}
	zig := dependencyTestZig(t)
	scratch := t.TempDir()
	t.Setenv("ZIG_GLOBAL_CACHE_DIR", filepath.Join(scratch, "zig-global"))
	t.Setenv("ZIG_LOCAL_CACHE_DIR", filepath.Join(scratch, "zig-local"))
	runtimeInclude, err := filepath.Abs("../../runtime/include")
	if err != nil {
		t.Fatal(err)
	}
	image := make(rom.Image, 0x10000)
	copy(image, []byte{0x22, 0x00, 0x80, 0x01, 0x60})        // JSL $01:8000; RTS
	copy(image[0x10:], []byte{0x22, 0x10, 0x80, 0x01, 0x60}) // JSL $01:8010; RTS
	source, err := emitter.EmitBank(image, 0, []config.Entry{
		{Name: "First", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}},
		{Name: "Second", Start: 0x8010, EntryMX: config.MX{M: 1, X: 1}},
	}, emitter.BankOptions{})
	if err != nil {
		t.Fatal(err)
	}
	sourcePath := filepath.Join(scratch, "generated.c")
	writeTestFile(t, sourcePath, source)
	if output, err := strictCompile(zig, sourcePath, sourcePath+".o", runtimeInclude); err != nil {
		t.Fatalf("default bank options lost external declarations: %v\n%s", err, output)
	}
}

// Generated translation units must compile and link with no funcs.h at all,
// declare exactly what they use with the definitions' own signatures, and
// stop depending on funcs.h, while authored code keeps using it.
func TestGeneratedUnitsNeedNoFuncsHeader(t *testing.T) {
	if testing.Short() {
		t.Skip("real compile, link and run of regenerated units")
	}
	zig := dependencyTestZig(t)
	scratch := t.TempDir()
	t.Setenv("ZIG_GLOBAL_CACHE_DIR", filepath.Join(scratch, "zig-global"))
	t.Setenv("ZIG_LOCAL_CACHE_DIR", filepath.Join(scratch, "zig-local"))
	sdk, err := filepath.Abs("../..")
	if err != nil {
		t.Fatal(err)
	}
	runtimeInclude := filepath.Join(sdk, "runtime", "include")

	root := filepath.Join(scratch, "synthetic game")
	image := syntheticGeneratedROM()
	romPath := filepath.Join(root, "game.sfc")
	cfgDir := filepath.Join(root, "recomp")
	writeTestFile(t, romPath, string(image))
	writeTestFile(t, filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nauto_vectors\n"+
		"func Leaf 8020 entry_mx:1,1\n"+
		"func HostRoutineSite 8030 entry_mx:1,1\nhle_func 8030 HostRoutine\n"+
		"func Gated 8040 entry_mx:1,1\nhle_func_if 8040 HostGated HostPredicate\n"+
		"func RootA 8050 entry_mx:1,1\nfunc RootB 8060 entry_mx:1,1\nfunc bank_00_8070 8070 entry_mx:1,1\n"+
		"func Parent 8080 entry_mx:1,1\nfunc bank_00_8090 8090 entry_mx:1,1\n")
	writeTestFile(t, filepath.Join(cfgDir, "bank01.cfg"), "bank = 01\nfunc Far 8000 entry_mx:1,1\n")
	facts := syntheticContinuationFacts()

	// The authored declaration surface lives outside every generated
	// directory and outside every include path the strict checks use.
	includeDir := filepath.Join(scratch, "authored include")
	funcsHeader := filepath.Join(includeDir, "funcs.h")
	if _, err := tooling.SyncFuncsWithEntryFacts(cfgDir, funcsHeader, facts); err != nil {
		t.Fatal(err)
	}

	shapes := []struct {
		name      string
		threshold int
		span      int
		unitLimit int
	}{
		{"split", 1, 0x10, 0},      // one chunk per function: every edge crosses units
		{"unsplit", 1 << 30, 0, 0}, // one unit per bank
		// Default-width chunks divided to the finest level a one-byte unit
		// limit allows: single functions, and regions kept whole.
		{"divided", 1, 0x800, 1},
	}
	outputs := make(map[string]string)
	for _, shape := range shapes {
		out := filepath.Join(scratch, "generated "+shape.name)
		options := regen.Options{
			ROMPath: romPath, ConfigDir: cfgDir, OutputDir: out, Jobs: 1, AllowStubs: true,
			ProvenEntryFacts: facts, ChunkThresholdBytes: shape.threshold, ChunkPCSpan: shape.span,
			MaxUnitBytes: shape.unitLimit,
		}
		report, err := regen.Run(options)
		if err != nil {
			t.Fatalf("%s regen: %v", shape.name, err)
		}
		// Unchanged input regenerates byte-identical units.
		if again, err := regen.Run(options); err != nil || again.ChangedFiles != 0 || again.Files != report.Files {
			t.Fatalf("%s: a second identical regen changed %d of %d files: %v",
				shape.name, again.ChangedFiles, again.Files, err)
		}
		if report.SharedContinuationBodies != 1 || report.SharedRegionBodies != 1 {
			t.Fatalf("%s: shared continuation/region bodies = %d/%d, want 1/1",
				shape.name, report.SharedContinuationBodies, report.SharedRegionBodies)
		}
		units, err := filepath.Glob(filepath.Join(out, "*.c"))
		if err != nil {
			t.Fatal(err)
		}
		sort.Strings(units)
		all := ""
		for _, unit := range units {
			data, err := os.ReadFile(unit)
			if err != nil {
				t.Fatal(err)
			}
			if strings.Contains(string(data), "funcs.h") {
				t.Fatalf("%s: %s still refers to funcs.h", shape.name, filepath.Base(unit))
			}
			all += string(data)
		}
		for _, fragment := range []string{
			"void I_RESET(CpuState *cpu) {", "void Far(CpuState *cpu) {",
			"extern RecompReturn HostRoutine(CpuState *cpu);", "extern bool HostPredicate(CpuState *cpu);",
			"RecompReturn sr_continuation_00_8070_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry) {",
			"static inline RecompReturn sr_region_00_8080_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry) {",
			"const DispatchEntry g_dispatch_table[]",
		} {
			if !strings.Contains(all, fragment) {
				t.Fatalf("%s output lacks %q", shape.name, fragment)
			}
		}
		if shape.name == "split" {
			// The external helper must be used from a unit that does not define it.
			declaredOnly := 0
			for _, unit := range units {
				data, _ := os.ReadFile(unit)
				text := string(data)
				if strings.Contains(text, "RecompReturn sr_continuation_00_8070_M1X1(CpuState *cpu, uint16 _entry_s, uint8 _hrv, uint16 _region_entry);") &&
					!strings.Contains(text, "_region_entry) {") {
					declaredOnly++
				}
			}
			if declaredOnly < 2 || len(units) < 8 {
				t.Fatalf("split output does not exercise cross-unit helpers: %d units, %d declaration-only users", len(units), declaredOnly)
			}
		}
		if shape.name == "divided" {
			// Bank 0 is one default-width chunk, divided into pieces; the
			// private region body shares a unit with every wrapper of it, and
			// that region is reported rather than split.
			pieces, regionUsers := 0, 0
			for _, unit := range units {
				name := filepath.Base(unit)
				if dividedUnitRE.MatchString(name) {
					pieces++
				}
				data, _ := os.ReadFile(unit)
				if strings.Contains(string(data), "sr_region_00_8080_M1X1") {
					regionUsers++
				}
			}
			reported := strings.Join(report.OversizedUnits, "\n")
			if pieces < 8 || regionUsers != 1 || !strings.Contains(reported, "resumable region owned by $8080") {
				t.Fatalf("divided output: %d pieces, %d units use the private region body, report:\n%s",
					pieces, regionUsers, reported)
			}
		}

		// Every unit on its own, with no funcs.h reachable.
		objects := filepath.Join(scratch, "strict "+shape.name)
		if err := os.MkdirAll(objects, 0o755); err != nil {
			t.Fatal(err)
		}
		for _, unit := range units {
			if output, err := strictCompile(zig, unit, filepath.Join(objects, filepath.Base(unit)+".o"), runtimeInclude); err != nil {
				t.Fatalf("%s: %s does not compile without funcs.h: %v\n%s", shape.name, filepath.Base(unit), err, output)
			}
		}
		// All units and the authored surface together: any two declarations
		// of one symbol that disagree are a hard error here.
		var amalgam strings.Builder
		amalgam.WriteString("#include \"funcs.h\"\n")
		for _, unit := range units {
			fmt.Fprintf(&amalgam, "#include %q\n", unit)
		}
		amalgamPath := filepath.Join(scratch, "amalgam "+shape.name+".c")
		writeTestFile(t, amalgamPath, amalgam.String())
		if output, err := strictCompile(zig, amalgamPath, amalgamPath+".o", includeDir, runtimeInclude); err != nil {
			t.Fatalf("%s: generated and authored declarations disagree: %v\n%s", shape.name, err, output)
		}
		outputs[shape.name] = out
	}
	// The strict flags are not vacuous.
	for name, source := range map[string]string{
		"implicit": "#include \"snesrecomp/game/cpu.h\"\nRecompReturn f(CpuState *cpu) { return undeclared_symbol(cpu); }\n",
		"conflict": "#include \"snesrecomp/game/cpu.h\"\nvoid Leaf(CpuState *cpu);\nRecompReturn Leaf(CpuState *cpu) { (void)cpu; return RECOMP_RETURN_NORMAL; }\n",
	} {
		path := filepath.Join(scratch, "control "+name+".c")
		writeTestFile(t, path, source)
		if _, err := strictCompile(zig, path, path+".o", runtimeInclude); err == nil {
			t.Fatalf("strict compile accepted the %s control", name)
		}
	}

	// Link and run both shapes against the real runtime, authored code using
	// funcs.h. The game archive is force-loaded, so every generated reference
	// must resolve.
	writeTestFile(t, filepath.Join(root, "snesbuild.ini"), "[project]\nname = SynthGame\nlink = -lm\nsource = src/main.c\nsource = src/host.c\n")
	writeTestFile(t, filepath.Join(root, "CMakeLists.txt"), "add_executable(SynthGame\n    src/main.c\n    src/host.c\n)\n")
	writeTestFile(t, filepath.Join(root, "src", "main.c"), syntheticMain)
	writeTestFile(t, filepath.Join(root, "src", "host.c"), syntheticHost(image))
	for _, relative := range []string{"src/main.c", "src/host.c"} {
		backdate(t, filepath.Join(root, filepath.FromSlash(relative)), time.Hour)
	}
	backdate(t, funcsHeader, time.Hour)
	build := func(generated string) (string, string, error) {
		paths := DefaultPaths(root)
		paths.GeneratedDir, paths.FuncsHeader = generated, funcsHeader
		paths.BuildDir, paths.ToolchainDir = filepath.Join(scratch, "build"), sdk
		var log bytes.Buffer
		binary, err := HermeticBuild(HermeticOptions{Paths: paths, ZigPath: zig, Jobs: 4, Optimize: "-O0",
			Stdout: &log, Stderr: &log, Verbose: true})
		return binary, log.String(), err
	}
	run := func(binary string) {
		t.Helper()
		if output, err := exec.Command(binary).CombinedOutput(); err != nil || !strings.Contains(string(output), "PASS") {
			t.Fatalf("synthetic game: %v\n%s", err, output)
		}
	}
	for _, shape := range []string{"split", "unsplit", "divided"} {
		binary, log, err := build(outputs[shape])
		if err != nil {
			t.Fatalf("%s build: %v\n%s", shape, err, log)
		}
		run(binary)
	}

	// A reference with no definition anywhere must fail the link: remove the
	// unit defining bank 1 from a copy of the unsplit output.
	incomplete := filepath.Join(scratch, "generated incomplete")
	units, _ := filepath.Glob(filepath.Join(outputs["unsplit"], "*.c"))
	for _, unit := range units {
		if filepath.Base(unit) == "bank01_v2.c" {
			continue
		}
		data, err := os.ReadFile(unit)
		if err != nil {
			t.Fatal(err)
		}
		writeTestFile(t, filepath.Join(incomplete, filepath.Base(unit)), string(data))
	}
	if _, log, err := build(incomplete); err == nil || !strings.Contains(log, "Far_M1X1") {
		t.Fatalf("a missing generated definition linked: %v\n%s", err, log)
	}

	// Editing funcs.h rebuilds its authored consumer and nothing else: not the
	// generated units, not host.c, not the runtime.
	binary, log, err := build(outputs["split"])
	if err != nil {
		t.Fatalf("rebuild: %v\n%s", err, log)
	}
	if match := compileCountRE.FindStringSubmatch(log); match == nil || match[2] != "0" {
		t.Fatalf("unchanged inputs recompiled:\n%s", log)
	}
	header, err := os.ReadFile(funcsHeader)
	if err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, funcsHeader, string(header)+"/* edited by an authored-code change */\n")
	backdate(t, funcsHeader, 30*time.Minute)
	binary, log, err = build(outputs["split"])
	if err != nil {
		t.Fatalf("build after editing funcs.h: %v\n%s", err, log)
	}
	match := compileCountRE.FindStringSubmatch(log)
	if match == nil || match[2] != "1" || !strings.Contains(log, "  cc "+filepath.Join(root, "src", "main.c")) {
		t.Fatalf("editing funcs.h must rebuild only src/main.c:\n%s", log)
	}
	run(binary)
}
