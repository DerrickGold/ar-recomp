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

func TestCopDispatchNativeExecution(t *testing.T) {
	if testing.Short() {
		t.Skip("native conformance build")
	}
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("native conformance needs CMake and C/C++")
	}
	image := make(rom.Image, 0x8000)
	// COP has three inline data bytes. Decoding PC+2 would execute BRK/RTI.
	copy(image, []byte{0x02, 0x9c, 0x00, 0xff, 0x40, 0xe8, 0x6b})
	// Native ISR advances the return PC over the three data bytes, then RTIs.
	copy(image[0x100:], []byte{0xc2, 0x20, 0xa3, 2, 0x18, 0x69, 3, 0, 0x83, 2, 0x40})
	cfg := &config.Config{CopDispatch: "GameCop"}
	ctx := codegen.NewContext()
	var entries []config.Entry
	var table strings.Builder
	for _, root := range []struct {
		name string
		pc   uint16
	}{{"Root", 0x8000}, {"Continue", 0x8005}, {"Interrupt", 0x8100}} {
		ctx.Names[uint32(root.pc)] = root.name
		fmt.Fprintf(&table, "{0x%04xu,{", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				entries = append(entries, config.Entry{Name: root.name, Start: root.pc, EntryMX: config.MX{M: m, X: x}})
				fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
			}
		}
		table.WriteString("}},\n")
	}
	source, err := EmitBank(image, 0, entries, BankOptions{Context: ctx, Decode: DecodeOptionsFromConfig(0, cfg)})
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(source, "g_cpu_brk_hook") {
		t.Fatal("COP inline data was decoded as executable code")
	}
	runNativeContract(t, cmake, map[string]string{
		"generated.c": source,
		"CMakeLists.txt": `cmake_minimum_required(VERSION 3.20)
project(CopConformance LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(cop_contract fixture.c)
target_link_libraries(cop_contract PRIVATE snesrecomp_runtime)
set_property(TARGET cop_contract PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "generated.c"
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
static unsigned calls, escape;
RecompReturn GameCop(CpuState *cpu,uint32 source,uint32 *resume) {
 if(source!=0x8000u) abort();
 unsigned mx=((cpu->m_flag&1)<<1)|(cpu->x_flag&1);
 uint8 hrv=cpu->host_return_valid;
 cpu_push_interrupt_frame(cpu);
 cpu_write16(cpu,0,(uint16)(cpu->S+2u),(uint16)(source+2u));
 cpu->host_return_valid=0;
 if(g_dispatch_table[2].variant[mx](cpu)!=RECOMP_RETURN_NORMAL) abort();
 uint32 target=cpu_read16(cpu,0,(uint16)(cpu->S-2u))|((uint32)cpu_read8(cpu,0,cpu->S)<<16);
 cpu->host_return_valid=hrv;
 if(target!=0x8005u) abort();
 ++calls;
 /* Repeated tails must stay in one driver, preserving the original caller. */
 if(escape) return (RecompReturn)escape;
 *resume=calls<10000u ? 0x8000u : target;
 return RECOMP_RETURN_NORMAL;
}
const DispatchEntry g_dispatch_table[]={
` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 for(unsigned mx=0;mx<4;mx++) {
  cpu_state_init(&g_cpu,g_ram);g_cpu.emulation=0;g_cpu.P=0;
  g_cpu.m_flag=(mx>>1)&1;g_cpu.x_flag=mx&1;g_cpu.S=0x1ff;g_cpu.X=10;g_cpu.Y=17;
  cpu_mirrors_to_p(&g_cpu);calls=0;
  cpu_push_jsl_return_frame(&g_cpu);
  RecompReturn r=g_dispatch_table[0].variant[mx](&g_cpu);
  if(r!=RECOMP_RETURN_NORMAL||calls!=10000u||g_cpu.X!=11||g_cpu.Y!=17||g_cpu.S!=0x1ff||
     g_cpu.m_flag!=((mx>>1)&1)||g_cpu.x_flag!=(mx&1)||g_recomp_stack_top||g_cpu_return_scope) return 1;
 }
 const RecompReturn escapes[]={RECOMP_RETURN_SKIP_1,RECOMP_RETURN_SKIP_2,RECOMP_RETURN_SKIP_3,
     RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND};
 for(unsigned i=0;i<sizeof(escapes)/sizeof(escapes[0]);i++) {
  escape=escapes[i];
  cpu_state_init(&g_cpu,g_ram);g_cpu.emulation=0;g_cpu.m_flag=0;g_cpu.x_flag=0;
  g_cpu.S=0x1ff;g_cpu.X=10;calls=0;cpu_mirrors_to_p(&g_cpu);
  cpu_push_jsl_return_frame(&g_cpu);
  RecompReturn r=g_dispatch_table[0].variant[0](&g_cpu);
  if(r!=(RecompReturn)escape||calls!=1||g_cpu.X!=10||g_cpu.S!=0x1fc||g_recomp_stack_top||g_cpu_return_scope) return 2;
 }
 return 0;
}
`,
	}, "cop_contract")
}
