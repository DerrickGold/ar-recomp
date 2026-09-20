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

// Execute generated JSR/RTS and JSL/RTL, not just the runtime acceptance helper.
// The callee moves its own frame below two result words. The immediate caller
// must pull both words and execute its continuation exactly once, without
// restoring the pre-call S over those results or dispatching a second copy.
func TestStackedResultsNativeExecution(t *testing.T) {
	if testing.Short() {
		t.Skip("native conformance build")
	}
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("native conformance needs CMake")
	}
	image := make(rom.Image, 0x10000)
	copy(image, []byte{0xc2, 0x30, 0x20, 0x00, 0x81, 0x68, 0x85, 0x40, 0x68, 0x85, 0x42, 0xe6, 0x44, 0x60})
	copy(image[0x100:], []byte{0x7a, 0xf4, 0x34, 0x12, 0xf4, 0x78, 0x56, 0x5a, 0x60})
	copy(image[0x200:], []byte{0xc2, 0x30, 0x22, 0x00, 0x83, 1, 0x68, 0x85, 0x40, 0x68, 0x85, 0x42, 0xe6, 0x44, 0x60})
	copy(image[0x8300:], []byte{0x7a, 0xe2, 0x20, 0x68, 0x85, 0x50, 0xc2, 0x20,
		0xf4, 0x34, 0x12, 0xf4, 0x78, 0x56, 0xe2, 0x20, 0xa5, 0x50, 0x48, 0xc2, 0x20, 0x5a, 0x6b})
	ctx := codegen.NewContext()
	ctx.ExactDirectCallMX = true
	entries := map[byte][]config.Entry{}
	var table, decl, source, bytes strings.Builder
	for _, root := range []struct {
		name string
		pc   uint32
	}{{"ShortCaller", 0x8000}, {"ShortResults", 0x8100}, {"LongCaller", 0x8200}, {"LongResults", 0x018300}} {
		ctx.Names[root.pc] = root.name
		fmt.Fprintf(&table, "{0x%06xu,{", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				bank := byte(root.pc >> 16)
				entries[bank] = append(entries[bank], config.Entry{Name: root.name, Start: uint16(root.pc), EntryMX: config.MX{M: m, X: x}})
				fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
				fmt.Fprintf(&decl, "RecompReturn %s_M%dX%d(CpuState*);\n", root.name, m, x)
			}
		}
		table.WriteString("}},\n")
	}
	for _, bank := range []byte{0, 1} {
		s, err := EmitBank(image, bank, entries[bank], BankOptions{Context: ctx, UnresolvedAllowed: true})
		if err != nil {
			t.Fatal(err)
		}
		source.WriteString(s)
	}
	if !strings.Contains(source.String(), "cpu_accept_stacked_result_return") {
		t.Fatal("no stacked-result return contract")
	}
	for i, b := range image {
		if b != 0 {
			fmt.Fprintf(&bytes, "[%d]=%d,", i, b)
		}
	}
	runNativeContract(t, cmake, map[string]string{
		"generated.c": source.String(), "funcs.h": decl.String(),
		"CMakeLists.txt": `cmake_minimum_required(VERSION 3.20)
project(StackedResults LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(stacked_results fixture.c)
target_link_libraries(stacked_results PRIVATE snesrecomp_runtime)
set_property(TARGET stacked_results PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include <stdio.h>
#include <string.h>
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
 for(unsigned iteration=0;iteration<100;iteration++) for(unsigned kind=0;kind<2;kind++) {
  memset(g_ram,0,sizeof(g_ram));
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=0;cpu_p_to_mirrors(&cpu);
  cpu.S=0x1ff0;cpu.PB=0;cpu.DB=0;cpu.host_return_valid=1;
  cpu_write16(&cpu,0,0x1ff1,0x1234);WatchdogFrameStart();
  RecompReturn r=kind?LongCaller_M0X0(&cpu):ShortCaller_M0X0(&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1ff2||cpu.PB!=0||
     cpu_read16(&cpu,0,0x40)!=0x5678||cpu_read16(&cpu,0,0x42)!=0x1234||
     cpu_read16(&cpu,0,0x44)!=1||g_cpu_return_scope||g_recomp_stack_top||g_fail) {
   fprintf(stderr,"kind=%u result=%d S=%04X PB=%02X results=%04X,%04X count=%u depth=%d fail=%d\n",
    kind,r,cpu.S,cpu.PB,cpu_read16(&cpu,0,0x40),cpu_read16(&cpu,0,0x42),
    cpu_read16(&cpu,0,0x44),g_recomp_stack_top,g_fail);return 1;
  }
 }
 puts("JSR/JSL stacked results resume exactly once: PASS");return 0;
}
`,
	}, "stacked_results")
}
