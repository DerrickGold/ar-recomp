package emitter

import (
	"fmt"

	"github.com/DerrickGold/snesrecomp-go/internal/cfg"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// A two-instruction load/test self-loop has no changing CPU state or memory
// effect between repeated reads. Offer its TAKEN backedge to an optional host
// scheduler, retaining the original read, flags and branch. This is structural
// evidence of a poll, not proof that an interrupt writes the polled byte, nor
// permission to clear it. The runtime only offers whole WRAM reads; hardware
// ports (and their side effects/timing) need a separate contract.
func memoryPollWait(block *cfg.Block, taken decoder.DecodeKey) string {
	if taken != block.Entry || len(block.Instructions) != 2 {
		return ""
	}
	read, branch := block.Instructions[0].Instruction, block.Instructions[1].Instruction
	if branch.Mnemonic != "BEQ" && branch.Mnemonic != "BNE" && branch.Mnemonic != "BMI" && branch.Mnemonic != "BPL" {
		return ""
	}
	width := 2 - int(read.M&1)
	switch read.Mnemonic {
	case "LDA":
	case "LDX", "LDY":
		width = 2 - int(read.X&1)
	default:
		return ""
	}
	address := ""
	switch read.Mode {
	case cpu65816.DP:
		address = fmt.Sprintf("(uint32)(uint16)(cpu->D + 0x%02xu)", read.Operand)
	case cpu65816.ABS:
		address = fmt.Sprintf("(((uint32)cpu->DB << 16) | 0x%04xu)", read.Operand)
	case cpu65816.LONG:
		address = fmt.Sprintf("0x%06xu", read.Operand&0xffffff)
	default:
		return ""
	}
	return fmt.Sprintf("cpu_poll_wait(cpu, 0x%06xu, %s, %du); ", taken.PC&0xffffff, address, width)
}
