package emitter

import (
	"fmt"
	"os/exec"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestColdInternalTailNative(t *testing.T) {
	image := make(rom.Image, 0x10000)
	copy(image, []byte{0x22, 0, 0x81, 1, 0xe6, 0x10, 0x60}) // JSL driver; INC $10; RTS
	copy(image[0x8100:], []byte{0x8b, 0x5a, 0x6c, 0x40, 0}) // PHB; PHY; JMP ($40)
	copy(image[0x8200:], []byte{0xe6, 0x12, 0x4c, 0, 0x83}) // callback; JMP cleanup
	copy(image[0x8300:], []byte{0x7a, 0xab, 0x6b})          // PLY; PLB; RTL of original JSL
	ctx := codegen.NewContext()
	ctx.Names[0x8000], ctx.Names[0x018100], ctx.Names[0x018200] = "Outer", "Driver", "Callback"
	var source, decl, table, bytes strings.Builder
	for _, root := range []struct {
		name string
		pc   uint32
	}{{"Outer", 0x8000}, {"Driver", 0x018100}, {"Callback", 0x018200}} {
		fmt.Fprintf(&table, "{0x%06xu,{", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				options := FunctionOptions{Name: root.name, Codegen: ctx, UnresolvedAllowed: true}
				if root.name == "Callback" {
					options.RegionBody = &RegionBodyOptions{HelperName: fmt.Sprintf("sr_region_fixture_M%dX%d", m, x), OwnerPC: 0x8200,
						Entries: []RegionEntry{{PC: 0x8300, M: m, X: x, Selector: 1}}}
				}
				r, err := EmitFunction(image, byte(root.pc>>16), uint16(root.pc), m, x, options)
				if err != nil {
					t.Fatal(err)
				}
				source.WriteString(r.Source)
				if options.RegionBody != nil {
					if !strings.Contains(r.Source, fmt.Sprintf("goto L_8300_M%dX%d;", m, x)) {
						t.Fatal("hot local jump lost")
					}
					source.WriteString(EmitColdRegionEntry(1, options.RegionBody.Entries[0], options.RegionBody.HelperName, 0x8200))
					fmt.Fprintf(&decl, "static inline RecompReturn %s(CpuState*,uint16,uint8,uint16);\n", options.RegionBody.HelperName)
				}
				fmt.Fprintf(&decl, "RecompReturn %s_M%dX%d(CpuState *cpu);\n", root.name, m, x)
				fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
			}
		}
		table.WriteString("}},\n")
	}
	table.WriteString("{0x018300u,{bank_01_8300_M0X0,bank_01_8300_M0X1,bank_01_8300_M1X0,bank_01_8300_M1X1}},\n")
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
project(ColdInternalTail LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(cold_tail_contract fixture.c)
target_link_libraries(cold_tail_contract PRIVATE snesrecomp_runtime)
set_property(TARGET cold_tail_contract PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include <stdio.h>
#include <stdlib.h>
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/generated_support.h"
#include "funcs.h"
#include "generated.c"
extern bool g_fail;
void RtlApuLock(void) {} void RtlApuUnlock(void) {}
static const uint8 fixture_rom[0x10000]={` + bytes.String() + `};
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 g_rom=fixture_rom;
 for(unsigned n=0;n<100;n++) for(unsigned mx=0;mx<4;mx++) for(unsigned cold=0;cold<2;cold++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=(mx&2?0x20:0)|(mx&1?0x10:0);cpu_p_to_mirrors(&cpu);
  cpu.S=0x1ffd;cpu.PB=0x80;cpu.DB=0x42;cpu.Y=(mx&1)?0x34:0x1234;cpu.host_return_valid=1;
  g_ram[0x10]=g_ram[0x11]=g_ram[0x12]=g_ram[0x13]=0;
  cpu_write16(&cpu,0,0x1ffe,0x1234);cpu_write16(&cpu,0,0x40,cold?0x8300:0x8200);WatchdogFrameStart();
  RecompReturn r=g_dispatch_table[0].variant[mx](&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||cpu.PB!=0x80||cpu.DB!=0x42||cpu.Y!=((mx&1)?0x34:0x1234)||
     g_ram[0x10]!=1||g_ram[0x11]||g_ram[0x12]!=(cold?0:1)||g_ram[0x13]||g_recomp_stack_top||g_cpu_return_scope||g_fail) {
   fprintf(stderr,"cold=%u mx=%u r=%d S=%04x PB=%02x depth=%d outer=%u callback=%u fail=%d\n",cold,mx,r,cpu.S,cpu.PB,g_recomp_stack_top,g_ram[0x10],g_ram[0x12],g_fail);return 1;
  }
 }
 puts("cold cleanup and local goto retain the original JSL, live M/X and exactly-once continuation: PASS");return 0;
}
`,
	}, "cold_tail_contract")
}
