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

func TestCalleeCleanNativeExecution(t *testing.T) {
	if testing.Short() {
		t.Skip("native conformance build")
	}
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("native conformance needs CMake and C/C++")
	}
	for _, kind := range []string{"short", "long", "recursive-split", "interrupt", "hle", "reset", "retired-caller", "retired-caller-recursive"} {
		t.Run(kind, func(t *testing.T) { testCalleeCleanNative(t, cmake, kind) })
	}
}

func testCalleeCleanNative(t *testing.T, cmake, kind string) {
	image := make(rom.Image, 0x80000)
	// Preserve live P, supply three words, then check that the continuation
	// executes once with all arguments removed, before restoring P and RTS.
	caller := []byte{0x08, 0xc2, 0x30}
	retired := strings.HasPrefix(kind, "retired-caller")
	if retired {
		caller = []byte{0xc2, 0x30, 0x68, 0x08} // consume incoming word, then save P
	}
	if kind == "reset" {
		caller = append([]byte{0xc2, 0x30, 0xa9, 0xff, 0x1f, 0x1b}, caller...)
	}
	if kind != "hle" {
		caller = append(caller, 0xf4, 1, 0, 0xf4, 2, 0, 0xf4, 3, 0)
	}
	bank := byte(0)
	if kind == "long" {
		bank = 1
		caller = append(caller, 0x22, 0x40, 0x80, 1)
	} else {
		caller = append(caller, 0x20, 0x40, 0x80)
	}
	returnWord := uint16(0x8000 + len(caller) - 1)
	caller = append(caller, 0xee, 0x10, 0, 0x28, 0x60)
	if kind == "reset" || retired {
		caller[len(caller)-1] = 0xdb
	} // terminal synthetic STP, not RTS from reset
	copy(image, caller)
	// Eight local bytes; move the original return word above six argument bytes.
	callee := []byte{}
	if kind == "recursive-split" || kind == "retired-caller-recursive" {
		// All recursive invocations use the same call site/return PC. The
		// cleanup is a separate generated tail body, inheriting its owner.
		callee = append(callee, 0xce, 0x20, 0, 0xf0, 15, 0xf4, 1, 0, 0xf4, 2, 0, 0xf4, 3, 0, 0x20, 0x40, 0x80, 0xee, 0x22, 0)
	}
	cleanup := uint16(0x8040 + len(callee))
	callee = append(callee, 0xf4, 0, 0, 0xf4, 0, 0, 0xf4, 0, 0, 0xf4, 0, 0)
	if kind == "interrupt" {
		callee = append(callee, 0xad, 0x30, 0, 0xd0, 0xfb)
	}
	if retired {
		// Discard eight locals, compute final S past six argument bytes,
		// then pull THIS invocation's word and push it at the new position.
		callee = append(callee, 0x3b, 0x18, 0x69, 8, 0, 0x1b,
			0x3b, 0x18, 0x69, 8, 0, 0x7a, 0x1b, 0x5a, 0x9c, 0x18, 0, 0x60)
	} else {
		callee = append(callee, 0xa3, 9, 0x83, 15)
		if kind == "long" {
			callee = append(callee, 0xe2, 0x20, 0xa3, 11, 0x83, 17, 0xc2, 0x20)
		} // move bank byte too
		callee = append(callee, 0x3b, 0x18, 0x69, 14, 0, 0x1b)
		if kind == "long" {
			callee = append(callee, 0x6b)
		} else {
			callee = append(callee, 0x60)
		}
	}
	copy(image[int(bank)*0x8000+0x40:], callee)
	copy(image[0x200:], []byte{0xce, 0x30, 0, 0x40}) // ISR: DEC; RTI
	ctx := codegen.NewContext()
	ctx.ExactDirectCallMX = true
	entries := map[byte][]config.Entry{}
	var table, declarations strings.Builder
	roots := []struct {
		name string
		pc   uint32
	}{{"Caller", 0x8000}, {"Interrupt", 0x8200}, {"Cleaner", uint32(bank)<<16 | 0x8040}}
	if kind == "recursive-split" || kind == "retired-caller-recursive" {
		roots = append(roots, struct {
			name string
			pc   uint32
		}{"Cleanup", uint32(cleanup)})
	}
	for _, root := range roots {
		ctx.Names[uint32(root.pc)] = root.name
		fmt.Fprintf(&table, "{0x%04xu,{", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				b := byte(root.pc >> 16)
				entries[b] = append(entries[b], config.Entry{Name: root.name, Start: uint16(root.pc), EntryMX: config.MX{M: m, X: x}})
				fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
				fmt.Fprintf(&declarations, "RecompReturn %s_M%dX%d(CpuState *cpu);\n", root.name, m, x)
			}
		}
		table.WriteString("}},\n")
	}
	opts := BankOptions{Context: ctx}
	if kind == "hle" {
		opts.HLEFunctions = map[uint16]string{0x8040: "legacy_hle"}
	}
	source, err := EmitBank(image, 0, entries[0], opts)
	if err != nil {
		t.Fatal(err)
	}
	if bank != 0 {
		other, e := EmitBank(image, bank, entries[bank], BankOptions{Context: ctx})
		if e != nil {
			t.Fatal(e)
		}
		source += other
	}
	expectedRecursive := 0
	if kind == "recursive-split" || kind == "retired-caller-recursive" {
		expectedRecursive = 2
	}
	expectedPolls := 0
	if kind == "interrupt" {
		expectedPolls = 3
	}
	expectedHLE := 0
	if kind == "hle" {
		expectedHLE = 1
	}
	isReset := 0
	if kind == "reset" {
		isReset = 1
	}
	expectedY, pMask := uint16(45), uint8(0xff)
	if retired {
		expectedY, pMask = returnWord, 0xcf
	}
	runNativeContract(t, cmake, map[string]string{
		"generated.c": source, "funcs.h": declarations.String(),
		"CMakeLists.txt": `cmake_minimum_required(VERSION 3.20)
project(CalleeCleanConformance LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(clean_contract fixture.c)
target_link_libraries(clean_contract PRIVATE snesrecomp_runtime)
set_property(TARGET clean_contract PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "generated.c"
extern bool g_fail;
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
RecompReturn legacy_hle(CpuState *cpu) { (void)cpu;++g_ram[0x12];return RECOMP_RETURN_NORMAL; }
static unsigned polls, failed;
static void frame(void) {}
static void poll(CpuState *cpu,uint32 pc,uint32 address,uint32 width) {
 cpu_mirrors_to_p(cpu);CpuState saved=*cpu;CpuReturnScope *owner=g_cpu_return_scope;int depth=g_recomp_stack_top;
 if(address!=0x30||width!=2||!owner) ++failed;
 cpu_push_interrupt_frame(cpu);cpu_write16(cpu,0,(uint16)(cpu->S+2u),(uint16)pc);
 WatchdogFrameStart();
 if(Interrupt_M0X0(cpu)!=RECOMP_RETURN_NORMAL) ++failed;
 cpu->host_return_valid=saved.host_return_valid;cpu_mirrors_to_p(cpu);++polls;
 if(cpu->S!=saved.S||cpu->P!=saved.P||cpu->A!=saved.A||g_cpu_return_scope!=owner||g_recomp_stack_top!=depth) ++failed;
}
const DispatchEntry g_dispatch_table[]={
` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
int main(void) {
 static const RtlGameIdentity identity={.struct_size=RTL_GAME_IDENTITY_V1_SIZE,.game_id="clean",.display_name="Clean",.save_name_prefix="clean"};
 static const RtlGameExecutionApi execution={.struct_size=RTL_GAME_EXECUTION_API_V4_SIZE,.run_frame=frame,.poll_wait=poll};
 static const RtlGameModule module={.abi_version=RTL_GAME_MODULE_ABI_VERSION,.struct_size=RTL_GAME_MODULE_V2_SIZE,
  .capabilities=RTL_GAME_MODULE_CAP_IDENTITY|RTL_GAME_MODULE_CAP_EXECUTION,.identity=&identity,.execution=&execution};
 if(RtlRegisterGame(&module)!=SR_RESULT_OK) return 2;
 for(unsigned mx=0;mx<4;mx++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.S=0x1ffd;cpu.host_return_valid=1;
  cpu.P=(uint8)(0x45|(mx&2?CPU_P_M:0)|(mx&1?CPU_P_X:0));cpu_p_to_mirrors(&cpu);uint8 old_p=cpu.P;
` + fmt.Sprintf("  old_p &= 0x%02xu;\n", pMask) + `
  cpu.X=23;cpu.Y=45;cpu.D=0;cpu.DB=0;cpu.PB=0;
  cpu_write16(&cpu,0,0x1ffe,0x1234);memset(g_ram+0x10,0,0x30);g_ram[0x20]=3;g_ram[0x30]=3;polls=0;WatchdogFrameStart();
` + fmt.Sprintf("  const int reset=%d;\n", isReset) + `
  CpuReturnScope reset_owner;
  if(reset) {cpu.S=0x1ff;cpu.host_return_valid=0;old_p&=(uint8)~0x30u;cpu_reset_scope_begin(&reset_owner,&cpu);}
  RecompReturn r=g_dispatch_table[0].variant[mx](&cpu);cpu_mirrors_to_p(&cpu);
  if(reset) cpu_return_scope_end(&reset_owner);
  if(r!=RECOMP_RETURN_NORMAL||g_fail||cpu.S!=0x1fff||cpu.P!=old_p||cpu.X!=23||
` + fmt.Sprintf("     cpu.Y!=0x%04xu||\n", expectedY) + `
     g_ram[0x10]!=1||g_ram[0x11]||g_recomp_stack_top||g_cpu_return_scope||failed||
` + fmt.Sprintf("     g_ram[0x22]!=%d||polls!=%d||g_ram[0x12]!=%d", expectedRecursive, expectedPolls, expectedHLE) + `) {
   fprintf(stderr,"callee-clean mx=%u r=%d fail=%d S=%04x P=%02x expected=%02x once=%u depth=%d\n",mx,r,g_fail,cpu.S,cpu.P,old_p,g_ram[0x10],g_recomp_stack_top);return 1;
  }
 }
 puts("native callee-clean: original relocated frame, one continuation, CPU/stack/ownership restored, all M/X PASS");return 0;
}
`,
	}, "clean_contract")
}
