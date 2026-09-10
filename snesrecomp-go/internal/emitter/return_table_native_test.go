package emitter

import (
	"fmt"
	"os/exec"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestReturnTableNativeExecution(t *testing.T) {
	image := make(rom.Image, 0x80000)
	copy(image, []byte{0x22, 0, 0x81, 0, 0xee, 0x10, 0, 0x60})               // Outer: JSL Dispatcher; INC $10; RTS
	copy(image[0x100:], []byte{0x22, 0, 0x83, 1, 8, 0x81, 0x10, 0x81, 0x6b}) // helper is in another bank
	image[0x110] = 0x6b
	image[0x112], image[0x113] = 0x40, 0x81 // selector 7, beyond the two-word prefix
	image[0x140] = 0x6b
	copy(image[0x8300:], []byte{
		0x08, 0xc2, 0x30, 0xda, 0x5a, 0x29, 0xff, 0, 0x0a, 0xa8, 0xc8, 0x0b, 0x3b, 0x5b,
		0xb7, 8, 0x85, 8, 0xc6, 8, 0x2b, 0x7a, 0xfa, 0x28, 0x6b,
	})
	ctx := codegen.NewContext()
	entries := map[byte][]config.Entry{}
	var table, romBytes, declarations strings.Builder
	for _, root := range []struct {
		name string
		pc   uint32
	}{
		{"Outer", 0x8000}, {"Dispatcher", 0x8100}, {"First", 0x8108}, {"Second", 0x8110}, {"Outside", 0x8140}, {"Helper", 0x018300},
	} {
		ctx.Names[uint32(root.pc)] = root.name
		fmt.Fprintf(&table, "{0x%04xu,{", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				bank := byte(root.pc >> 16)
				entries[bank] = append(entries[bank], config.Entry{Name: root.name, Start: uint16(root.pc), EntryMX: config.MX{M: m, X: x}})
				fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
				fmt.Fprintf(&declarations, "RecompReturn %s_M%dX%d(CpuState *cpu);\n", root.name, m, x)
			}
		}
		table.WriteString("}},\n")
	}
	for at, b := range image {
		if b != 0 {
			fmt.Fprintf(&romBytes, "[%d]=%d,", at, b)
		}
	}
	source, err := EmitBank(image, 0, entries[0], BankOptions{Context: ctx, Decode: decoder.Options{NativeReturnTables: true}})
	if err != nil {
		t.Fatal(err)
	}
	helper, err := EmitBank(image, 1, entries[1], BankOptions{Context: ctx})
	if err != nil {
		t.Fatal(err)
	}
	source += helper
	for _, want := range []string{"Helper_M0X0(cpu)", "0x000000u | (uint32)cpu->A", "cpu_dispatch_paired_tail_from(cpu, _handler", "cpu->S < 10u"} {
		if !strings.Contains(source, want) {
			t.Fatalf("missing %q", want)
		}
	}
	if strings.Contains(source, "L_8104_") {
		t.Fatal("table data decoded as the caller's continuation")
	}
	if testing.Short() {
		t.Skip("native execution tier (emission assertions passed)")
	}
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("native conformance needs CMake and a C/C++ toolchain")
	}
	runNativeContract(t, cmake, map[string]string{
		"generated.c": source, "funcs.h": declarations.String(),
		"CMakeLists.txt": `cmake_minimum_required(VERSION 3.20)
project(ReturnTableConformance LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(return_contract fixture.c)
target_link_libraries(return_contract PRIVATE snesrecomp_runtime)
set_property(TARGET return_contract PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "generated.c"
extern bool g_fail;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
static unsigned continuation_calls;
static RecompReturn continuation(CpuState *cpu) {
 (void)cpu; ++continuation_calls; return RECOMP_RETURN_NORMAL;
}
static const uint8 fixture_rom[0x80000]={` + romBytes.String() + `};
const DispatchEntry g_dispatch_table[]={
 {0x001235u,{continuation,continuation,continuation,continuation}},
` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 g_rom=fixture_rom;
 for(unsigned mx=0;mx<4;mx++) for(unsigned choice=0;choice<3;choice++) for(unsigned paired=0;paired<2;paired++) {
  CpuState cpu; cpu_state_init(&cpu,g_ram); cpu.emulation=0;
  cpu.P=(uint8)(0x45u|(mx&2?CPU_P_M:0)|(mx&1?CPU_P_X:0)); cpu_p_to_mirrors(&cpu);
  uint8 old_p=cpu.P;
  cpu.A=(uint16)(0xab00u|(choice==2?7:choice));cpu.X=mx&1?0x34:0x1234;cpu.Y=mx&1?0x78:0x5678;
  cpu.D=0x125;cpu.DB=0x7e;cpu.PB=0;cpu.S=0x1ffd;cpu.host_return_valid=(uint8)paired;
  uint16 old_x=cpu.X,old_y=cpu.Y;g_ram[0x10]=g_ram[0x11]=0;
  cpu_write8(&cpu,0,0x1ffe,0x34);cpu_write8(&cpu,0,0x1fff,0x12);
  WatchdogFrameStart();continuation_calls=0;
  RecompReturn r=paired?g_dispatch_table[1].variant[mx](&cpu):cpu_dispatch_pc_from(&cpu,0x8000,0x1ffd,0x9999);
  cpu_mirrors_to_p(&cpu);
  uint16 target=choice==2?0x8140:choice?0x8110:0x8108;
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||g_recomp_stack_top||g_fail||
    g_ram[0x10]!=1||g_ram[0x11]!=0||continuation_calls!=(paired?0u:1u)||
    cpu.X!=old_x||cpu.Y!=old_y||cpu.D!=0x125||cpu.DB!=0x7e||cpu.PB!=0||cpu.P!=old_p||cpu.A!=target) {
   fprintf(stderr,"return contract mx=%u choice=%u paired=%u r=%d S=%04x depth=%d fail=%d count=%u ram=%u A=%04x P=%02x expected=%02x\n",
    mx,choice,paired,r,cpu.S,g_recomp_stack_top,g_fail,continuation_calls,g_ram[0x10],cpu.A,cpu.P,old_p);
   return 1;
  }
 }
 puts("native return tables: all M/X, nested JSL/RTS, paired/unpaired, open-prefix fallback, exact continuation/register/stack effects PASS");
 return 0;
}
`,
	}, "return_contract")
}
