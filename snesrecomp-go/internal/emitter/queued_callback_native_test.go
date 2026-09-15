package emitter

import (
	"fmt"
	"os/exec"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestQueuedLongCallbackNativeContinuation(t *testing.T) {
	image := make(rom.Image, 0x18000)
	copy(image, []byte{0xa9, 0, 0x92, 0x22, 0, 0x81, 1, 0x22, 0, 0x83, 2, 0xe6, 0x24, 0x60})
	copy(image[0x8100:], []byte{0x4c, 0, 0x82})
	copy(image[0x8200:], []byte{0xac, 0, 0x10, 0x99, 0, 0x12, 0xa3, 3, 0x99, 2, 0x12, 0xa5, 0x70, 0x99, 3, 0x12, 0x6b})
	copy(image[0x10300:], []byte{0xac, 0, 0x10, 0xb9, 0, 0x12, 0x85, 0x42, 0xb9, 2, 0x12, 0x85, 0x44, 0xbe, 3, 0x12, 0x86, 0x70, 0x4b, 0xf4, 0x18, 0x83, 0xdc, 0x42, 0, 0xe6, 0x22, 0x6b})
	copy(image[0x1200:], []byte{0xe6, 0x20, 0x6b})
	roots := []struct {
		name string
		pc   uint32
	}{{"Outer", 0x8000}, {"Callback", 0x9200}, {"Enqueue", 0x018100}, {"Dequeue", 0x028300}, {"Resume", 0x028319}}
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
project(QueuedCallback LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(queued_callback fixture.c)
target_link_libraries(queued_callback PRIVATE snesrecomp_runtime)
set_property(TARGET queued_callback PROPERTY LINKER_LANGUAGE CXX)
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
static const uint8 fixture_rom[0x18000]={` + bytes.String() + `};
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 g_rom=fixture_rom;
 for(unsigned n=0;n<100;n++) for(unsigned mirror=0;mirror<2;mirror++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=0;cpu_p_to_mirrors(&cpu);
  cpu.S=0x1ffd;cpu.PB=mirror?0x80:0;cpu.host_return_valid=1;
  cpu_write16(&cpu,0,0x1000,0);cpu_write16(&cpu,0,0x70,0x1234);
  cpu_write16(&cpu,0,0x20,0);cpu_write16(&cpu,0,0x22,0);cpu_write16(&cpu,0,0x24,0);
  cpu_write16(&cpu,0,0x1ffe,0x1234);WatchdogFrameStart();
  RecompReturn r=Outer_M0X0(&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||cpu.PB!=(mirror?0x80:0)||
     cpu_read16(&cpu,0,0x20)!=1||cpu_read16(&cpu,0,0x22)!=1||cpu_read16(&cpu,0,0x24)!=1||
     cpu_read16(&cpu,0,0x1200)!=0x9200||cpu_read8(&cpu,0,0x1202)!=(mirror?0x80:0)||
     cpu_read16(&cpu,0,0x1203)!=0x1234||g_recomp_stack_top||g_cpu_return_scope||sr_diagnostic_trap_warning_count()) {
   fprintf(stderr,"mirror=%u r=%d S=%04x PB=%02x depth=%d callback=%u resume=%u outer=%u bank=%02x\n",mirror,r,cpu.S,cpu.PB,g_recomp_stack_top,g_ram[0x20],g_ram[0x22],g_ram[0x24],g_ram[0x1202]);return 1;
  }
 }
 puts("queued long callback: JSL bank, overlapping metadata, exactly-once callback/continuation/outer cleanup: PASS");return 0;
}
`,
	}, "queued_callback")
}
