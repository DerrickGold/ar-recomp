package emitter

import (
	"fmt"
	"os/exec"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestUnpairedRTLDispatchInstallsActualTargetBank(t *testing.T) {
	image := make(rom.Image, 0x10000)
	image[0x8400] = 0x6b
	copy(image[0x1400:], []byte{0x7c, 0, 0x95})
	copy(image[0x1500:], []byte{0, 0x96})
	copy(image[0x1600:], []byte{0xe6, 0x20, 0x08, 0xe2, 0x20, 0x4b, 0x68, 0x85, 0x22, 0x28, 0x6b})
	// The same table offset in the dispatcher's old bank is deliberately invalid.
	copy(image[0x9500:], []byte{0xff, 0xff})
	roots := []struct {
		name string
		pc   uint32
	}{{"Jump", 0x009400}, {"Handler", 0x009600}, {"Return", 0x018400}}
	ctx := codegen.NewContext()
	for _, root := range roots {
		ctx.Names[root.pc] = root.name
	}
	var source, decl, table, bytes strings.Builder
	for _, root := range roots {
		fmt.Fprintf(&table, "{0x%06xu,{", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				r, err := EmitFunction(image, byte(root.pc>>16), uint16(root.pc), m, x, FunctionOptions{Name: root.name, Codegen: ctx, UnresolvedAllowed: true})
				if err != nil {
					t.Fatal(err)
				}
				source.WriteString(r.Source)
				fmt.Fprintf(&decl, "RecompReturn %s_M%dX%d(CpuState*);\n", root.name, m, x)
				fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
			}
		}
		table.WriteString("}},\n")
	}
	table.WriteString("{0x018500u,{Stop,Stop,Stop,Stop}},\n")
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
project(DispatchBank LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(dispatch_bank fixture.c)
target_link_libraries(dispatch_bank PRIVATE snesrecomp_runtime)
set_property(TARGET dispatch_bank PROPERTY LINKER_LANGUAGE CXX)
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
static unsigned stopped,wrong_bank;
static RecompReturn Stop(CpuState *cpu) { ++stopped; if(cpu->PB!=1||cpu->S!=0x1fff) ++wrong_bank;return RECOMP_RETURN_NORMAL; }
static const uint8 fixture_rom[0x10000]={` + bytes.String() + `};
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 g_rom=fixture_rom;
 for(unsigned n=0;n<100;n++) for(unsigned mx=0;mx<4;mx++) for(unsigned mirror=0;mirror<2;mirror++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=(mx&2?0x20:0)|(mx&1?0x10:0);cpu_p_to_mirrors(&cpu);
  cpu.S=0x1ff9;cpu.PB=0x7e;cpu.X=0;stopped=wrong_bank=0;
  cpu_write16(&cpu,0,0x20,0);cpu_write8(&cpu,0,0x22,0xff);
  cpu_write16(&cpu,0,0x1ffa,0x93ff);cpu_write8(&cpu,0,0x1ffc,mirror?0x80:0);
  cpu_write16(&cpu,0,0x1ffd,0x84ff);cpu_write8(&cpu,0,0x1fff,1);WatchdogFrameStart();
  RecompReturn r=cpu_dispatch_pc_from(&cpu,0x018400,0x1fff,0xffffff);
  if(r!=RECOMP_RETURN_NORMAL||stopped!=1||wrong_bank||g_ram[0x20]!=1||g_ram[0x21]||g_ram[0x22]!=(mirror?0x80:0)||cpu.S!=0x1fff||cpu.PB!=1||
     g_recomp_stack_top||g_cpu_return_scope||sr_diagnostic_trap_warning_count()) {
   fprintf(stderr,"mx=%u mirror=%u r=%d S=%04x PB=%02x stopped=%u wrong=%u marker=%u\n",mx,mirror,r,cpu.S,cpu.PB,stopped,wrong_bank,g_ram[0x20]);return 1;
  }
 }
 puts("unpaired RTL -> mirrored jump table -> RTL: actual target PB and exactly-once execution: PASS");return 0;
}
`,
	}, "dispatch_bank")
}
