package emitter

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// Original redistributable fixture: a routine conditionally branches to a
// sibling body whose RTL belongs to the SAME JSL activation, not a new call.
func TestSplitTailKeepsPairedReturnContext(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xad, 0x10, 0x00, 0xd0, 0x01, 0x6b, 0xe8, 0x6b})
	for _, mx := range []config.MX{{M: 0, X: 0}, {M: 0, X: 1}, {M: 1, X: 0}, {M: 1, X: 1}} {
		context := codegen.NewContext()
		context.Names[0x008000] = "Head"
		context.Names[0x008006] = "Body"
		source, err := EmitBank(image, 0, []config.Entry{
			{Name: "Head", Start: 0x8000, EntryMX: mx},
			{Name: "Body", Start: 0x8006, EntryMX: mx},
		}, BankOptions{Context: context})
		if err != nil {
			t.Fatal(err)
		}
		for _, want := range []string{
			"cpu_dispatch_paired_tail_from(cpu, (((uint32)cpu->PB << 16) | 0x8006u), _entry_s, _hrv, 0x008006u)",
			"cpu_tailcall_inherit_return_context(_entry_s, _hrv)",
			"return RECOMP_RETURN_TAILCALL", "/* RTL host return */",
		} {
			if !strings.Contains(source, want) {
				t.Fatalf("M%dX%d missing %q:\n%s", mx.M, mx.X, want, source)
			}
		}
		if strings.Contains(source, "return cpu_dispatch_pc_from(cpu, 0x008006u") {
			t.Fatal("paired split tail still drops return context")
		}
	}
}

func TestDispatchReturnUsesSplitTailContract(t *testing.T) {
	context := codegen.NewContext()
	context.Names[0x008010] = "Continuation"
	pc := uint16(0x8010)
	instruction := &cpu65816.Instruction{Address: 0x008000, DispatchReturn: &pc}
	source := strings.Join(dispatchReturnTransfer(context, instruction, nil, 1, 0), "\n")
	if !strings.Contains(source, "cpu_dispatch_paired_tail_from(cpu, (((uint32)cpu->PB << 16) | 0x8010u), _entry_s, _hrv, 0x008010u)") {
		t.Fatalf("dispatch return bypasses common tail contract: %s", source)
	}
	local := map[decoder.DecodeKey]struct{}{{PC: 0x008010, M: 1, X: 0}: {}}
	if got := strings.Join(dispatchReturnTransfer(context, instruction, local, 1, 0), "\n"); got != "  goto L_8010_M1X0;" {
		t.Fatalf("local continuation lost direct goto: %s", got)
	}
}
