package decoder

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
)

func AnalyzeExitModes(graph *Graph, callee map[Variant]MX) ([]MX, bool) {
	seen := map[[2]int8]struct{}{}
	for _, decoded := range graph.Instructions {
		instruction := decoded.Instruction
		if instruction.Mnemonic == "RTS" || instruction.Mnemonic == "RTL" || instruction.Mnemonic == "RTI" {
			if instruction.DispatchKind != "rts_trick" {
				seen[[2]int8{int8(instruction.M & 1), int8(instruction.X & 1)}] = struct{}{}
			}
			continue
		}
		if !isDispatchTerminator(decoded) {
			continue
		}
		if callee == nil {
			return nil, false
		}
		for _, target := range instruction.DispatchEntries {
			if target == 0 {
				continue
			}
			if instruction.DispatchKind != "long" {
				target = uint32(byte(instruction.Address>>16))<<16 | target&0xffff
			}
			exit, found := callee[Variant{target, instruction.M & 1, instruction.X & 1}]
			if !found || exit.M < 0 || exit.X < 0 {
				return nil, false
			}
			seen[[2]int8{exit.M & 1, exit.X & 1}] = struct{}{}
		}
	}
	if len(seen) == 0 {
		return nil, false
	}
	result := make([]MX, 0, len(seen))
	for pair := range seen {
		result = append(result, MX{pair[0], pair[1]})
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].M != result[j].M {
			return result[i].M < result[j].M
		}
		return result[i].X < result[j].X
	})
	return result, true
}

func AnalyzeExitMX(graph *Graph, callee map[Variant]MX) MX {
	modes, ok := AnalyzeExitModes(graph, callee)
	if !ok {
		return MX{-1, -1}
	}
	result := modes[0]
	for _, mode := range modes[1:] {
		if mode.M != result.M {
			result.M = -1
		}
		if mode.X != result.X {
			result.X = -1
		}
	}
	return result
}

func isDispatchTerminator(decoded *DecodedInstruction) bool {
	instruction := decoded.Instruction
	return instruction.DispatchEntries != nil && len(decoded.Successors) == 0 && (instruction.Mnemonic == "JSL" || instruction.Mnemonic == "JMP")
}

func applyConstantZFold(graph *Graph) {
	predecessors := map[DecodeKey]map[DecodeKey]struct{}{}
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		for _, successor := range decoded.Successors {
			if predecessors[successor] == nil {
				predecessors[successor] = map[DecodeKey]struct{}{}
			}
			predecessors[successor][key] = struct{}{}
		}
	}
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		instruction := decoded.Instruction
		if instruction.Mnemonic != "BEQ" && instruction.Mnemonic != "BNE" {
			continue
		}
		preds := predecessors[key]
		if len(preds) != 1 || len(decoded.Successors) != 2 {
			continue
		}
		var predecessorKey DecodeKey
		for candidate := range preds {
			predecessorKey = candidate
		}
		predecessor := graph.Instructions[predecessorKey]
		if predecessor == nil || len(predecessor.Successors) != 1 || predecessor.Successors[0] != key {
			continue
		}
		previous := predecessor.Instruction
		if (previous.Mnemonic != "LDA" && previous.Mnemonic != "LDX" && previous.Mnemonic != "LDY") || previous.Mode != cpu65816.IMM {
			continue
		}
		width := 16
		if (previous.Mnemonic == "LDA" && previous.M == 1) || (previous.Mnemonic != "LDA" && previous.X == 1) {
			width = 8
		}
		mask := uint32(1<<width) - 1
		immediate := previous.Operand & mask
		z := immediate == 0
		taken := (instruction.Mnemonic == "BEQ" && z) || (instruction.Mnemonic == "BNE" && !z)
		live, dead, kind := decoded.Successors[0], decoded.Successors[1], "fall"
		if taken {
			live, dead, kind = decoded.Successors[1], decoded.Successors[0], "jump"
		}
		decoded.Successors = []DecodeKey{live}
		instruction.ConstantZFold = true
		deadPC := dead.PC & 0xffffff
		instruction.ConstantZDeadPC = &deadPC
		zValue := 0
		if z {
			zValue = 1
		}
		graph.ConstantZFolds = append(graph.ConstantZFolds, ConstantZFold{
			BranchPC: instruction.Address & 0xffffff, PreviousPC: previous.Address & 0xffffff,
			BranchMnemonic: instruction.Mnemonic, PreviousMnemonic: previous.Mnemonic,
			PreviousImmediate: immediate, WidthBits: width, ZValue: zValue, TakenKind: kind,
			LivePC: live.PC & 0xffffff, DeadPC: dead.PC & 0xffffff, FunctionEntry: graph.Entry.PC & 0xffffff,
			EntryM: graph.Entry.M & 1, EntryX: graph.Entry.X & 1,
		})
	}
	reachable := map[DecodeKey]struct{}{}
	stack := []DecodeKey{graph.Entry}
	for len(stack) > 0 {
		key := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		if _, seen := reachable[key]; seen {
			continue
		}
		decoded := graph.Instructions[key]
		if decoded == nil {
			continue
		}
		reachable[key] = struct{}{}
		stack = append(stack, decoded.Successors...)
	}
	keptOrder := make([]DecodeKey, 0, len(graph.Order))
	for _, key := range graph.Order {
		if _, found := reachable[key]; !found {
			delete(graph.Instructions, key)
			continue
		}
		keptOrder = append(keptOrder, key)
	}
	graph.Order = keptOrder
}
