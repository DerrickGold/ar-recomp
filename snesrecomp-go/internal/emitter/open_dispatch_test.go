package emitter

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestOpenJumpUsesActualTargetAndLocalFastPath(t *testing.T) {
	for _, opcode := range []byte{0x6c, 0x7c} {
		ctx := codegen.NewContext()
		i := &cpu65816.Instruction{Address: 0x8000, Opcode: opcode, Mnemonic: "JMP", Operand: 0x9000,
			DispatchEntries: []uint32{0x9100, 0, 0x9100}, DispatchOpen: true}
		local := map[decoder.DecodeKey]struct{}{{PC: 0x9100, M: 1, X: 0}: {}}
		source := strings.Join(emitIndirectDispatch(ctx, i, local), "\n")
		for _, want := range []string{"cpu_read16_bank_wrap", "cpu_accept_indirect_return", "goto L_9100_M1X0", "cpu_dispatch_has_entry", "cpu_trace_trapped_dispatch", "cpu_dispatch_paired_tail_from"} {
			if !strings.Contains(source, want) {
				t.Fatalf("missing %q:\n%s", want, source)
			}
		}
		if strings.Count(source, "goto L_9100_M1X0") != 1 || len(ctx.Demands) != 4 || strings.Contains(source, "dispatch_oob") || strings.Contains(source, "_idx") {
			t.Fatalf("open prefix became a selector bound or lost exact demands:\n%s", source)
		}
		if opcode == 0x7c && !strings.Contains(source, "(uint16)(0x9000u + cpu->X)") {
			t.Fatal("odd and out-of-prefix byte indices must address the actual ROM word")
		}
		if opcode == 0x6c && !strings.Contains(source, "cpu_read16_bank_wrap(cpu, 0x00,") {
			t.Fatal("absolute-indirect pointer must be read from bank zero")
		}
	}
}
