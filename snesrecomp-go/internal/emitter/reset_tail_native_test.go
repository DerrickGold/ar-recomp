package emitter

import (
	"fmt"
	"os/exec"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestResetTailRetiresCompiledActivations(t *testing.T) {
	if testing.Short() {
		t.Skip("native conformance")
	}
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("native conformance needs CMake")
	}
	image := make(rom.Image, 0x10000)
	copy(image, []byte{0xe6, 0x10, 0x20, 0, 0x81, 0xe6, 0x12, 0xdb})
	copy(image[0x100:], []byte{0x4c, 0x10, 0x81})
	copy(image[0x110:], []byte{0x22, 0, 0x82, 1, 0xe6, 0x14, 0x60})
	copy(image[0x8200:], []byte{0x6b})
	ctx := codegen.NewContext()
	entries := map[byte][]config.Entry{}
	var table, decl, bytes strings.Builder
	for _, root := range []struct {
		name string
		pc   uint32
	}{{"Root", 0x8000}, {"Middle", 0x8100}, {"Split", 0x8110}, {"Transfer", 0x018200}} {
		ctx.Names[root.pc] = root.name
		ctx.ValidVariants[root.pc] = map[[2]uint8]struct{}{{0, 0}: {}}
		entries[byte(root.pc>>16)] = append(entries[byte(root.pc>>16)], config.Entry{Name: root.name, Start: uint16(root.pc), EntryMX: config.MX{M: 0, X: 0}})
		fmt.Fprintf(&table, "{0x%06xu,{%s_M0X0,NULL,NULL,NULL}},\n", root.pc, root.name)
		fmt.Fprintf(&decl, "RecompReturn %s_M0X0(CpuState*);\n", root.name)
	}
	source, err := EmitBank(image, 0, entries[0], BankOptions{Context: ctx})
	if err != nil {
		t.Fatal(err)
	}
	other, err := EmitBank(image, 1, entries[1], BankOptions{Context: ctx, HLEFunctions: map[uint16]string{0x8200: "transfer"}})
	if err != nil {
		t.Fatal(err)
	}
	for i, b := range image {
		if b != 0 {
			fmt.Fprintf(&bytes, "[%d]=%d,", i, b)
		}
	}
	runNativeContract(t, cmake, map[string]string{
		"generated.c": source + other, "funcs.h": decl.String(),
		"CMakeLists.txt": `cmake_minimum_required(VERSION 3.20)
project(ResetTail LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(reset_tail fixture.c)
target_link_libraries(reset_tail PRIVATE snesrecomp_runtime)
set_property(TARGET reset_tail PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include <assert.h>
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/generated_support.h"
#include "funcs.h"
#include "generated.c"
void RtlApuLock(void) {} void RtlApuUnlock(void) {}
static unsigned requests;
RecompReturn transfer(CpuState *cpu) {
 assert(g_recomp_stack_top==3 && cpu->S==0x1ffa && cpu->PB==1);
 if(++requests==200) {cpu->S+=3;return RECOMP_RETURN_NORMAL;}
 cpu->S=0x1fff; /* Explicit native terminal stack-reset semantics. */
 assert(cpu_begin_reset_tail(cpu,0x008000,0x018200));
 return RECOMP_RETURN_OWNED_UNWIND;
}
static const uint8 fixture_rom[0x10000]={` + bytes.String() + `};
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 g_rom=fixture_rom;CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=0;cpu_p_to_mirrors(&cpu);
 cpu.S=0x1fff;cpu.host_return_valid=0;WatchdogFrameStart();
 CpuReturnScope root;cpu_reset_scope_begin(&root,&cpu);
 RecompReturn result=Root_M0X0(&cpu);
 while(result==RECOMP_RETURN_TAILCALL || (result==RECOMP_RETURN_OWNED_UNWIND && cpu_finish_reset_tail(&root,&cpu)))
   result=cpu_dispatch_pc_from(&cpu,g_tailcall_pc24,g_tailcall_miss_s,g_tailcall_src24);
 cpu_return_scope_end(&root);
 assert(result==RECOMP_RETURN_NORMAL && requests==200 && !g_recomp_stack_top && !g_cpu_return_scope && !g_cpu_owned_unwind_scope);
 assert(cpu.S==0x1fff && cpu.PB==0 && cpu_read16(&cpu,0,0x10)==200 && cpu_read16(&cpu,0,0x12)==1 && cpu_read16(&cpu,0,0x14)==1);
 puts("200 reset-root transfers: flat compiled depth, dead callers never resume, final real return preserved: PASS");return 0;
}
`,
	}, "reset_tail")
}
