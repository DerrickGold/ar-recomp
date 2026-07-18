package decoder

import (
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func inDataRegion(regions []DataRegion, bank byte, pc uint16) bool {
	for _, region := range regions {
		if region.Bank == bank && pc >= region.Start && pc < region.End {
			return true
		}
	}
	return false
}

func targetIsPadding(image rom.Image, bank byte, pc uint16) bool {
	offset, err := rom.LoROMOffset(bank, pc)
	if err != nil || offset+16 > len(image) {
		return true
	}
	allZero, allFF := true, true
	for _, value := range image[offset : offset+16] {
		allZero = allZero && value == 0
		allFF = allFF && value == 0xff
	}
	return allZero || allFF
}

func brkContinuationLooksValid(image rom.Image, bank byte, pc uint16, m, x uint8) bool {
	for count := 0; count < 8; count++ {
		if pc < 0x8000 {
			return false
		}
		offset, err := rom.LoROMOffset(bank, pc)
		if err != nil || offset+3 >= len(image) {
			return false
		}
		instruction, err := cpu65816.Decode(image, offset, pc, bank, m, x)
		if err != nil || instruction == nil {
			return false
		}
		if instruction.Mnemonic == "JSL" {
			targetBank := byte(instruction.Operand >> 16)
			if targetBank > 0x0d && targetBank != 0x7e && targetBank != 0x7f {
				return false
			}
		}
		if (instruction.Mode == cpu65816.LONG || instruction.Mode == cpu65816.LONGX) && instruction.Mnemonic != "JSL" {
			addressBank := byte(instruction.Operand >> 16)
			if addressBank > 0x0d && addressBank != 0x7e && addressBank != 0x7f {
				return false
			}
		}
		if instruction.Mnemonic == "JSR" && instruction.Operand < 0x0800 {
			return false
		}
		switch instruction.Mnemonic {
		case "RTS", "RTL", "RTI", "JMP", "JML", "BRA", "BRL", "BRK":
			return true
		case "REP":
			if instruction.Operand&0x20 != 0 {
				m = 0
			}
			if instruction.Operand&0x10 != 0 {
				x = 0
			}
		case "SEP":
			if instruction.Operand&0x20 != 0 {
				m = 1
			}
			if instruction.Operand&0x10 != 0 {
				x = 1
			}
		}
		pc += uint16(instruction.Length)
	}
	return true
}

// ClassifyDispatchHelper recognizes the ExecutePtr-style helper idiom.
func ClassifyDispatchHelper(image rom.Image, bank byte, address uint16) string {
	var instructions []*cpu65816.Instruction
	pc, m, x := address, uint8(1), uint8(1)
	for safety := 0; safety < 256; safety++ {
		if pc < 0x8000 {
			return ""
		}
		offset, err := rom.LoROMOffset(bank, pc)
		if err != nil || offset >= len(image) {
			return ""
		}
		instruction, err := cpu65816.Decode(image, offset, pc, bank, m, x)
		if err != nil || instruction == nil {
			return ""
		}
		instructions = append(instructions, instruction)
		if instruction.Mnemonic == "REP" {
			if instruction.Operand&0x20 != 0 {
				m = 0
			}
			if instruction.Operand&0x10 != 0 {
				x = 0
			}
		}
		if instruction.Mnemonic == "SEP" {
			if instruction.Operand&0x20 != 0 {
				m = 1
			}
			if instruction.Operand&0x10 != 0 {
				x = 1
			}
		}
		switch instruction.Mnemonic {
		case "RTS", "RTL", "RTI", "BRA", "BRL", "JMP", "JML", "STP":
			safety = 256
			continue
		}
		pc += uint16(instruction.Length)
	}
	if len(instructions) == 0 {
		return ""
	}
	pulled := false
	for _, instruction := range instructions {
		if instruction.Mnemonic == "PLA" || instruction.Mnemonic == "PLY" {
			pulled = true
		}
	}
	last := instructions[len(instructions)-1]
	if !pulled || last.Mnemonic != "JMP" || (last.Mode != cpu65816.INDIR && last.Mode != cpu65816.INDIRX && last.Mode != cpu65816.INDIRL) {
		return ""
	}
	asl, adc := false, false
	for _, instruction := range instructions {
		if !asl {
			if instruction.Mnemonic == "ASL" && instruction.Mode == cpu65816.ACC {
				asl = true
			}
			continue
		}
		if instruction.Mnemonic == "ADC" {
			adc = true
		}
		if instruction.Mnemonic == "TAY" || instruction.Mnemonic == "TAX" {
			if adc {
				return "long"
			}
			return "short"
		}
	}
	return ""
}

func resolveDispatch(image rom.Image, bank byte, instruction *cpu65816.Instruction, auth DispatchAuth) ([]uint32, bool) {
	if auth.Count <= 0 {
		return nil, false
	}
	entries := make([]uint32, 0, auth.Count)
	if len(auth.TableBases) >= 2 {
		lo, hi := auth.TableBases[0], auth.TableBases[1]
		for index := 0; index < auth.Count; index++ {
			loOff, loErr := rom.LoROMOffset(bank, lo+uint16(index))
			hiOff, hiErr := rom.LoROMOffset(bank, hi+uint16(index))
			if loErr != nil || hiErr != nil || loOff >= len(image) || hiOff >= len(image) {
				return nil, false
			}
			address := uint32(image[loOff]) | uint32(image[hiOff])<<8
			if len(auth.TableBases) == 3 {
				bankOff, err := rom.LoROMOffset(bank, auth.TableBases[2]+uint16(index))
				if err != nil || bankOff >= len(image) {
					return nil, false
				}
				address |= uint32(image[bankOff]) << 16
			} else {
				address |= uint32(bank) << 16
			}
			entries = append(entries, address)
		}
		return entries, true
	}
	base := uint16(instruction.Operand)
	if len(auth.TableBases) == 1 {
		base = auth.TableBases[0]
	}
	entrySize := 2
	if instruction.Length == 4 {
		entrySize = 3
	}
	tablePC := base
	for index := 0; index < auth.Count; index++ {
		if uint32(tablePC)+uint32(entrySize)-1 > 0xffff {
			return nil, false
		}
		offset, err := rom.LoROMOffset(bank, tablePC)
		if err != nil || offset+entrySize > len(image) {
			return nil, false
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
	return entries, true
}

func autorecoverXTable(image rom.Image, bank byte, instruction *cpu65816.Instruction, regions []DataRegion, functionStart uint16) []uint32 {
	base := uint16(instruction.Operand)
	entrySize := 2
	if instruction.Length == 4 {
		entrySize = 3
	}
	tablePC := base
	codeBoundary := uint16(0)
	hasBoundary := base < functionStart
	if hasBoundary {
		codeBoundary = functionStart
	}
	var entries []uint32
	var handlers []uint16
	nulls := 0
	for len(entries) < 256 {
		if uint32(tablePC)+uint32(entrySize)-1 > 0xffff || (hasBoundary && tablePC >= codeBoundary) {
			break
		}
		overlaps := false
		for _, handler := range handlers {
			if tablePC >= handler {
				overlaps = true
				break
			}
		}
		if overlaps {
			break
		}
		offset, err := rom.LoROMOffset(bank, tablePC)
		if err != nil || offset+entrySize > len(image) {
			break
		}
		pc := uint16(image[offset]) | uint16(image[offset+1])<<8
		targetBank := bank
		if entrySize == 3 {
			targetBank = image[offset+2]
		}
		if pc == 0 && (entrySize == 2 || targetBank == 0) {
			nulls++
			if nulls >= 2 {
				if len(entries) > 0 && entries[len(entries)-1] == 0 {
					entries = entries[:len(entries)-1]
				}
				break
			}
			entries = append(entries, 0)
			tablePC += uint16(entrySize)
			continue
		}
		nulls = 0
		if pc < 0x8000 || inDataRegion(regions, targetBank, pc) || targetIsPadding(image, targetBank, pc) {
			break
		}
		entries = append(entries, uint32(targetBank)<<16|uint32(pc))
		if targetBank == bank && pc >= base {
			handlers = append(handlers, pc)
		}
		tablePC += uint16(entrySize)
	}
	return entries
}

func autorecoverDP(image rom.Image, bank byte, functionStart, sitePC, dpAddress uint16, regions []DataRegion) ([]uint16, string) {
	type winner struct {
		base  uint16
		index string
	}
	winners := map[int]winner{}
	pc := functionStart
	m, x := uint8(1), uint8(1)
	var candidate *winner
	for scanned := 0; pc < sitePC && scanned < 256; scanned++ {
		offset, err := rom.LoROMOffset(bank, pc)
		if err != nil || offset >= len(image) {
			return nil, ""
		}
		instruction, err := cpu65816.Decode(image, offset, pc, bank, m, x)
		if err != nil || instruction == nil {
			return nil, ""
		}
		if instruction.Mnemonic == "REP" {
			if instruction.Operand&0x20 != 0 {
				m = 0
			}
			if instruction.Operand&0x10 != 0 {
				x = 0
			}
		}
		if instruction.Mnemonic == "SEP" {
			if instruction.Operand&0x20 != 0 {
				m = 1
			}
			if instruction.Operand&0x10 != 0 {
				x = 1
			}
		}
		if instruction.Mnemonic == "LDA" && (instruction.Mode == cpu65816.ABSX || instruction.Mode == cpu65816.LONGX) {
			v := winner{uint16(instruction.Operand), "X"}
			candidate = &v
		} else if instruction.Mnemonic == "LDA" && instruction.Mode == cpu65816.ABSY {
			v := winner{uint16(instruction.Operand), "Y"}
			candidate = &v
		} else if instruction.Mnemonic == "STA" && instruction.Mode == cpu65816.DP {
			slot := int(uint16(instruction.Operand)) - int(dpAddress)
			if slot >= 0 && slot <= 2 && candidate != nil {
				winners[slot] = *candidate
				candidate = nil
			}
		} else if instruction.Mnemonic == "STA" || instruction.Mnemonic == "STZ" {
			slot := int(uint16(instruction.Operand)) - int(dpAddress)
			if slot >= 0 && slot <= 2 {
				delete(winners, slot)
			}
		} else if instruction.Mnemonic == "LDA" {
			candidate = nil
		}
		pc += uint16(instruction.Length)
	}
	if len(winners) == 0 {
		return nil, ""
	}
	index := ""
	for _, w := range winners {
		if index == "" {
			index = w.index
		} else if index != w.index {
			return nil, ""
		}
	}
	needed := 1
	if _, ok := winners[1]; ok {
		needed = 2
	}
	if _, ok := winners[2]; ok && m == 1 {
		needed = 3
	}
	bases := make([]uint16, needed)
	for i := 0; i < needed; i++ {
		w, ok := winners[i]
		if !ok {
			return nil, ""
		}
		bases[i] = w.base
	}
	return bases, index
}

func autorecoverDPCount(image rom.Image, bank byte, bases []uint16, regions []DataRegion) int {
	if len(bases) == 0 {
		return 0
	}
	count := 0
	for index := 0; index < 256; index++ {
		var pc uint16
		targetBank := bank
		if len(bases) == 1 {
			tablePC := uint32(bases[0]) + uint32(2*index)
			if tablePC+1 > 0xffff {
				break
			}
			offset, err := rom.LoROMOffset(bank, uint16(tablePC))
			if err != nil || offset+1 >= len(image) {
				break
			}
			pc = uint16(image[offset]) | uint16(image[offset+1])<<8
		} else {
			loOff, e1 := rom.LoROMOffset(bank, bases[0]+uint16(index))
			hiOff, e2 := rom.LoROMOffset(bank, bases[1]+uint16(index))
			if e1 != nil || e2 != nil || loOff >= len(image) || hiOff >= len(image) {
				break
			}
			pc = uint16(image[loOff]) | uint16(image[hiOff])<<8
			if len(bases) >= 3 {
				bankOff, e := rom.LoROMOffset(bank, bases[2]+uint16(index))
				if e != nil || bankOff >= len(image) {
					break
				}
				targetBank = image[bankOff]
			}
		}
		if pc < 0x8000 || inDataRegion(regions, targetBank, pc) || targetIsPadding(image, targetBank, pc) {
			break
		}
		count++
	}
	return count
}
