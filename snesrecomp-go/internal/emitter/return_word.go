package emitter

import (
	"fmt"

	"github.com/DerrickGold/snesrecomp-go/internal/cfg"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
)

// A local def-use contract, not a callee summary or a guessed entry kind:
// pull a word, set S using a DIFFERENT register, push the unchanged word, RTS.
// The runtime witnesses that the pull actually came from this immediate
// call's incoming frame. All guest instructions still execute normally.
// One basic block is essential: no entry/branch may bypass the witness.
type returnWordShuttle struct {
	pull, push int
	register   string
	widthFlag  string
}

func findReturnWordShuttle(block *cfg.Block, options FunctionOptions) *returnWordShuttle {
	ins := block.Instructions
	if len(ins) < 4 || options.TailCallPC != nil || options.HLEFunction != "" || options.HLESPCUpload {
		return nil
	}
	last := ins[len(ins)-1].Instruction
	if last.Opcode != 0x60 || last.DispatchKind != "" || last.DispatchEntries != nil {
		return nil
	}
	push := len(ins) - 2
	// Permit only a bounded suffix of stores with a runtime WRAM/disjointness
	// guard. No call, branch, flag/register write, MMIO assumption, or alias
	// assumption is allowed between the saved word and its final consumption.
	for push >= 0 && len(ins)-2-push < 4 && returnWordGuardedStore(ins[push].Instruction) {
		push--
	}
	if push < 2 {
		return nil
	}
	pull := push - 2
	r := &returnWordShuttle{pull: pull, push: push, widthFlag: "x"}
	pop, set, put := ins[pull].Instruction.Opcode, ins[pull+1].Instruction.Opcode, ins[push].Instruction.Opcode
	switch {
	case pop == 0x7a && set == 0x1b && put == 0x5a: // PLY; TCS; PHY
		r.register = "Y"
	case pop == 0xfa && set == 0x1b && put == 0xda: // PLX; TCS; PHX
		r.register = "X"
	case pop == 0x68 && set == 0x9a && put == 0x48: // PLA; TXS; PHA
		r.register, r.widthFlag = "A", "m"
	default:
		return nil
	}
	for _, d := range ins[pull:] {
		i := d.Instruction
		if d.Key.X != 0 || (r.register == "A" && d.Key.M != 0) || i.DispatchKind != "" || i.DispatchEntries != nil {
			return nil
		}
		pc := uint16(i.Address)
		if options.HLEDispatch[pc] != "" || options.Decode.HLEDispatch[pc] != "" {
			return nil
		}
		for _, excluded := range options.ExcludeRanges {
			if pc >= excluded[0] && pc <= excluded[1] {
				return nil
			}
		}
		for _, barrier := range options.Decode.NativeReturnBarriers {
			if i.Address <= barrier[1] && i.Address+uint32(i.Length)-1 >= barrier[0] {
				return nil
			}
		}
	}
	return r
}

func returnWordGuardedStore(i *cpu65816.Instruction) bool {
	return i.Opcode == 0x64 || i.Opcode == 0x9c // STZ dp / STZ abs, no indexing
}

func returnWordStoreGuard(i *cpu65816.Instruction, m uint8) string {
	bank, address := "cpu->DB", fmt.Sprintf("0x%04xu", uint16(i.Operand))
	if i.Opcode == 0x64 {
		bank, address = "0x00u", fmt.Sprintf("(uint16)(cpu->D + 0x%02xu)", byte(i.Operand))
	}
	return fmt.Sprintf("if (cpu->m_flag != %du || !cpu_return_word_store_disjoint(cpu, %s, %s, %du)) _return_origin = NULL;", m, bank, address, 2-m)
}
