package decoder

import (
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// This deliberately narrow whole-body contract recognizes a native JSL
// return-table helper, not a byte search for an interesting suffix. PHP/PLP
// restores live P; X, Y and D are saved after REP #$30; TSC/TCD aliases the
// stack; [$08],Y reads from the original three-byte JSL return frame. The
// replacement word is decremented before RTL, leaving its actual target in A.
// No callee is HLE'd and no caller flags/register effects are synthesized.
func decodeNativeReturnTable(image rom.Image, call *cpu65816.Instruction, options Options) bool {
	if call.Mnemonic != "JSL" || nativeReturnBlocked(call.Address, call.Address+3, options) {
		return false
	}
	pc := call.Operand & 0xffffff
	start := pc
	steps := []struct {
		name string
		mode cpu65816.AddressingMode
		arg  uint32
	}{
		{"PHP", cpu65816.IMP, 0}, {"REP", cpu65816.IMM, 0x30},
		{"PHX", cpu65816.IMP, 0}, {"PHY", cpu65816.IMP, 0},
		{"AND", cpu65816.IMM, 0xff}, {"ASL", cpu65816.ACC, 0},
		{"TAY", cpu65816.IMP, 0}, {"INY", cpu65816.IMP, 0},
		{"PHD", cpu65816.IMP, 0}, {"TSC", cpu65816.IMP, 0}, {"TCD", cpu65816.IMP, 0},
		{"LDA", cpu65816.INDIRLY, 8}, {"STA", cpu65816.DP, 8}, {"DEC", cpu65816.DP, 8},
		{"PLD", cpu65816.IMP, 0}, {"PLY", cpu65816.IMP, 0}, {"PLX", cpu65816.IMP, 0},
		{"PLP", cpu65816.IMP, 0}, {"RTL", cpu65816.IMP, 0},
	}
	for _, step := range steps {
		if byte(pc>>16) != byte(start>>16) || uint16(pc) < 0x8000 {
			return false
		}
		offset, err := rom.LoROMOffset(byte(pc>>16), uint16(pc))
		if err != nil || offset >= len(image) {
			return false
		}
		ins, err := cpu65816.Decode(image, offset, uint16(pc), byte(pc>>16), 0, 0)
		if err != nil || ins == nil || ins.Mnemonic != step.name || ins.Mode != step.mode || ins.Operand != step.arg {
			return false
		}
		pc += uint32(ins.Length)
	}
	if nativeReturnBlocked(start, pc-1, options) {
		return false
	}
	// Stay within the current LoROM mapper's readable interval; never infer a
	// wrapping inline table or use a below-$8000 policy as a universal mapper.
	table := call.Address + 4
	if table>>16 != call.Address>>16 || uint16(table) < 0x8000 {
		return false
	}
	evidence := &cpu65816.NativeReturnTable{TablePC: table, ReturnPC: pc - 1}
	// Each word is an actual ROM value read by a possible selector. Stop at
	// the earliest handler, an explicit data target, or an unmapped target.
	// This only chooses which bodies to discover: the emitted runtime transfer
	// always uses the real A, including selectors OUTSIDE this open prefix.
	end := uint32(0x10000)
	for index := uint32(0); index < 256; index++ {
		address := uint32(uint16(table)) + 2*index
		if address+2 > end {
			break
		}
		offset, err := rom.LoROMOffset(byte(table>>16), uint16(address))
		if err != nil || offset+2 > len(image) {
			break
		}
		target := uint32(image[offset]) | uint32(image[offset+1])<<8
		targetOffset, err := rom.LoROMOffset(byte(table>>16), uint16(target))
		if err != nil || targetOffset >= len(image) ||
			inDataRegion(options.DataRegions, byte(table>>16), uint16(target)) ||
			(target >= uint32(uint16(table)) && target < address+2) {
			break
		}
		evidence.Targets = append(evidence.Targets, table&0xff0000|target)
		if target >= uint32(uint16(table)) && target < end {
			end = target
		}
	}
	call.NativeReturnTable = evidence
	return true
}

func nativeReturnBlocked(start, end uint32, options Options) bool {
	for _, barrier := range options.NativeReturnBarriers {
		if start <= barrier[1] && end >= barrier[0] {
			return true
		}
	}
	for _, region := range options.DataRegions {
		if start <= Address24(region.Bank, region.End)-1 && end >= Address24(region.Bank, region.Start) {
			return true
		}
	}
	return false
}
