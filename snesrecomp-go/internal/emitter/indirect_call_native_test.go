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

func TestIndirectCallExternalContinuationMergesEquivalentPHPHistories(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xd0, 6, 0x08, 0x80, 8, 0xea, 0xea, 0xea, 0xc2, 0x30, 0x08, 0xe2, 0x30, 0xfc, 6, 0, 0x60})
	r, err := EmitFunction(image, 0, 0x8000, 1, 1, FunctionOptions{
		Name: "MergedHistories", UnresolvedAllowed: true,
		Decode: decoder.Options{SiblingEntryPCs: map[uint16]struct{}{0x8010: {}}},
	})
	if err != nil {
		t.Fatal(err)
	}
	start := strings.Index(r.Source, "/* live post-call M/X */")
	if start < 0 {
		t.Fatal("lost live continuation switch")
	}
	end := strings.Index(r.Source[start:], "default:")
	if end < 0 || strings.Count(r.Source[start:start+end], "case ") != 4 {
		t.Fatal("equivalent external PHP-history edges were not merged")
	}
}

func TestUnconfiguredIndirectJSRNative(t *testing.T) {
	image := make(rom.Image, 0x10000)
	// The base is outside the ROM window; only live X selects the pointer.
	copy(image[0x8000:], []byte{0xfc, 0xf0, 0x7f, 0xa9, 7, 0x85, 0x10, 0xc2, 0x30, 0xe6, 0x12, 0x60})
	copy(image[0x8010:], []byte{0, 0x82})
	copy(image[0x8030:], []byte{0, 0x84})
	copy(image[0x10:], []byte{0, 0x96}) // wrong DB bank is not the program table
	copy(image[0x8200:], []byte{0x08, 0xe2, 0x20, 0x4b, 0x68, 0x85, 0x22, 0x28, 0xe6, 0x20, 0xe2, 0x20, 0x60})
	copy(image[0x8400:], []byte{0xcb, 0x80, 0xfd})
	ctx := codegen.NewContext()
	ctx.Names[0x018000] = "Caller"
	ctx.Names[0x018200] = "Handler"
	ctx.Names[0x018400] = "Park"
	var source, decl, table, bytes strings.Builder
	for _, root := range []struct {
		name string
		pc   uint32
	}{{"Caller", 0x018000}, {"Handler", 0x018200}, {"Park", 0x018400}} {
		if root.name != "Caller" {
			fmt.Fprintf(&table, "{0x%06xu,{", root.pc)
		}
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				r, err := EmitFunction(image, byte(root.pc>>16), uint16(root.pc), m, x, FunctionOptions{Name: root.name, Codegen: ctx, UnresolvedAllowed: true, ParkClosedWaits: true})
				if err != nil {
					t.Fatal(err)
				}
				if root.name == "Caller" && (strings.Contains(r.Source, "SUPPRESSED") || !strings.Contains(r.Source, "live post-call M/X")) {
					t.Fatal("missing live indirect-call continuation")
				}
				source.WriteString(r.Source)
				fmt.Fprintf(&decl, "RecompReturn %s_M%dX%d(CpuState*);\n", root.name, m, x)
				if root.name != "Caller" {
					fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
				}
			}
		}
		if root.name != "Caller" {
			table.WriteString("}},")
		}
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
project(IndirectCall LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(indirect_call fixture.c)
target_link_libraries(indirect_call PRIVATE snesrecomp_runtime)
set_property(TARGET indirect_call PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include <stdio.h>
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/generated_support.h"
#include "funcs.h"
#include "generated.c"
void RtlApuLock(void) {} void RtlApuUnlock(void) {}
extern bool g_fail;
extern unsigned sr_diagnostic_trap_warning_count(void);
static const uint8 fixture_rom[0x10000]={` + bytes.String() + `};
/* The call's internal continuation has deliberately NO registry entry. */
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=2;
static RecompReturn (*const callers[4])(CpuState*)={Caller_M0X0,Caller_M0X1,Caller_M1X0,Caller_M1X1};
int main(void) {
 g_rom=fixture_rom;
 for(unsigned n=0;n<100;n++) for(unsigned mx=0;mx<4;mx++) for(unsigned mirror=0;mirror<2;mirror++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=(mx&2?0x20:0)|(mx&1?0x10:0);cpu_p_to_mirrors(&cpu);
  cpu.S=0x1ffd;cpu.PB=mirror?0x81:1;cpu.DB=0;cpu.X=0x20;cpu.host_return_valid=1;
  cpu_write16(&cpu,0,0x10,0);cpu_write16(&cpu,0,0x12,0);cpu_write16(&cpu,0,0x20,0xffff);cpu_write8(&cpu,0,0x22,0xff);
  cpu_write16(&cpu,0,0x1ffe,0x1234);WatchdogFrameStart();
  RecompReturn r=callers[mx](&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||cpu.PB!=(mirror?0x81:1)||cpu.m_flag||cpu.x_flag||g_ram[0x10]!=7||g_ram[0x12]!=1||
    g_ram[0x20]||g_ram[0x21]!=(mx&2?0xff:0)||g_ram[0x22]!=(mirror?0x81:1)||g_recomp_stack_top||g_cpu_return_scope||g_cpu_owned_unwind_scope||g_fail||sr_diagnostic_trap_warning_count()) {
    fprintf(stderr,"mx=%u mirror=%u r=%d S=%04x PB=%02x marker=%u count=%u word=%04x depth=%d\n",mx,mirror,r,cpu.S,cpu.PB,g_ram[0x10],g_ram[0x12],cpu_read16(&cpu,0,0x20),g_recomp_stack_top);return 1;
  }
 }
 CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=0;cpu_p_to_mirrors(&cpu);cpu.PB=1;cpu.S=0x1ffd;cpu.X=0x40;cpu.host_return_valid=1;
 cpu_write16(&cpu,0,0x12,0);WatchdogFrameStart();RecompReturn parked=Caller_M0X0(&cpu);
 if(parked!=RECOMP_RETURN_PARKED_WAIT||cpu.S!=0x1ffb||cpu.PB!=1||g_cpu_wait_pc24!=0x018400||g_ram[0x12]||g_cpu_return_scope||g_recomp_stack_top) {fputs("indirect call lost parked CPU/ownership\n",stderr);return 1;}
 cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=0;cpu_p_to_mirrors(&cpu);cpu.PB=1;cpu.S=0x1ffd;cpu.X=0x30;cpu.host_return_valid=1;
 cpu_write16(&cpu,0,0x12,0);WatchdogFrameStart();(void)Caller_M0X0(&cpu);
 if(sr_diagnostic_trap_warning_count()!=1||g_ram[0x12]||g_cpu_return_scope||g_recomp_stack_top) {
  fprintf(stderr,"missing target: diagnostics=%u continued=%u owner=%p depth=%d\n",sr_diagnostic_trap_warning_count(),g_ram[0x12],(void*)g_cpu_return_scope,g_recomp_stack_top);return 1;
 }
 puts("unconfigured indirect JSR: actual PB, live widths, exact-once continuation and hard missing body: PASS");return 0;
}
`,
	}, "indirect_call")
}
