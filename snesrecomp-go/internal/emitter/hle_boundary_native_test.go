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

func TestConfiguredHLEKindsStopPredecessorDecode(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xea, 0xe6, 0x10, 0x60})
	for name, cfg := range map[string]*config.Config{
		"function":    {HLEFunctions: map[uint16]string{0x8001: "Hook"}},
		"conditional": {HLEFunctionsIf: map[uint16]config.HLEFunctionIf{0x8001: {Function: "Hook", Predicate: "Enabled"}}},
		"SPC upload":  {HLESPCUpload: []uint16{0x8001}},
	} {
		t.Run(name, func(t *testing.T) {
			graph, err := decoder.DecodeFunction(image, 0, 0x8000, 1, 0, DecodeOptionsFromConfig(0, cfg))
			if err != nil {
				t.Fatal(err)
			}
			if len(graph.KeysAtPC(0x8001)) != 0 || len(graph.KeysAtPC(0x8000)) != 1 {
				t.Fatal("configured interception did not terminate its predecessor")
			}
		})
	}
}

// Execute emitted code and the real return dispatcher. Calling an HLE directly
// cannot show whether a predecessor bypasses it or loses a pushed continuation.
func TestHLEBoundariesAndPushedReturnNative(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0x20, 0, 0x81, 0xe6, 0x10, 0x20, 0, 0x82, 0xe6, 0x11,
		0x20, 0, 0x83, 0xe6, 0x12, 0x20, 0, 0x84, 0xe6, 0x13, 0x60})
	copy(image[0x100:], []byte{0xe6, 0x20, 0xe6, 0x22, 0x60}) // fall into hook
	copy(image[0x200:], []byte{0xd0, 2, 0xea, 0xea, 0xe6, 0x24, 0x60})
	copy(image[0x300:], []byte{0x00, 0, 0xe6, 0x26, 0x60})             // BRK continuation
	copy(image[0x400:], []byte{0x80, 3, 0xe6, 0x28, 0x60, 0x80, 0xfb}) // backward branch
	// PHP/REP; push cleanup and handler targets; RTS. The handler's current
	// stack differs from the original host entry, exactly as in actor dispatch.
	copy(image[0x500:], []byte{0x08, 0xc2, 0x20, 0xf4, 0x1f, 0x85, 0xf4, 0x2f, 0x85, 0x60})
	copy(image[0x520:], []byte{0x28, 0xe6, 0x14, 0x60})
	copy(image[0x530:], []byte{0x60})
	// An authored byte end at $8702 splits the M1 stream before NOP,
	// while M0's three-byte immediate continues at $8703.
	copy(image[0x700:], []byte{0xa9, 0x11, 0xea, 0x60})
	ctx := codegen.NewContext()
	var entries []config.Entry
	var table, bytes strings.Builder
	for _, pc := range []uint16{0x8000, 0x8100, 0x8102, 0x8104, 0x8200, 0x8204, 0x8206,
		0x8300, 0x8302, 0x8304, 0x8400, 0x8402, 0x8404, 0x8500, 0x8520, 0x8530,
		0x8700, 0x8702, 0x8703} {
		name := fmt.Sprintf("Body%04X", pc)
		ctx.Names[uint32(pc)] = name
		fmt.Fprintf(&table, "{0x%04xu,{", pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				entry := config.Entry{Name: name, Start: pc, EntryMX: config.MX{M: m, X: x}}
				if pc == 0x8700 || pc == 0x8702 {
					end := uint16(0x8703)
					if pc == 0x8700 {
						end = 0x8702
					}
					entry.End = &end
				}
				entries = append(entries, entry)
				fmt.Fprintf(&table, "%s_M%dX%d,", name, m, x)
			}
		}
		table.WriteString("}},\n")
	}
	hooks := map[uint16]config.HLEFunctionIf{}
	for _, pc := range []uint16{0x8102, 0x8204, 0x8302, 0x8402} {
		hooks[pc] = config.HLEFunctionIf{Function: fmt.Sprintf("Hook%04X", pc), Predicate: "Enabled"}
	}
	hooks[0x8530] = config.HLEFunctionIf{Function: "Departure", Predicate: "DepartureEntry"}
	source, err := EmitBank(image, 0, entries, BankOptions{Context: ctx, HLEFunctionsIf: hooks, UnresolvedAllowed: true})
	if err != nil {
		t.Fatal(err)
	}
	for _, pc := range []uint16{0x8102, 0x8204, 0x8302, 0x8402} {
		// Four root variants can contain their fallback; no predecessor may.
		if count := strings.Count(source, fmt.Sprintf("cpu_trace_block(cpu, 0x00%04X)", pc)); count != 4 {
			t.Fatalf("HLE $%04X has %d native copies, want four root fallbacks", pc, count)
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
		"generated.c": source,
		"CMakeLists.txt": `cmake_minimum_required(VERSION 3.20)
project(HLEBoundaries LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(hle_boundaries fixture.c)
target_link_libraries(hle_boundaries PRIVATE snesrecomp_runtime)
set_property(TARGET hle_boundaries PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include <stdio.h>
#include <string.h>
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/generated_support.h"
#include "generated.c"
void RtlApuLock(void) {} void RtlApuUnlock(void) {}
extern bool g_fail;
extern unsigned sr_diagnostic_trap_warning_count(void);
static const uint8 fixture_rom[0x8000]={` + bytes.String() + `};
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
static bool enabled, bypass;
static unsigned calls;
bool Enabled(CpuState *cpu) { (void)cpu;return enabled; }
static RecompReturn Prefix(CpuState *cpu,unsigned pc,unsigned marker) {
 ++calls;++g_ram[marker];
 if(!cpu_hle_tailcall_request(pc+2,pc))return RECOMP_RETURN_SKIP_1;
 return RECOMP_RETURN_TAILCALL;
}
RecompReturn Hook8102(CpuState*c){return Prefix(c,0x8102,0x32);}
RecompReturn Hook8204(CpuState*c){return Prefix(c,0x8204,0x34);}
RecompReturn Hook8302(CpuState*c){return Prefix(c,0x8302,0x36);}
RecompReturn Hook8402(CpuState*c){return Prefix(c,0x8402,0x38);}
bool DepartureEntry(CpuState*c){(void)c;if(bypass){bypass=false;return false;}return true;}
RecompReturn Departure(CpuState*c){
 (void)c;++calls;bypass=true;
 if(!cpu_hle_tailcall_request(0x8530,0x8530))return RECOMP_RETURN_SKIP_1;
 return RECOMP_RETURN_TAILCALL;
}
static void Brk(CpuState*c){(void)c;}
int main(void) {
 g_rom=fixture_rom;g_cpu_brk_hook=Brk;
 for(unsigned on=0;on<2;on++)for(unsigned z=0;z<2;z++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);memset(g_ram,0,0x100);
  cpu.emulation=0;cpu.P=CPU_P_M|(z?CPU_P_Z:0);cpu_p_to_mirrors(&cpu);
  cpu.S=0x1ffd;cpu.host_return_valid=1;cpu_write16(&cpu,0,0x1ffe,0x1234);
  enabled=on;calls=0;WatchdogFrameStart();RecompReturn r=Body8000_M1X0(&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||!cpu.m_flag||cpu.x_flag||calls!=4*on||
    g_ram[0x10]!=1||g_ram[0x11]!=1||g_ram[0x12]!=1||g_ram[0x13]!=1)return 1;
  for(unsigned i=0;i<4;i++)if(g_ram[0x22+2*i]!=!on||g_ram[0x32+2*i]!=on)return 2;
 }
 for(unsigned p=0;p<256;p++) {
  if(p&CPU_P_X)continue;
  CpuState cpu;cpu_state_init(&cpu,g_ram);memset(g_ram,0,0x100);
  cpu.emulation=0;cpu.P=p;cpu_p_to_mirrors(&cpu);cpu.S=0x1ffd;cpu.host_return_valid=1;
  cpu_write16(&cpu,0,0x1ffe,0x1234);calls=0;WatchdogFrameStart();
  RecompReturn r=(p&CPU_P_M)?Body8500_M1X0(&cpu):Body8500_M0X0(&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.S!=0x1fff||cpu.m_flag!=!!(p&CPU_P_M)||
    cpu.x_flag||calls!=1||g_ram[0x14]!=1||g_ram[0x15]||g_recomp_stack_top||
    g_cpu_return_scope||g_cpu_owned_unwind_scope||g_fail||sr_diagnostic_trap_warning_count()) {
    fprintf(stderr,"p=%02x r=%d S=%04x m=%u calls=%u cleanup=%u depth=%d\n",
      p,r,cpu.S,cpu.m_flag,calls,g_ram[0x14],g_recomp_stack_top);return 3;
  }
 }
 for(unsigned m=0;m<2;m++) {
  CpuState cpu;cpu_state_init(&cpu,g_ram);cpu.emulation=0;cpu.P=m?CPU_P_M:0;
  cpu_p_to_mirrors(&cpu);cpu.S=0x1ffd;cpu.host_return_valid=1;
  cpu_write16(&cpu,0,0x1ffe,0x1234);WatchdogFrameStart();
  RecompReturn r=m?Body8700_M1X0(&cpu):Body8700_M0X0(&cpu);
  if(r!=RECOMP_RETURN_NORMAL||cpu.A!=(m?0x11:0xea11)||cpu.S!=0x1fff||
    cpu.m_flag!=m||cpu.x_flag||g_recomp_stack_top||g_fail||sr_diagnostic_trap_warning_count())return 4;
 }
 puts("HLE routing, exact-width boundary continuations and pushed-return PLP: PASS");return 0;
}
`,
	}, "hle_boundaries")
}
