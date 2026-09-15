package emitter

import (
	"fmt"
	"os/exec"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestDiscardedCallReturnsToExactOuterOwnerNative(t *testing.T) {
	image := make(rom.Image, 0x20000)
	copy(image, []byte{0x22, 0, 0x81, 1, 0xe6, 0x10, 0x60})
	// Form a software long frame and dispatch the callback, keeping the outer
	// JSL owner. The callback discards its inner JSR then finishes that JSL.
	copy(image[0x8100:], []byte{0xf4, 0, 2, 0xab, 0xf4, 0xff, 0x81, 0x6b})
	copy(image[0x10200:], []byte{0xa5, 0x30, 0xf0, 7, 0x22, 0, 0x83, 3, 0x4c, 0x0e, 0x82, 0x20, 0, 0x83, 0xe6, 0x12, 0x6b})
	copy(image[0x10300:], []byte{0x68, 0x5c, 0, 0x84, 1})
	copy(image[0x18300:], []byte{0x68, 0xab, 0x5c, 0, 0x84, 1})
	copy(image[0x8400:], []byte{0xe6, 0x14, 0x6b})
	roots := []struct {
		name string
		pc   uint32
	}{{"Outer", 0x8000}, {"Driver", 0x018100}, {"Cleanup", 0x018400}, {"Callback", 0x028200}, {"Discard", 0x028300}, {"DiscardLong", 0x038300}}
	ctx := codegen.NewContext()
	for _, root := range roots {
		ctx.Names[root.pc] = root.name
		ctx.ValidVariants[root.pc] = map[[2]uint8]struct{}{{0, 0}: {}}
	}
	var source, decl, table, bytes strings.Builder
	for _, root := range roots {
		r, err := EmitFunction(image, byte(root.pc>>16), uint16(root.pc), 0, 0, FunctionOptions{Name: root.name, Codegen: ctx, UnresolvedAllowed: true})
		if err != nil {
			t.Fatal(err)
		}
		source.WriteString(r.Source)
		fmt.Fprintf(&decl, "RecompReturn %s_M0X0(CpuState*);\n", root.name)
		fmt.Fprintf(&table, "{0x%06xu,{%s_M0X0,NULL,NULL,NULL}},\n", root.pc, root.name)
	}
	for i, b := range image {
		if b != 0 {
			fmt.Fprintf(&bytes, "[%d]=%d,", i, b)
		}
	}
	if testing.Short() {
		t.Skip("native conformance")
	}
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("native conformance needs CMake")
	}
	runNativeContract(t, cmake, map[string]string{
		"generated.c": source.String(), "funcs.h": decl.String(),
		"CMakeLists.txt": `cmake_minimum_required(VERSION 3.20)
project(OwnedUnwind LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(owned_unwind fixture.c)
target_link_libraries(owned_unwind PRIVATE snesrecomp_runtime)
set_property(TARGET owned_unwind PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include <stdio.h>
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/generated_support.h"
#include "funcs.h"
#include "generated.c"
void RtlApuLock(void) {} void RtlApuUnlock(void) {}
extern unsigned sr_diagnostic_trap_warning_count(void);
static const uint8 fixture_rom[0x20000]={` + bytes.String() + `};
/* Deliberately no registry entry for either call's continuation. */
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 g_rom=fixture_rom;
 for(unsigned n=0;n<100;n++) for(unsigned mirror=0;mirror<2;mirror++) for(unsigned long_inner=0;long_inner<2;long_inner++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=0;cpu_p_to_mirrors(&cpu);
  cpu.S=0x1ffd;cpu.PB=mirror?0x80:0;cpu.host_return_valid=1;
  cpu_write16(&cpu,0,0x10,0);cpu_write16(&cpu,0,0x12,0);cpu_write16(&cpu,0,0x14,0);
  cpu_write16(&cpu,0,0x30,long_inner);
  cpu_write16(&cpu,0,0x1ffe,0x1234);WatchdogFrameStart();
  RecompReturn r=Outer_M0X0(&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||cpu.PB!=(mirror?0x80:0)||
     cpu_read16(&cpu,0,0x10)!=1||cpu_read16(&cpu,0,0x12)!=0||cpu_read16(&cpu,0,0x14)!=1||
     g_recomp_stack_top||g_cpu_return_scope||g_cpu_owned_unwind_scope||sr_diagnostic_trap_warning_count()) {
   fprintf(stderr,"mirror=%u r=%d S=%04x PB=%02x depth=%d outer=%u skipped=%u cleanup=%u\n",mirror,r,cpu.S,cpu.PB,g_recomp_stack_top,g_ram[0x10],g_ram[0x12],g_ram[0x14]);return 1;
  }
 }
 puts("discarded JSR/JSL, cross-bank shared RTL: exact outer JSL resumed once without continuation body: PASS");return 0;
}
`,
	}, "owned_unwind")
}
