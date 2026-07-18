package decoder

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

type labeledSuccessor struct {
	Key  DecodeKey
	Kind string
}

func labeledSuccessors(image rom.Image, instruction *cpu65816.Instruction, key DecodeKey, bank byte, options Options) []labeledSuccessor {
	post := PostState(instruction, key)
	pc := uint16(instruction.Address)
	nextPC := pc + uint16(instruction.Length)
	makeKey := func(target uint16, m, x uint8, keepStack bool) DecodeKey {
		result := DecodeKey{PC: Address24(bank, target), M: m & 1, X: x & 1}
		if keepStack {
			result.PStack, result.PDepth = post.PStack, post.PDepth
		}
		return result
	}
	switch instruction.Mnemonic {
	case "RTS", "RTL", "RTI", "STP", "WAI":
		return nil
	case "BRK":
		return []labeledSuccessor{{makeKey(nextPC, post.M, post.X, true), "fall_brk"}}
	case "BRA", "BRL":
		return []labeledSuccessor{{makeKey(uint16(instruction.Operand), post.M, post.X, true), "jump"}}
	case "BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ":
		return []labeledSuccessor{
			{makeKey(nextPC, post.M, post.X, true), "fall"},
			{makeKey(uint16(instruction.Operand), post.M, post.X, true), "jump"},
		}
	case "JMP":
		if instruction.Mode == cpu65816.ABS {
			return []labeledSuccessor{{makeKey(uint16(instruction.Operand), post.M, post.X, true), "jump"}}
		}
		return nil
	case "JSR", "JSL":
		return callSuccessors(image, instruction, post, bank, nextPC, options)
	default:
		return []labeledSuccessor{{makeKey(nextPC, post.M, post.X, true), "fall"}}
	}
}

func callSuccessors(image rom.Image, instruction *cpu65816.Instruction, post DecodeKey, bank byte, nextPC uint16, options Options) []labeledSuccessor {
	target, hasTarget := uint32(0), false
	if instruction.Mnemonic == "JSR" && instruction.Length == 3 {
		target, hasTarget = Address24(bank, uint16(instruction.Operand)), true
	} else if instruction.Mnemonic == "JSL" {
		target, hasTarget = instruction.Operand&0xffffff, true
	}
	returnM, returnX := post.M, post.X
	if hasTarget && options.CalleeExitMX != nil {
		exit, found := options.CalleeExitMX[Variant{target, post.M, post.X}]
		if !found {
			targetBank := byte(target >> 16)
			if targetBank < 0x40 || (targetBank >= 0x80 && targetBank < 0xc0) {
				exit, found = options.CalleeExitMX[Variant{target ^ 0x800000, post.M, post.X}]
			}
		}
		if found && exit.M >= 0 && exit.X >= 0 {
			returnM, returnX = uint8(exit.M)&1, uint8(exit.X)&1
		}
	}
	if hasTarget && options.CalleeExitModes != nil && returnM == post.M && returnX == post.X {
		modes, found := options.CalleeExitModes[Variant{target, post.M, post.X}]
		if !found {
			targetBank := byte(target >> 16)
			if targetBank < 0x40 || (targetBank >= 0x80 && targetBank < 0xc0) {
				modes, found = options.CalleeExitModes[Variant{target ^ 0x800000, post.M, post.X}]
			}
		}
		if found && len(modes) <= 2 {
			offset, err := rom.LoROMOffset(bank, nextPC)
			if err != nil || offset >= len(image) {
				found = false
			} else {
				next, decodeErr := cpu65816.Decode(image, offset, nextPC, bank, post.M, post.X)
				if decodeErr != nil || next == nil || !isConditional(next.Mnemonic) {
					found = false
				}
			}
		}
		if found {
			sort.Slice(modes, func(i, j int) bool {
				if modes[i].M != modes[j].M {
					return modes[i].M < modes[j].M
				}
				return modes[i].X < modes[j].X
			})
			seen := map[[2]uint8]struct{}{}
			var successors []labeledSuccessor
			for _, mode := range modes {
				if mode.M < 0 || mode.X < 0 {
					continue
				}
				pair := [2]uint8{uint8(mode.M) & 1, uint8(mode.X) & 1}
				if _, duplicate := seen[pair]; duplicate {
					continue
				}
				seen[pair] = struct{}{}
				successors = append(successors, labeledSuccessor{DecodeKey{PC: Address24(bank, nextPC), M: pair[0], X: pair[1], PStack: post.PStack, PDepth: post.PDepth}, "fall"})
			}
			if len(successors) > 0 {
				return successors
			}
		}
	}
	return []labeledSuccessor{{DecodeKey{PC: Address24(bank, nextPC), M: returnM, X: returnX, PStack: post.PStack, PDepth: post.PDepth}, "fall"}}
}

func isConditional(mnemonic string) bool {
	switch mnemonic {
	case "BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ":
		return true
	}
	return false
}
