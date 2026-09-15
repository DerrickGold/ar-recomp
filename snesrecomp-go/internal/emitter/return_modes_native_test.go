package emitter

import (
	"fmt"
	"os/exec"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestComputedReturnLiveWidthContinuationNative(t *testing.T) {
	image := make(rom.Image, 0x8000)
	// Both post-call interpretations are legal but different: M0 loads $EA42;
	// M1 loads $42 then executes NOP. Pinning one decode silently corrupts the
	// operand width rather than necessarily trapping.
	copy(image, []byte{0x22, 0, 0x81, 0, 0xa9, 0x42, 0xea, 0x85, 0x50, 0x86, 0x54, 0xc2, 0x30, 0x60})
	copy(image[0x100:], []byte{0xe2, 0x20, 0xd4, 0x40, 0x60}) // SEP; PEI target; RTS (not the caller return)
	copy(image[0x200:], []byte{0xa5, 0x30, 0x48, 0x28, 0x6b}) // choose live P, then return original JSL
	ctx := codegen.NewContext()
	ctx.Names[0x8000], ctx.Names[0x8100], ctx.Names[0x8200] = "Caller", "Dispatch", "Handler"
	var source, decl, table, bytes strings.Builder
	for _, root := range []struct {
		name string
		pc   uint16
	}{{"Caller", 0x8000}, {"Dispatch", 0x8100}, {"Handler", 0x8200}} {
		fmt.Fprintf(&table, "{0x%04xu,{", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				options := FunctionOptions{Name: root.name, Codegen: ctx, UnresolvedAllowed: true}
				options.Decode.CalleeExitMX = map[decoder.Variant]decoder.MX{}
				for a := uint8(0); a < 2; a++ {
					for b := uint8(0); b < 2; b++ {
						options.Decode.CalleeExitMX[decoder.Variant{Address: 0x8100, M: a, X: b}] = decoder.MX{M: -1, X: -1}
					}
				}
				r, err := EmitFunction(image, 0, root.pc, m, x, options)
				if err != nil {
					t.Fatal(err)
				}
				if root.name == "Caller" && !strings.Contains(r.Source, "live post-call M/X") {
					t.Fatal("post-call continuation guessed one width")
				}
				if root.name == "Caller" && !strings.Contains(r.Source, "default: RecompStackPop(); return sr_missing_mx_variant_warn") {
					t.Fatal("unexpected live-width guard leaks its activation")
				}
				source.WriteString(r.Source)
				fmt.Fprintf(&decl, "RecompReturn %s_M%dX%d(CpuState*);\n", root.name, m, x)
				fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
			}
		}
		table.WriteString("}},\n")
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
project(ComputedReturnModes LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(return_modes fixture.c)
target_link_libraries(return_modes PRIVATE snesrecomp_runtime)
set_property(TARGET return_modes PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include <stdio.h>
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/generated_support.h"
#include "funcs.h"
#include "generated.c"
extern bool g_fail;
void RtlApuLock(void) {} void RtlApuUnlock(void) {}
static const uint8 fixture_rom[0x8000]={` + bytes.String() + `};
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 g_rom=fixture_rom;
 for(unsigned n=0;n<100;n++) for(unsigned mx=0;mx<4;mx++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=0;cpu_p_to_mirrors(&cpu);
  cpu.S=0x1ffd;cpu.PB=0;cpu.DB=0;cpu.A=0xbe00;cpu.X=0x1234;cpu.host_return_valid=1;
  g_ram[0x30]=(mx&2?0x20:0)|(mx&1?0x10:0);
  g_ram[0x50]=g_ram[0x51]=g_ram[0x54]=g_ram[0x55]=0xa5;
  cpu_write16(&cpu,0,0x1ffe,0x1234);cpu_write16(&cpu,0,0x40,0x81ff);WatchdogFrameStart();
  RecompReturn r=Caller_M0X0(&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||g_ram[0x50]!=0x42||
     g_ram[0x51]!=(mx&2?0xa5:0xea)||g_ram[0x54]!=0x34||g_ram[0x55]!=(mx&1?0xa5:0x12)||
     g_recomp_stack_top||g_cpu_return_scope||g_fail) {
   fprintf(stderr,"mx=%u r=%d S=%04x writes=%02x%02x %02x%02x depth=%d fail=%d\n",mx,r,cpu.S,
    g_ram[0x51],g_ram[0x50],g_ram[0x55],g_ram[0x54],g_recomp_stack_top,g_fail);return 1;
  }
 }
 puts("PEI/RTS dispatch returns through live M/X exactly once: PASS");return 0;
}
`,
	}, "return_modes")
}
