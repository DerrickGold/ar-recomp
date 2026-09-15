package emitter

import (
	"fmt"
	"os/exec"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestOpenJumpNativeTargetAndContinuation(t *testing.T) {
	image := make(rom.Image, 0x10000)
	copy(image, []byte{0x22, 0, 0x81, 1, 0xe6, 0x10, 0x60})
	copy(image[0x20:], []byte{0x22, 0, 0x81, 0x81, 0xe6, 0x10, 0x60})
	copy(image[0x8100:], []byte{0x8b, 0x5a, 0xf4, 0x7f, 0x81, 0x7c, 0, 0x90}) // PHB; PHY; PEA cleanup-1; JMP ($9000,X)
	copy(image[0x8180:], []byte{0xf4, 0x8f, 0x81, 0x7c, 0, 0x90})             // another software call from the first continuation
	copy(image[0x8190:], []byte{0x7a, 0xab, 0x6b})                            // shared cleanup; original JSL return
	handler := func(count int) []byte {
		var bytes []byte
		for range count {
			bytes = append(bytes, 0xe6, 0x12)
		}
		// Record the live PB without changing return M/X or consuming a frame.
		return append(bytes, 0x08, 0xe2, 0x20, 0x4b, 0x68, 0x85, 0x20, 0x28, 0x60)
	}
	copy(image[0x8282:], handler(1))
	copy(image[0x8382:], handler(2))
	copy(image[0x8400:], handler(3))
	// Automatic recovery stops at the unmapped second word ($0083). X=1
	// nevertheless reads the valid overlapping word $8382; X=4 reads $8400.
	copy(image[0x9000:], []byte{0x82, 0x82, 0x83, 0, 0, 0x84, 0, 0})
	ctx := codegen.NewContext()
	roots := []struct {
		name string
		pc   uint32
	}{{"Outer", 0x8000}, {"MirrorOuter", 0x8020}, {"Driver", 0x018100}, {"Continue", 0x018180}, {"Cleanup", 0x018190}, {"First", 0x018282}, {"Odd", 0x018382}, {"Later", 0x018400}}
	for _, root := range roots {
		ctx.Names[root.pc] = root.name
	}
	ctx.Names[0x818100] = "Driver"
	var source, decl, table, bytes strings.Builder
	for _, root := range roots {
		fmt.Fprintf(&table, "{0x%06xu,{", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				r, err := EmitFunction(image, byte(root.pc>>16), uint16(root.pc), m, x,
					FunctionOptions{Name: root.name, Codegen: ctx, UnresolvedAllowed: true})
				if err != nil {
					t.Fatal(err)
				}
				if root.name == "Driver" && (!strings.Contains(r.Source, "open dispatch: exact local fast path") || strings.Contains(r.Source, "dispatch_oob")) {
					t.Fatal("automatic jump did not retain an open, locally optimized target inventory")
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
project(OpenDispatch LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(open_dispatch fixture.c)
target_link_libraries(open_dispatch PRIVATE snesrecomp_runtime)
set_property(TARGET open_dispatch PROPERTY LINKER_LANGUAGE CXX)
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
static const uint8 fixture_rom[0x10000]={` + bytes.String() + `};
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 g_rom=fixture_rom;
 for(unsigned n=0;n<100;n++) for(unsigned mx=0;mx<4;mx++) for(unsigned selector=0;selector<3;selector++) for(unsigned mirror=0;mirror<2;mirror++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=(mx&2?0x20:0)|(mx&1?0x10:0);cpu_p_to_mirrors(&cpu);
  cpu.S=0x1ffd;cpu.PB=0x80;cpu.DB=0x42;cpu.Y=(mx&1)?0x34:0x1234;cpu.X=selector==2?4:selector;cpu.host_return_valid=1;
  g_ram[0x10]=g_ram[0x11]=g_ram[0x12]=g_ram[0x13]=g_ram[0x20]=0;
  cpu_write16(&cpu,0,0x1ffe,0x1234);WatchdogFrameStart();
  RecompReturn r=g_dispatch_table[mirror].variant[mx](&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||cpu.PB!=0x80||cpu.DB!=0x42||cpu.Y!=((mx&1)?0x34:0x1234)||
     g_ram[0x10]!=1||g_ram[0x11]||g_ram[0x12]!=2*(selector+1)||g_ram[0x13]||g_ram[0x20]!=(mirror?0x81:1)||g_recomp_stack_top||g_cpu_return_scope||sr_diagnostic_trap_warning_count()) {
   fprintf(stderr,"selector=%u mx=%u r=%d S=%04x PB=%02x depth=%d outer=%u callback=%u\n",selector,mx,r,cpu.S,cpu.PB,g_recomp_stack_top,g_ram[0x10],g_ram[0x12]);return 1;
  }
 }
 /* A real null target is not a made-up return or index-zero handler. */
 CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=0;cpu_p_to_mirrors(&cpu);
 cpu.S=0x1ffd;cpu.X=6;cpu.host_return_valid=1;cpu_write16(&cpu,0,0x1ffe,0x1234);g_ram[0x12]=0;
 (void)Outer_M0X0(&cpu);
 if(sr_diagnostic_trap_warning_count()!=1||g_ram[0x12]!=0) return 2;
 puts("open jump: local, odd, beyond-prefix, missing target, two software-call continuations and exactly-once JSL cleanup: PASS");return 0;
}
`,
	}, "open_dispatch")
}
