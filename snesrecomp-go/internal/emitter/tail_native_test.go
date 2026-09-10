package emitter

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// Optional native conformance tier: emits original ROM bytes, compiles them
// against the real runner (not a dispatch mock), and executes both entry kinds.
// Pure-Go emission contracts above remain available without a native toolchain.
func TestSplitTailNativeExecution(t *testing.T) {
	if testing.Short() {
		t.Skip("native conformance build")
	}
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("native conformance needs CMake and a C/C++ toolchain")
	}
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xad, 0x10, 0x00, 0xd0, 0x01, 0x6b})     // Head: LDA dp; BNE Body; RTL
	copy(image[6:], []byte{0xca, 0xd0, 0x03, 0x6b, 0xea, 0xea}) // Body: DEX; BNE Loop; RTL
	copy(image[12:], []byte{0x20, 0x20, 0x80, 0x80, 0xf5})      // Loop: JSR Child; BRA Body
	copy(image[32:], []byte{0x80, 0x02, 0xea, 0xea, 0x60})      // Child: BRA ChildRet; RTS
	roots := []struct {
		name string
		pc   uint16
	}{{"Head", 0x8000}, {"Body", 0x8006}, {"Loop", 0x800c}, {"Child", 0x8020}, {"ChildRet", 0x8024}}
	ctx := codegen.NewContext()
	var entries []config.Entry
	var table strings.Builder
	for _, root := range roots {
		ctx.Names[uint32(root.pc)] = root.name
		fmt.Fprintf(&table, "{0x%04xu, {", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				entries = append(entries, config.Entry{Name: root.name, Start: root.pc, EntryMX: config.MX{M: m, X: x}})
				fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
			}
		}
		table.WriteString("}},\n")
	}
	source, err := EmitBank(image, 0, entries, BankOptions{Context: ctx})
	if err != nil {
		t.Fatal(err)
	}
	files := map[string]string{
		"generated.c": source, "funcs.h": "/* synthetic fixture forward declarations are in generated.c */\n",
		"CMakeLists.txt": `cmake_minimum_required(VERSION 3.20)
project(SplitTailConformance LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(tail_contract fixture.c)
target_link_libraries(tail_contract PRIVATE snesrecomp_runtime)
set_property(TARGET tail_contract PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "generated.c"
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
static unsigned continuation_calls;
static RecompReturn forbidden_continuation(CpuState *cpu) {
    (void)cpu; ++continuation_calls; return RECOMP_RETURN_NORMAL;
}
const DispatchEntry g_dispatch_table[] = {
{0x001235u,{forbidden_continuation,forbidden_continuation,forbidden_continuation,forbidden_continuation}},
` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 for (unsigned mx=0;mx<4;mx++) for (unsigned taken=0;taken<2;taken++) {
    CpuState cpu; cpu_state_init(&cpu,g_ram); cpu.emulation=0;
    cpu.P=(uint8)((mx&2?CPU_P_M:0)|(mx&1?CPU_P_X:0)); cpu_p_to_mirrors(&cpu);
    cpu.S=0x1ffcu; cpu.X=(mx&1)?200u:10000u; uint16 old_x=cpu.X;
    cpu.PB=0; cpu.host_return_valid=1; g_ram[0x10]=(uint8)taken; g_ram[0x11]=0;
    cpu_write8(&cpu,0,0x1ffd,0x34); cpu_write8(&cpu,0,0x1ffe,0x12); cpu_write8(&cpu,0,0x1fff,0);
    WatchdogFrameStart();
    RecompReturn result=g_dispatch_table[1].variant[mx](&cpu);
    if(result!=RECOMP_RETURN_NORMAL || continuation_calls || cpu.S!=0x1fff ||
       g_recomp_stack_top!=0 || cpu.X!=(taken?0:old_x) ||
       cpu.m_flag!=((mx>>1)&1) || cpu.x_flag!=(mx&1)) return 1;
    /* Registry entry is unpaired: its real hardware continuation executes
     * once. A paired C call above must never dispatch that continuation. */
    cpu.S=0x1ffc; cpu.X=3; g_ram[0x10]=1; cpu.host_return_valid=0;
    result=cpu_dispatch_pc_from(&cpu,0x008000,0x1ffc,0x00abcd);
    if(result!=RECOMP_RETURN_NORMAL || continuation_calls!=1 || cpu.S!=0x1fff ||
       g_recomp_stack_top!=0 || cpu.X!=0) return 2;
    continuation_calls=0;
 }
 puts("native split tails: all M/X states, taken/untaken, paired/unpaired, long chains and nested calls PASS");
 return 0;
}
`,
	}
	runNativeContract(t, cmake, files, "tail_contract")
}

func runNativeContract(t *testing.T, cmake string, files map[string]string, target string) {
	t.Helper()
	dir := t.TempDir()
	runtimeRoot, err := filepath.Abs("../../runtime")
	if err != nil {
		t.Fatal(err)
	}
	for name, contents := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(contents), 0600); err != nil {
			t.Fatal(err)
		}
	}
	commands := [][]string{
		{"-S", dir, "-B", filepath.Join(dir, "build"), "-DCMAKE_BUILD_TYPE=Release", "-DSNESRECOMP_RUNTIME_ROOT=" + filepath.ToSlash(runtimeRoot)},
		{"--build", filepath.Join(dir, "build"), "--config", "Release", "--target", target, "--parallel", "2"},
	}
	deadline, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	for _, args := range commands {
		if out, err := exec.CommandContext(deadline, cmake, args...).CombinedOutput(); err != nil {
			t.Fatalf("native fixture build: %v\n%s", err, out)
		}
	}
	binary := ""
	for _, candidate := range []string{target, target + ".exe", "Release/" + target + ".exe"} {
		path := filepath.Join(dir, "build", filepath.FromSlash(candidate))
		if _, err := os.Stat(path); err == nil {
			binary = path
			break
		}
	}
	if binary == "" {
		t.Fatal("native fixture executable not found")
	}
	if out, err := exec.CommandContext(deadline, binary).CombinedOutput(); err != nil {
		t.Fatalf("native fixture execution: %v\n%s", err, out)
	} else {
		t.Log(string(out))
	}
}
