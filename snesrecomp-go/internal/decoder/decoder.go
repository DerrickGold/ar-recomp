package decoder

import (
	"fmt"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

type workItem struct {
	Key         DecodeKey
	Kind        string
	Predecessor int
}

// DecodeFunction performs worklist-driven decoding keyed by PC/M/X/PHP state.
func DecodeFunction(image rom.Image, bank byte, start uint16, entryM, entryX uint8, options Options) (*Graph, error) {
	if options.MaxInstructions <= 0 {
		options.MaxInstructions = 12000
	}
	entry := DecodeKey{PC: Address24(bank, start), M: entryM & 1, X: entryX & 1}
	graph := &Graph{Entry: entry, Instructions: make(map[DecodeKey]*DecodedInstruction)}
	worklist := []workItem{{entry, "entry", -1}}
	for len(worklist) > 0 {
		if len(graph.Instructions) >= options.MaxInstructions {
			return nil, fmt.Errorf("v2 decoder exceeded max_insns=%d at function $%06X", options.MaxInstructions, Address24(bank, start))
		}
		item := worklist[len(worklist)-1]
		worklist = worklist[:len(worklist)-1]
		if _, decoded := graph.Instructions[item.Key]; decoded {
			continue
		}
		pc := uint16(item.Key.PC)
		if options.End != nil && pc >= *options.End && item.Kind == "fall" && item.Predecessor >= 0 && item.Predecessor < int(*options.End) {
			continue
		}
		if item.Kind == "jump" {
			if _, sibling := options.SiblingEntryPCs[pc]; sibling {
				continue
			}
		}
		if pc < 0x8000 {
			continue
		}
		offset, err := rom.LoROMOffset(bank, pc)
		if err != nil || offset >= len(image) {
			continue
		}
		instruction, err := cpu65816.Decode(image, offset, pc, bank, item.Key.M, item.Key.X)
		if err != nil {
			return nil, err
		}
		if instruction == nil {
			return nil, fmt.Errorf("v2 decoder: unknown opcode $%02X at $%02X:%04X entry_mx=(%d,%d)", image[offset], bank, pc, item.Key.M, item.Key.X)
		}
		instruction.M, instruction.X = item.Key.M, item.Key.X

		if handled, err := decodeDispatchHelper(image, bank, start, pc, item.Key, instruction, graph, options); err != nil {
			return nil, err
		} else if handled {
			continue
		}
		if handled, err := decodeIndirectJump(image, bank, start, pc, item.Key, instruction, graph, &worklist, options); err != nil {
			return nil, err
		} else if handled {
			continue
		}
		if handled, err := decodePHADispatch(image, bank, pc, item.Key, instruction, graph, &worklist, options); err != nil {
			return nil, err
		} else if handled {
			continue
		}
		if handled, err := decodeIndirectJSR(image, bank, start, pc, item.Key, instruction, graph, &worklist, options); err != nil {
			return nil, err
		} else if handled {
			continue
		}
		if handled := decodeRTSTrick(bank, pc, item.Key, instruction, graph, &worklist, options); handled {
			continue
		}

		successors := labeledSuccessors(image, instruction, item.Key, bank, options)
		keys := make([]DecodeKey, len(successors))
		for index, successor := range successors {
			keys[index] = successor.Key
		}
		graph.record(&DecodedInstruction{Key: item.Key, Instruction: instruction, Successors: keys})
		for _, successor := range successors {
			if successor.Kind == "fall_brk" {
				if brkContinuationLooksValid(image, byte(successor.Key.PC>>16), uint16(successor.Key.PC), successor.Key.M, successor.Key.X) {
					if _, done := graph.Instructions[successor.Key]; !done {
						worklist = append(worklist, workItem{successor.Key, successor.Kind, int(pc)})
					}
				}
				continue
			}
			if _, done := graph.Instructions[successor.Key]; !done {
				worklist = append(worklist, workItem{successor.Key, successor.Kind, int(pc)})
			}
		}
	}

	decodedPCMX := map[[3]uint32]struct{}{}
	for key := range graph.Instructions {
		decodedPCMX[[3]uint32{key.PC, uint32(key.M), uint32(key.X)}] = struct{}{}
	}
	for key, decoded := range graph.Instructions {
		if decoded.Instruction.Mnemonic == "BRK" && len(decoded.Successors) > 0 {
			successor := decoded.Successors[0]
			if _, found := decodedPCMX[[3]uint32{successor.PC, uint32(successor.M), uint32(successor.X)}]; !found {
				decoded.Successors = nil
				graph.Instructions[key] = decoded
			}
		}
	}
	dedupeByPCMX(graph)
	applyConstantZFold(graph)
	return graph, nil
}

func decodeDispatchHelper(image rom.Image, bank byte, start, pc uint16, key DecodeKey, instruction *cpu65816.Instruction, graph *Graph, options Options) (bool, error) {
	isLongCall := instruction.Mnemonic == "JSL" || (instruction.Mnemonic == "JMP" && instruction.Length == 4)
	if !isLongCall || options.DispatchHelpers == nil {
		return false, nil
	}
	kind := options.DispatchHelpers[instruction.Operand&0xffffff]
	if kind == "" {
		return false, nil
	}
	entrySize := 2
	if kind == "long" {
		entrySize = 3
	}
	tablePC := pc + uint16(instruction.Length)
	var entries []uint32
	for len(entries) < 256 && uint32(tablePC)+uint32(entrySize)-1 <= 0xffff {
		offset, err := rom.LoROMOffset(bank, tablePC)
		if err != nil || offset+entrySize > len(image) {
			break
		}
		address := uint16(image[offset]) | uint16(image[offset+1])<<8
		if kind == "long" {
			targetBank := image[offset+2]
			if address == 0 && targetBank == 0 {
				entries = append(entries, 0)
				tablePC += uint16(entrySize)
				continue
			}
			if address < 0x8000 || (targetBank >= 0x40 && targetBank < 0x80) || targetIsPadding(image, targetBank, address) {
				break
			}
			if inDataRegion(options.DataRegions, targetBank, address) {
				graph.DispatchTargetsSuppressed = append(graph.DispatchTargetsSuppressed, DispatchTargetSuppressed{Address24(bank, pc), Address24(targetBank, address), "data_region", len(entries)})
				break
			}
			entries = append(entries, Address24(targetBank, address))
		} else {
			if address == 0 {
				entries = append(entries, 0)
				tablePC += uint16(entrySize)
				continue
			}
			if address < 0x8000 || targetIsPadding(image, bank, address) {
				break
			}
			if inDataRegion(options.DataRegions, bank, address) {
				graph.DispatchTargetsSuppressed = append(graph.DispatchTargetsSuppressed, DispatchTargetSuppressed{Address24(bank, pc), Address24(bank, address), "data_region", len(entries)})
				break
			}
			entries = append(entries, uint32(address))
		}
		tablePC += uint16(entrySize)
	}
	if len(entries) == 0 {
		return false, nil
	}
	instruction.DispatchEntries, instruction.DispatchKind = entries, kind
	graph.record(&DecodedInstruction{Key: key, Instruction: instruction})
	_ = start
	return true, nil
}

func decodeIndirectJump(image rom.Image, bank byte, start, pc uint16, key DecodeKey, instruction *cpu65816.Instruction, graph *Graph, worklist *[]workItem, options Options) (bool, error) {
	if instruction.Mnemonic != "JMP" || (instruction.Mode != cpu65816.INDIR && instruction.Mode != cpu65816.INDIRX) {
		return false, nil
	}
	site := Address24(bank, pc)
	auth, authorized := options.IndirectDispatch[site]
	if !authorized && instruction.Mode == cpu65816.INDIRX {
		if entries := autorecoverXTable(image, bank, instruction, options.DataRegions, start); len(entries) > 0 {
			auth = DispatchAuth{Count: len(entries), IndexReg: "X"}
			authorized = true
		}
	}
	if !authorized && instruction.Mode == cpu65816.INDIR && uint16(instruction.Operand) <= 0xff {
		bases, index := autorecoverDP(image, bank, start, pc, uint16(instruction.Operand), options.DataRegions)
		if len(bases) > 0 {
			if count := autorecoverDPCount(image, bank, bases, options.DataRegions); count > 0 {
				auth = DispatchAuth{Count: count, IndexReg: index, TableBases: bases}
				authorized = true
			}
		}
	}
	if !authorized && instruction.Mode == cpu65816.INDIR && uint16(instruction.Operand) >= 0x8000 {
		tablePC := uint16(instruction.Operand)
		offset, err := rom.LoROMOffset(bank, tablePC)
		size := 2
		if instruction.Length == 4 {
			size = 3
		}
		if err == nil && offset+size <= len(image) {
			target := uint16(image[offset]) | uint16(image[offset+1])<<8
			if target >= 0x8000 && !inDataRegion(options.DataRegions, bank, target) && !targetIsPadding(image, bank, target) {
				auth = DispatchAuth{Count: 1, IndexReg: "X", TableBases: []uint16{tablePC}}
				authorized = true
			}
		}
	}
	if authorized {
		entries, ok := resolveDispatch(image, bank, instruction, auth)
		if ok {
			instruction.DispatchEntries = entries
			instruction.DispatchKind = "short"
			if instruction.Length == 4 || len(auth.TableBases) == 3 {
				instruction.DispatchKind = "long"
			}
			instruction.DispatchIndexReg = auth.IndexReg
			instruction.DispatchTableBase = append([]uint16(nil), auth.TableBases...)
			successors := dispatchSuccessors(bank, entries, instruction.M, instruction.X)
			storeAndQueue(key, instruction, successors, graph, worklist, pc)
			return true, nil
		}
	}
	graph.record(&DecodedInstruction{Key: key, Instruction: instruction})
	if _, hle := options.HLEDispatch[pc]; !hle {
		graph.UnresolvedIndirects = append(graph.UnresolvedIndirects, UnresolvedIndirect{site, Address24(bank, start), instruction.Mnemonic, instruction.Mode, instruction.Operand & 0xffffff, key.M, key.X})
	}
	return true, nil
}

func decodePHADispatch(image rom.Image, bank byte, pc uint16, key DecodeKey, instruction *cpu65816.Instruction, graph *Graph, worklist *[]workItem, options Options) (bool, error) {
	if instruction.Mnemonic != "PHA" {
		return false, nil
	}
	auth, found := options.IndirectDispatch[Address24(bank, pc)]
	if !found || auth.RTSTrick {
		return false, nil
	}
	entries, ok := resolveDispatch(image, bank, instruction, auth)
	if !ok {
		return false, nil
	}
	for index, entry := range entries {
		target := uint16(entry + 1)
		if entry == 0 || target < 0x8000 {
			entries[index] = 0
		} else {
			entries[index] = entry&0xff0000 | uint32(target)
		}
	}
	instruction.DispatchEntries = entries
	instruction.DispatchKind = "short"
	if len(auth.TableBases) == 3 {
		instruction.DispatchKind = "long"
	}
	instruction.DispatchIndexReg = auth.IndexReg
	instruction.DispatchTableBase = append([]uint16(nil), auth.TableBases...)
	instruction.DispatchSEP = auth.SEPMask
	instruction.DispatchReturn = auth.ReturnPC
	instruction.DispatchTerminal = auth.ReturnPC == nil
	m, x := instruction.M&1, instruction.X&1
	if instruction.DispatchTerminal || auth.SEPMask&0x20 != 0 {
		m = 1
	}
	if instruction.DispatchTerminal || auth.SEPMask&0x10 != 0 {
		x = 1
	}
	successors := dispatchSuccessors(bank, entries, m, x)
	if auth.ReturnPC != nil && *auth.ReturnPC >= 0x8000 {
		successors = append(successors, labeledSuccessor{DecodeKey{PC: Address24(bank, *auth.ReturnPC), M: m, X: x}, "jump"})
	}
	storeAndQueue(key, instruction, successors, graph, worklist, pc)
	return true, nil
}

func decodeIndirectJSR(image rom.Image, bank byte, start, pc uint16, key DecodeKey, instruction *cpu65816.Instruction, graph *Graph, worklist *[]workItem, options Options) (bool, error) {
	if instruction.Mnemonic != "JSR" || instruction.Mode != cpu65816.INDIRX {
		return false, nil
	}
	site := Address24(bank, pc)
	auth, authorized := options.IndirectDispatch[site]
	if !authorized {
		if entries := autorecoverXTable(image, bank, instruction, options.DataRegions, start); len(entries) > 0 {
			auth = DispatchAuth{Count: len(entries), IndexReg: "X"}
			authorized = true
		}
	}
	if authorized {
		if entries, ok := resolveDispatch(image, bank, instruction, auth); ok {
			instruction.DispatchEntries = entries
			instruction.DispatchKind = "short"
			if instruction.Length == 4 || len(auth.TableBases) == 3 {
				instruction.DispatchKind = "long"
			}
			instruction.DispatchIndexReg = auth.IndexReg
			instruction.DispatchTableBase = append([]uint16(nil), auth.TableBases...)
			successors := labeledSuccessors(image, instruction, key, bank, options)
			successors = append(successors, dispatchSuccessors(bank, entries, instruction.M, instruction.X)...)
			storeAndQueue(key, instruction, successors, graph, worklist, pc)
			return true, nil
		}
	}
	if legacy, found := options.IndirectCallTables[site]; found {
		entrySize := 2
		if legacy.Kind == "long" {
			entrySize = 3
		}
		tablePC := legacy.Base
		var entries []uint32
		for index := 0; index < legacy.Count; index++ {
			if uint32(tablePC)+uint32(entrySize)-1 > 0xffff {
				break
			}
			offset, err := rom.LoROMOffset(bank, tablePC)
			if err != nil || offset+entrySize > len(image) {
				break
			}
			address := uint32(image[offset]) | uint32(image[offset+1])<<8
			if entrySize == 3 {
				address |= uint32(image[offset+2]) << 16
			} else {
				address |= uint32(bank) << 16
			}
			entries = append(entries, address)
			tablePC += uint16(entrySize)
		}
		instruction.DispatchEntries, instruction.DispatchKind = entries, legacy.Kind
		successors := labeledSuccessors(image, instruction, key, bank, options)
		successors = append(successors, dispatchSuccessors(bank, entries, instruction.M, instruction.X)...)
		storeAndQueue(key, instruction, successors, graph, worklist, pc)
		return true, nil
	}
	graph.record(&DecodedInstruction{Key: key, Instruction: instruction})
	graph.SuppressedIndirectCalls = append(graph.SuppressedIndirectCalls, SuppressedIndirectCall{site, Address24(bank, start), uint16(instruction.Operand), key.M, key.X})
	return true, nil
}

func decodeRTSTrick(bank byte, pc uint16, key DecodeKey, instruction *cpu65816.Instruction, graph *Graph, worklist *[]workItem, options Options) bool {
	if instruction.Mnemonic != "RTS" && instruction.Mnemonic != "RTL" {
		return false
	}
	auth, found := options.IndirectDispatch[Address24(bank, pc)]
	if !found || !auth.RTSTrick {
		return false
	}
	instruction.DispatchKind = "rts_trick"
	instruction.DispatchTerminal = true
	var successors []labeledSuccessor
	for _, target := range auth.Targets {
		instruction.DispatchEntries = append(instruction.DispatchEntries, Address24(bank, target))
		if target >= 0x8000 {
			successors = append(successors, labeledSuccessor{DecodeKey{PC: Address24(bank, target), M: key.M, X: key.X}, "jump"})
		}
	}
	storeAndQueue(key, instruction, successors, graph, worklist, pc)
	return true
}

func dispatchSuccessors(bank byte, entries []uint32, m, x uint8) []labeledSuccessor {
	var successors []labeledSuccessor
	for _, entry := range entries {
		if entry == 0 {
			continue
		}
		targetBank := byte(entry >> 16)
		target := uint16(entry)
		if targetBank == bank && target >= 0x8000 {
			successors = append(successors, labeledSuccessor{DecodeKey{PC: Address24(bank, target), M: m & 1, X: x & 1}, "jump"})
		}
	}
	return successors
}

func storeAndQueue(key DecodeKey, instruction *cpu65816.Instruction, successors []labeledSuccessor, graph *Graph, worklist *[]workItem, pc uint16) {
	keys := make([]DecodeKey, len(successors))
	for index, successor := range successors {
		keys[index] = successor.Key
	}
	graph.record(&DecodedInstruction{Key: key, Instruction: instruction, Successors: keys})
	for _, successor := range successors {
		if _, done := graph.Instructions[successor.Key]; !done {
			*worklist = append(*worklist, workItem{successor.Key, successor.Kind, int(pc)})
		}
	}
}

func dedupeByPCMX(graph *Graph) {
	canonical := map[[3]uint32]DecodeKey{}
	remap := map[DecodeKey]DecodeKey{}
	for _, key := range graph.Order {
		group := [3]uint32{key.PC, uint32(key.M), uint32(key.X)}
		chosen, found := canonical[group]
		if !found {
			chosen = key
			canonical[group] = key
		}
		remap[key] = chosen
	}
	merged := map[DecodeKey]*DecodedInstruction{}
	mergedOrder := make([]DecodeKey, 0, len(graph.Order))
	seen := map[DecodeKey]map[DecodeKey]struct{}{}
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		canonicalKey := remap[key]
		if merged[canonicalKey] == nil {
			mergedOrder = append(mergedOrder, canonicalKey)
			successors := make([]DecodeKey, len(decoded.Successors))
			for index, successor := range decoded.Successors {
				if mapped, found := remap[successor]; found {
					successors[index] = mapped
				} else {
					successors[index] = successor
				}
			}
			merged[canonicalKey] = &DecodedInstruction{Key: canonicalKey, Instruction: decoded.Instruction, Successors: successors}
			seen[canonicalKey] = map[DecodeKey]struct{}{}
			for _, successor := range successors {
				seen[canonicalKey][successor] = struct{}{}
			}
			continue
		}
		for _, successor := range decoded.Successors {
			if mapped, found := remap[successor]; found {
				successor = mapped
			}
			if _, found := seen[canonicalKey][successor]; !found {
				merged[canonicalKey].Successors = append(merged[canonicalKey].Successors, successor)
				seen[canonicalKey][successor] = struct{}{}
			}
		}
	}
	graph.Instructions = merged
	graph.Order = mergedOrder
	if mapped, found := remap[graph.Entry]; found {
		graph.Entry = mapped
	}
}
