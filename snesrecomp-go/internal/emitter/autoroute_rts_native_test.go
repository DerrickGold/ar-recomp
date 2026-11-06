package emitter

import (
	"fmt"
	"os/exec"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// The autoroute inventories raw handler PCs at the RTS, leaving all preceding
// instructions native. Compare that guard with an unconfigured RTS using the
// real runtime; parked handler endpoints expose the exact post-transfer CPU and
// stack without assuming that an M1 PHA supplied a complete return word.
func TestAutorouteRTSGuardNativeExecution(t *testing.T) {
	if testing.Short() {
		t.Skip("native conformance")
	}
	cmake, err := exec.LookPath("cmake")
	if err != nil {
		t.Skip("native conformance needs CMake")
	}
	image := make(rom.Image, 0x8000)
	roots := []struct {
		name    string
		pc      uint16
		opcode  byte
		guarded bool
	}{
		{"GuardX", 0x8000, 0xbd, true}, {"OpenX", 0x8010, 0xbd, false},
		{"GuardY", 0x8020, 0xb9, true}, {"OpenY", 0x8030, 0xb9, false},
	}
	for _, root := range roots {
		copy(image[int(root.pc)-0x8000:], []byte{root.opcode, 0x00, 0x90, 0x3a, 0x48, 0xe2, 0x30, 0x60})
	}
	// Raw table words are final PCs: DEC and the RTS increment cancel. A high
	// 16-bit index reads the second table before SEP truncates the registers.
	copy(image[0x1000:], []byte{0x00, 0x81, 0x00, 0x82})
	copy(image[0x1100:], []byte{0x00, 0x82})
	for n := 0; n < 3; n++ {
		// STZ dp does not change A, X, Y or flags. WAI/BRA parks at the exact
		// selected handler instead of consuming an artificial caller frame.
		copy(image[0x100+n*0x100:], []byte{0x64, byte(0x20 + n), 0xcb, 0x80, 0xfd})
	}
	var source, decl, table, bytes strings.Builder
	for _, root := range roots {
		cfg := &config.Config{Bank: 0}
		if root.guarded {
			cfg.RTSDispatch = []config.RTSDispatch{{SitePC: root.pc + 7, Targets: []uint16{0x8100, 0x8200}}}
		}
		fmt.Fprintf(&table, "{0x%04xu,{", root.pc)
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				r, err := EmitFunction(image, 0, root.pc, m, x, FunctionOptions{
					Name: root.name, Decode: DecodeOptionsFromConfig(0, cfg), ParkClosedWaits: true,
				})
				if err != nil {
					t.Fatal(err)
				}
				if root.guarded && (!strings.Contains(r.Source, "goto L_8100_M1X1") || !strings.Contains(r.Source, "goto L_8200_M1X1")) {
					t.Fatalf("%s M%dX%d did not emit local raw-target guards", root.name, m, x)
				}
				if !root.guarded && strings.Contains(r.Source, "_rts_t") {
					t.Fatalf("%s unexpectedly received a dispatch guard", root.name)
				}
				source.WriteString(r.Source)
				fmt.Fprintf(&decl, "RecompReturn %s_M%dX%d(CpuState*);\n", root.name, m, x)
				fmt.Fprintf(&table, "%s_M%dX%d,", root.name, m, x)
			}
		}
		table.WriteString("}},\n")
	}
	for n := 0; n < 3; n++ {
		pc := uint16(0x8100 + n*0x100)
		name := fmt.Sprintf("Handler%d", n)
		r, err := EmitFunction(image, 0, pc, 1, 1, FunctionOptions{Name: name, ParkClosedWaits: true})
		if err != nil {
			t.Fatal(err)
		}
		source.WriteString(r.Source)
		fmt.Fprintf(&decl, "RecompReturn %s_M1X1(CpuState*);\n", name)
		fmt.Fprintf(&table, "{0x%04xu,{NULL,NULL,NULL,%s_M1X1}},\n", pc, name)
	}
	for i, b := range image {
		if b != 0 {
			fmt.Fprintf(&bytes, "[%d]=%d,", i, b)
		}
	}
	runNativeContract(t, cmake, map[string]string{
		"generated.c": source.String(), "funcs.h": decl.String(),
		"CMakeLists.txt": `cmake_minimum_required(VERSION 3.20)
project(AutorouteRTSGuard LANGUAGES C CXX)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SNESRECOMP_ENABLE_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory("${SNESRECOMP_RUNTIME_ROOT}" runtime)
add_executable(autoroute_rts_guard fixture.c)
target_link_libraries(autoroute_rts_guard PRIVATE snesrecomp_runtime)
set_property(TARGET autoroute_rts_guard PROPERTY LINKER_LANGUAGE CXX)
`,
		"fixture.c": `#include <stdio.h>
#include <string.h>
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/generated_support.h"
#include "funcs.h"
#include "generated.c"
void RtlApuLock(void) {} void RtlApuUnlock(void) {}
extern bool g_fail;
extern unsigned sr_diagnostic_trap_warning_count(void);
static const uint8 fixture_rom[0x8000]={` + bytes.String() + `};
const DispatchEntry g_dispatch_table[]={` + table.String() + `};
const unsigned g_dispatch_table_count=sizeof(g_dispatch_table)/sizeof(g_dispatch_table[0]);
typedef struct Case {
    const char *name;
    unsigned mx, index, db, target, accumulator;
} Case;
static const Case cases[]={
    {"raw ROM word",0,0,0,0x8100,0x80ff},
    {"second ROM word",1,2,0,0x8200,0x81ff},
    {"live WRAM DB fallback",0,0,0x7e,0x8300,0x82ff},
    {"16-bit index before SEP",0,0x100,0,0x8200,0x81ff},
    {"M1 one-byte PHA X0",2,0,0,0x8200,0xbeff},
    {"M1 one-byte PHA X1",3,0,0,0x8200,0xbeff},
};
typedef struct Snapshot {
    CpuState cpu;
    RecompReturn result;
    uint32 wait_pc, resume_pc;
    uint8 stack[32], markers[3];
} Snapshot;
static int capture(Snapshot *out, const Case *test, unsigned use_y, unsigned guarded) {
    CpuState cpu;
    unsigned m=(test->mx>>1)&1u, x=test->mx&1u;
    uint16 expected_x=(uint16)(use_y?0x34u:(test->index&0xffu));
    uint16 expected_y=(uint16)(use_y?(test->index&0xffu):0x56u);
    uint16 root=(uint16)(0x8000u+use_y*0x20u+(guarded?0u:0x10u));
    memset(out,0,sizeof(*out));
    memset(g_ram,0,kSnesWramSize);
    cpu_state_init(&cpu,g_ram); cpu.emulation=0;
    cpu.P=(uint8)(CPU_P_C|CPU_P_V|CPU_P_I|CPU_P_D|(m?CPU_P_M:0)|(x?CPU_P_X:0));
    cpu_p_to_mirrors(&cpu);
    cpu.A=0xbe7a; cpu.S=0x1ff0; cpu.PB=0; cpu.DB=(uint8)test->db;
    cpu.X=(uint16)(use_y?(x?0x34u:0xab34u):test->index);
    cpu.Y=(uint16)(use_y?test->index:(x?0x56u:0xcd56u));
    memset(g_ram+0x1fe0,0xa5,32);
    /* With M1, PHA supplies only the low byte. The existing stack supplies
     * $81 as the high byte, so RTS reaches $8200 despite table word $8100. */
    g_ram[0x1ff1]=0x81;
    g_ram[0x20]=g_ram[0x21]=g_ram[0x22]=0xa5;
    cpu_write16(&cpu,0x7e,0x9000,0x8300);
    g_cpu_wait_pc24=g_cpu_wait_resume_pc24=0;
    WatchdogFrameStart();
    out->result=cpu_dispatch_pc_from(&cpu,root,cpu.S,0xffffff);
    out->cpu=cpu; out->wait_pc=g_cpu_wait_pc24; out->resume_pc=g_cpu_wait_resume_pc24;
    memcpy(out->stack,g_ram+0x1fe0,sizeof(out->stack));
    memcpy(out->markers,g_ram+0x20,sizeof(out->markers));
    unsigned selected=(test->target-0x8100u)/0x100u;
    if(out->result!=RECOMP_RETURN_PARKED_WAIT||out->wait_pc!=test->target+2u||
       out->resume_pc!=test->target+3u||cpu.A!=test->accumulator||
       cpu.X!=expected_x||cpu.Y!=expected_y||cpu.S!=0x1ff0u+m||
       cpu.DB!=test->db||cpu.PB||cpu.D||cpu.P!=0xfdu||cpu.m_flag!=1||cpu.x_flag!=1||
       !cpu._flag_N||cpu._flag_Z||!cpu._flag_C||!cpu._flag_V||!cpu._flag_D||!cpu._flag_I||
       g_recomp_stack_top||g_cpu_return_scope||g_cpu_owned_unwind_scope||g_fail||sr_diagnostic_trap_warning_count()) {
        fprintf(stderr,"%s %c guarded=%u: r=%d wait=%06x A=%04x X=%04x Y=%04x S=%04x P=%02x DB=%02x depth=%d\n",
            test->name,use_y?'Y':'X',guarded,out->result,out->wait_pc,cpu.A,cpu.X,cpu.Y,cpu.S,cpu.P,cpu.DB,g_recomp_stack_top);
        return 1;
    }
    for(unsigned n=0;n<3;n++) if(out->markers[n]!=(n==selected?0:0xa5)) return 2;
    /* Assert native writes independently of the before/after comparison. */
    for(unsigned n=0;n<sizeof(out->stack);n++) {
        unsigned address=0x1fe0u+n;
        uint8 expected=address==0x1ff1u?0x81:0xa5;
        if(address==0x1ff0u) expected=m?0xff:(uint8)(test->accumulator>>8);
        if(!m&&address==0x1fefu) expected=0xff;
        if(out->stack[n]!=expected) {
            fprintf(stderr,"%s stack[%04x]=%02x expected %02x\n",test->name,address,out->stack[n],expected);
            return 3;
        }
    }
    return 0;
}
int main(void) {
    g_rom=fixture_rom;
    for(unsigned use_y=0;use_y<2;use_y++) for(unsigned n=0;n<sizeof(cases)/sizeof(cases[0]);n++) {
        Snapshot open,guarded;
        if(capture(&open,&cases[n],use_y,0)||capture(&guarded,&cases[n],use_y,1)) return 1;
        if(memcmp(&open,&guarded,sizeof(open))) {
            fprintf(stderr,"guard differs from native/open execution: %s %c\n",cases[n].name,use_y?'Y':'X');return 2;
        }
    }
    puts("RTS guard/native parity: 12 X/Y cases; raw targets, live DB fallback, M1 stack/B, 16-bit index and exact CPU/stack/markers: PASS");
    return 0;
}
`,
	}, "autoroute_rts_guard")
}
