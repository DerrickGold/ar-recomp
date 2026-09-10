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

func TestPollWaitNativeExecution(t *testing.T) {
	if testing.Short() {
		t.Skip("native conformance build")
	}
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("native conformance needs CMake and C/C++")
	}
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xad, 0x30, 0, 0xd0, 0xfb, 0xe8, 0x6b}) // load/poll, then INX exactly once
	copy(image[0x100:], []byte{0xce, 0x30, 0, 0x40})           // original synthetic ISR: DEC counter; RTI
	ctx := codegen.NewContext()
	var entries []config.Entry
	var table strings.Builder
	for _, root := range []struct {
		name string
		pc   uint16
	}{{"Poll", 0x8000}, {"Interrupt", 0x8100}} {
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
	source, err := EmitBank(image, 0, entries, BankOptions{Context: ctx})
	if err != nil {
		t.Fatal(err)
	}
	runNativeContract(t, cmake, map[string]string{
		"generated.c": source, "funcs.h": "/* declarations in generated.c */\n",
		"CMakeLists.txt": `cmake_minimum_required(VERSION 3.20)
project(PollWaitConformance LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(poll_contract fixture.c)
target_link_libraries(poll_contract PRIVATE snesrecomp_runtime)
set_property(TARGET poll_contract PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "generated.c"
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
static unsigned polls,failed;
static void frame(void) {}
static void poll_wait(CpuState *cpu,uint32_t pc,uint32_t address,uint32_t width) {
 cpu_mirrors_to_p(cpu);CpuState saved=*cpu;unsigned mx=((cpu->m_flag&1)<<1)|(cpu->x_flag&1);
 ++polls;
 if(pc!=0x8000||address!=0x30||width!=(cpu->m_flag?1u:2u)||g_recomp_stack_top!=1) ++failed;
 cpu_push_interrupt_frame(cpu);
 cpu_write16(cpu,0,(uint16)(cpu->S+2u),(uint16)pc);
 cpu->host_return_valid=0;
 WatchdogFrameStart();
 if(g_dispatch_table[1].variant[mx](cpu)!=RECOMP_RETURN_NORMAL) ++failed;
 cpu->host_return_valid=saved.host_return_valid;
 cpu_mirrors_to_p(cpu);
 if(cpu->A!=saved.A||cpu->X!=saved.X||cpu->Y!=saved.Y||cpu->D!=saved.D||cpu->DB!=saved.DB||
    cpu->S!=saved.S||cpu->P!=saved.P||cpu->PB!=saved.PB||g_recomp_stack_top!=1) ++failed;
}
const DispatchEntry g_dispatch_table[]={
` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 const RtlGameIdentity identity={.struct_size=RTL_GAME_IDENTITY_V1_SIZE,.game_id="poll",.display_name="Poll",.save_name_prefix="poll"};
 const RtlGameExecutionApi execution={.struct_size=RTL_GAME_EXECUTION_API_V4_SIZE,.run_frame=frame,.poll_wait=poll_wait};
 const RtlGameModule module={.abi_version=RTL_GAME_MODULE_ABI_VERSION,.struct_size=RTL_GAME_MODULE_V2_SIZE,
  .capabilities=RTL_GAME_MODULE_CAP_IDENTITY|RTL_GAME_MODULE_CAP_EXECUTION,.identity=&identity,.execution=&execution};
 if(RtlRegisterGame(&module)!=SR_RESULT_OK) return 1;
 for(unsigned mx=0;mx<4;mx++) for(unsigned value=0;value<4;value+=3) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.A=0x5600;cpu.X=31;cpu.Y=48;cpu.D=0x121;cpu.DB=0;cpu.PB=0;
  cpu.P=(uint8)(0x45|(mx&2?CPU_P_M:0)|(mx&1?CPU_P_X:0));cpu_p_to_mirrors(&cpu);uint8 old_p=cpu.P;
  cpu.S=0x1ffc;cpu.host_return_valid=1;cpu_write8(&cpu,0,0x1ffd,0x34);cpu_write8(&cpu,0,0x1ffe,0x12);cpu_write8(&cpu,0,0x1fff,0);
  g_ram[0x30]=(uint8)value;g_ram[0x31]=0;polls=0;WatchdogFrameStart();
  RecompReturn result=g_dispatch_table[0].variant[mx](&cpu);cpu_mirrors_to_p(&cpu);
  if(result!=RECOMP_RETURN_NORMAL||failed||polls!=value||g_ram[0x30]||g_ram[0x31]||
    cpu.A!=(mx&2?0x5600:0)||cpu.X!=32||cpu.Y!=48||cpu.D!=0x121||cpu.P!=old_p||cpu.S!=0x1fff||g_recomp_stack_top) {
   fprintf(stderr,"poll native mx=%u initial=%u polls=%u failed=%u A=%04x X=%04x S=%04x P=%02x\n",mx,value,polls,failed,cpu.A,cpu.X,cpu.S,cpu.P);return 2;
  }
 }
 puts("native poll wait: all M/X, compiled DEC/RTI releases original read loop, exact callbacks/continuation, CPU/stack restored PASS");return 0;
}
`,
	}, "poll_contract")
}
