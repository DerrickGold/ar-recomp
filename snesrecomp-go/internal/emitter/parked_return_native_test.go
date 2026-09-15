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

func TestPushedDispatchAndParkedWaitNative(t *testing.T) {
	image := make(rom.Image, 0x10000)
	copy(image, []byte{0x22, 0, 0x81, 1, 0xe6, 0x10, 0x60})
	copy(image[0x8100:], []byte{0x8b, 0x5a, 0xf4, 0xff, 0x81, 0x60}) // PHB/PHY/PEA handler-1/RTS
	copy(image[0x8200:], []byte{0x7a, 0xab, 0x6b})                   // handler finishes original JSL
	copy(image[0x400:], []byte{0x22, 0, 0x85, 1, 0xee, 0x12, 0, 0x60})
	copy(image[0x8500:], []byte{0x20, 0, 0x86, 0xee, 0x14, 0, 0x6b})
	copy(image[0x8600:], []byte{0xcb, 0x80, 0xfd})
	copy(image[0x800:], []byte{0x20, 0, 0x89, 0xe6, 0x18, 0x60})
	copy(image[0x900:], []byte{0x48, 0xda, 0xf4, 0x4f, 0x89, 0xf4, 0x2f, 0x89, 0x60})
	copy(image[0x930:], []byte{0x60})
	copy(image[0x950:], []byte{0xfa, 0x68, 0x18, 0x60})
	// A data-driven callback tail-jumps to shared cleanup instead of RTS.
	copy(image[0xa00:], []byte{0x22, 0, 0x8b, 1, 0xe6, 0x1a, 0x60})
	copy(image[0x8b00:], []byte{0x8b, 0x5a, 0x6c, 0x40, 0})
	copy(image[0x8c00:], []byte{0xe6, 0x1c, 0x4c, 0, 0x8d})
	copy(image[0x8d00:], []byte{0x7a, 0xab, 0x6b})
	ctx := codegen.NewContext()
	entries := map[byte][]config.Entry{}
	var table, decl, source, bytes strings.Builder
	for _, root := range []struct {
		name string
		pc   uint32
	}{
		{"Outer", 0x8000}, {"ParkOuter", 0x8400},
		{"WebOuter", 0x008800}, {"WebContinuation", 0x008803}, {"Web", 0x008900},
		{"WebBody", 0x008930}, {"WebCleanup", 0x008950},
		{"ScriptOuter", 0x008a00},
		{"Dispatch", 0x018100}, {"Handler", 0x018200}, {"ParkChild", 0x018500}, {"Park", 0x018600},
		{"ScriptDriver", 0x018b00}, {"ScriptCallback", 0x018c00}, {"ScriptCleanup", 0x018d00},
	} {
		ctx.Names[root.pc] = root.name
		fmt.Fprintf(&table, "{0x%06xu,{", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				bank := byte(root.pc >> 16)
				entries[bank] = append(entries[bank], config.Entry{Name: root.name, Start: uint16(root.pc), EntryMX: config.MX{M: m, X: x}})
				fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
				fmt.Fprintf(&decl, "RecompReturn %s_M%dX%d(CpuState *cpu);\n", root.name, m, x)
			}
		}
		table.WriteString("}},\n")
	}
	for _, bank := range []byte{0, 1} {
		s, err := EmitBank(image, bank, entries[bank], BankOptions{Context: ctx, ParkClosedWaits: true, UnresolvedAllowed: true})
		if err != nil {
			t.Fatal(err)
		}
		source.WriteString(s)
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
project(ParkedReturnConformance LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(parked_contract fixture.c)
target_link_libraries(parked_contract PRIVATE snesrecomp_runtime)
set_property(TARGET parked_contract PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "generated.c"
extern bool g_fail;
void RtlApuLock(void) {} void RtlApuUnlock(void) {}
static const uint8 fixture_rom[0x10000]={` + bytes.String() + `};
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 g_rom=fixture_rom;
 for(unsigned n=0;n<100;n++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=0;cpu_p_to_mirrors(&cpu);
  cpu.S=0x1ffd;cpu.PB=0x80;cpu.DB=0x42;cpu.Y=0x1234;cpu.host_return_valid=1;
  g_ram[0x10]=g_ram[0x11]=0;
  cpu_write16(&cpu,0,0x1ffe,0x1234);WatchdogFrameStart();
  RecompReturn r=Outer_M0X0(&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||cpu.PB!=0x80||cpu.DB!=0x42||cpu.Y!=0x1234||
    g_ram[0x10]!=1||g_ram[0x11]!=0||g_ram[0x1ffd]!=0x80||g_recomp_stack_top||g_cpu_return_scope||g_fail) {
   fprintf(stderr,"pushed dispatch r=%d S=%04x PB=%02x depth=%d count=%u fail=%d\n",r,cpu.S,cpu.PB,g_recomp_stack_top,g_ram[0x10],g_fail);return 1;
  }
  cpu.S=0x1ffd;cpu.PB=0x80;cpu.DB=0x42;cpu.Y=0x1234;cpu.host_return_valid=1;
  g_ram[0x1a]=g_ram[0x1b]=g_ram[0x1c]=g_ram[0x1d]=0;
  cpu_write16(&cpu,0,0x40,0x8c00);
  r=ScriptOuter_M0X0(&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||cpu.PB!=0x80||cpu.DB!=0x42||cpu.Y!=0x1234||
     g_ram[0x1a]!=1||g_ram[0x1b]||g_ram[0x1c]!=1||g_ram[0x1d]||g_cpu_return_scope||g_recomp_stack_top||g_fail) {
   fprintf(stderr,"indirect callback/shared cleanup r=%d S=%04x count=%u/%u fail=%d\n",r,cpu.S,g_ram[0x1a],g_ram[0x1c],g_fail);return 1;
  }
  cpu.S=0x1ffd;cpu.PB=0;cpu.A=0x5678;cpu.X=0x2345;cpu.host_return_valid=1;g_ram[0x18]=g_ram[0x19]=0;
  r=WebOuter_M0X0(&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||cpu.A!=0x5678||cpu.X!=0x2345||g_ram[0x18]!=1||g_ram[0x19]||g_cpu_return_scope||g_recomp_stack_top||g_fail) {
   fprintf(stderr,"registered continuation executed twice: r=%d S=%04x count=%u fail=%d\n",r,cpu.S,g_ram[0x18],g_fail);return 1;
  }
  cpu.S=0x1ffd;cpu.PB=0x80;cpu.host_return_valid=1;g_ram[0x12]=g_ram[0x14]=0;
  r=ParkOuter_M0X0(&cpu);
  if(r!=RECOMP_RETURN_PARKED_WAIT||cpu.S!=0x1ff8||cpu.PB!=1||g_cpu_wait_pc24!=0x018600||
    g_cpu_wait_resume_pc24!=0x018601||g_ram[0x12]||g_ram[0x14]||g_recomp_stack_top||g_cpu_return_scope||g_fail) {
   fprintf(stderr,"parked wait r=%d S=%04x PB=%02x depth=%d fail=%d\n",r,cpu.S,cpu.PB,g_recomp_stack_top,g_fail);return 1;
  }
 }
 puts("pushed dispatch retains JSL owner; nested parked waits retain CPU and release host scopes: PASS");return 0;
}
`,
	}, "parked_contract")
}
