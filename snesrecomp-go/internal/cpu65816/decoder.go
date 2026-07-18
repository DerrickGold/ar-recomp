// Package cpu65816 decodes individual WDC 65C816 instructions.
package cpu65816

import "fmt"

type AddressingMode uint8

const (
	IMP AddressingMode = iota
	ACC
	IMM
	DP
	DPX
	DPY
	ABS
	ABSX
	ABSY
	LONG
	LONGX
	REL
	REL16
	STK
	INDIR
	INDIRX
	INDIRY
	INDIRLY
	INDIRL
	INDIRDPX
	DPINDIR
	STKIY
)

var modeNames = [...]string{
	"imp", "acc", "imm", "dp", "dp,x", "dp,y", "abs", "abs,x",
	"abs,y", "long", "long,x", "rel", "rel16", "stk", "(abs)",
	"(abs,x)", "(dp),y", "[dp],y", "[dp]", "(dp,x)", "(dp)", "(stk,S),Y",
}

func (mode AddressingMode) String() string {
	if int(mode) >= len(modeNames) {
		return fmt.Sprintf("mode(%d)", mode)
	}
	return modeNames[mode]
}

type Instruction struct {
	Address  uint32
	Opcode   byte
	Mnemonic string
	Mode     AddressingMode
	Operand  uint32
	Length   uint8

	DispatchEntries   []uint32
	DispatchKind      string
	DispatchIndexReg  string
	DispatchTableBase []uint16
	M                 uint8
	X                 uint8
	DispatchTerminal  bool
	DispatchReturn    *uint16
	DispatchSEP       byte
	ConstantZFold     bool
	ConstantZDeadPC   *uint32
}

func (instruction Instruction) String() string {
	bank := byte(instruction.Address >> 16)
	pc := uint16(instruction.Address)
	return fmt.Sprintf("$%02X:%04X [M=%d X=%d] %-5s %s", bank, pc, instruction.M, instruction.X, instruction.Mnemonic, instruction.FormatOperand())
}

func (instruction Instruction) FormatOperand() string {
	value := instruction.Operand
	switch instruction.Mode {
	case IMP:
		return ""
	case ACC:
		return "A"
	case IMM:
		if instruction.Mnemonic == "REP" || instruction.Mnemonic == "SEP" || value <= 0xff {
			return fmt.Sprintf("#$%02X", value)
		}
		return fmt.Sprintf("#$%04X", value)
	case DP:
		return fmt.Sprintf("$%02X", value)
	case DPX:
		return fmt.Sprintf("$%02X,X", value)
	case DPY:
		return fmt.Sprintf("$%02X,Y", value)
	case ABS:
		return fmt.Sprintf("$%04X", value)
	case ABSX:
		return fmt.Sprintf("$%04X,X", value)
	case ABSY:
		return fmt.Sprintf("$%04X,Y", value)
	case LONG:
		return fmt.Sprintf("$%06X", value)
	case LONGX:
		return fmt.Sprintf("$%06X,X", value)
	case REL, REL16:
		return fmt.Sprintf("$%04X", value)
	case STK:
		return fmt.Sprintf("$%02X,S", value)
	case INDIR:
		return fmt.Sprintf("($%04X)", value)
	case INDIRX:
		return fmt.Sprintf("($%04X,X)", value)
	case INDIRY:
		return fmt.Sprintf("($%02X),Y", value)
	case INDIRLY:
		return fmt.Sprintf("[$%02X],Y", value)
	case INDIRL:
		return fmt.Sprintf("[$%02X]", value)
	case INDIRDPX:
		return fmt.Sprintf("($%02X,X)", value)
	case DPINDIR:
		return fmt.Sprintf("($%02X)", value)
	case STKIY:
		// Preserve the Python formatter's historical spelling.
		return fmt.Sprintf("$%02X,S),Y", value)
	default:
		return fmt.Sprintf("$%X", value)
	}
}

type lengthKind uint8

const (
	lengthFixed lengthKind = iota
	lengthM
	lengthX
)

type spec struct {
	mnemonic string
	mode     AddressingMode
	length   uint8
	kind     lengthKind
}

func (s spec) instructionLength(m, x uint8) uint8 {
	switch s.kind {
	case lengthM:
		if m&1 != 0 {
			return 2
		}
		return 3
	case lengthX:
		if x&1 != 0 {
			return 2
		}
		return 3
	default:
		return s.length
	}
}

var opcodeTable = buildOpcodeTable()

func buildOpcodeTable() [256]spec {
	var table [256]spec
	set := func(opcode byte, mnemonic string, mode AddressingMode, length uint8) {
		// Python keeps the first fixed entry when a duplicate is present.
		if table[opcode].mnemonic == "" {
			table[opcode] = spec{mnemonic: mnemonic, mode: mode, length: length}
		}
	}

	set(0xAA, "TAX", IMP, 1)
	set(0x8A, "TXA", IMP, 1)
	set(0xA8, "TAY", IMP, 1)
	set(0x98, "TYA", IMP, 1)
	set(0x9B, "TXY", IMP, 1)
	set(0xBB, "TYX", IMP, 1)
	set(0xBA, "TSX", IMP, 1)
	set(0x9A, "TXS", IMP, 1)
	set(0x5B, "TCD", IMP, 1)
	set(0x7B, "TDC", IMP, 1)
	set(0x1B, "TCS", IMP, 1)
	set(0x3B, "TSC", IMP, 1)
	set(0xDA, "PHX", IMP, 1)
	set(0xFA, "PLX", IMP, 1)
	set(0x5A, "PHY", IMP, 1)
	set(0x7A, "PLY", IMP, 1)
	set(0x48, "PHA", IMP, 1)
	set(0x68, "PLA", IMP, 1)
	set(0x08, "PHP", IMP, 1)
	set(0x28, "PLP", IMP, 1)
	set(0x8B, "PHB", IMP, 1)
	set(0xAB, "PLB", IMP, 1)
	set(0x0B, "PHD", IMP, 1)
	set(0x2B, "PLD", IMP, 1)
	set(0x4B, "PHK", IMP, 1)
	set(0xE8, "INX", IMP, 1)
	set(0xC8, "INY", IMP, 1)
	set(0xCA, "DEX", IMP, 1)
	set(0x88, "DEY", IMP, 1)
	set(0x1A, "INC", ACC, 1)
	set(0x3A, "DEC", ACC, 1)
	set(0x18, "CLC", IMP, 1)
	set(0x38, "SEC", IMP, 1)
	set(0x58, "CLI", IMP, 1)
	set(0x78, "SEI", IMP, 1)
	set(0xD8, "CLD", IMP, 1)
	set(0xF8, "SED", IMP, 1)
	set(0xB8, "CLV", IMP, 1)
	set(0xFB, "XCE", IMP, 1)
	set(0xEB, "XBA", IMP, 1)
	set(0x0A, "ASL", ACC, 1)
	set(0x4A, "LSR", ACC, 1)
	set(0x2A, "ROL", ACC, 1)
	set(0x6A, "ROR", ACC, 1)
	set(0x60, "RTS", IMP, 1)
	set(0x6B, "RTL", IMP, 1)
	set(0x40, "RTI", IMP, 1)
	set(0xEA, "NOP", IMP, 1)
	set(0xDB, "STP", IMP, 1)
	set(0xCB, "WAI", IMP, 1)

	set(0x64, "STZ", DP, 2)
	set(0x74, "STZ", DPX, 2)
	set(0xA5, "LDA", DP, 2)
	set(0xB5, "LDA", DPX, 2)
	set(0xB2, "LDA", DPINDIR, 2)
	set(0xB1, "LDA", INDIRY, 2)
	set(0xA7, "LDA", INDIRL, 2)
	set(0xB7, "LDA", INDIRLY, 2)
	set(0x85, "STA", DP, 2)
	set(0x95, "STA", DPX, 2)
	set(0x92, "STA", DPINDIR, 2)
	set(0x91, "STA", INDIRY, 2)
	set(0x87, "STA", INDIRL, 2)
	set(0x97, "STA", INDIRLY, 2)
	set(0xA6, "LDX", DP, 2)
	set(0xB6, "LDX", DPY, 2)
	set(0xA4, "LDY", DP, 2)
	set(0xB4, "LDY", DPX, 2)
	set(0x86, "STX", DP, 2)
	set(0x96, "STX", DPY, 2)
	set(0x84, "STY", DP, 2)
	set(0x94, "STY", DPX, 2)
	set(0x25, "AND", DP, 2)
	set(0x35, "AND", DPX, 2)
	set(0x21, "AND", INDIRDPX, 2)
	set(0x27, "AND", INDIRL, 2)
	set(0x37, "AND", INDIRLY, 2)
	set(0x05, "ORA", DP, 2)
	set(0x15, "ORA", DPX, 2)
	set(0x01, "ORA", INDIRDPX, 2)
	set(0x07, "ORA", INDIRL, 2)
	set(0x17, "ORA", INDIRLY, 2)
	set(0x45, "EOR", DP, 2)
	set(0x55, "EOR", DPX, 2)
	set(0x41, "EOR", INDIRDPX, 2)
	set(0x47, "EOR", INDIRL, 2)
	set(0x57, "EOR", INDIRLY, 2)
	set(0x65, "ADC", DP, 2)
	set(0x75, "ADC", DPX, 2)
	set(0x61, "ADC", INDIRDPX, 2)
	set(0x67, "ADC", INDIRL, 2)
	set(0x77, "ADC", INDIRLY, 2)
	set(0xE5, "SBC", DP, 2)
	set(0xF5, "SBC", DPX, 2)
	set(0xE1, "SBC", INDIRDPX, 2)
	set(0xE7, "SBC", INDIRL, 2)
	set(0xF7, "SBC", INDIRLY, 2)
	set(0xC5, "CMP", DP, 2)
	set(0xD5, "CMP", DPX, 2)
	set(0xC1, "CMP", INDIRDPX, 2)
	set(0xC7, "CMP", INDIRL, 2)
	set(0xD7, "CMP", INDIRLY, 2)
	set(0xA1, "LDA", INDIRDPX, 2)
	set(0x81, "STA", INDIRDPX, 2)
	set(0x12, "ORA", DPINDIR, 2)
	set(0x32, "AND", DPINDIR, 2)
	set(0x52, "EOR", DPINDIR, 2)
	set(0x72, "ADC", DPINDIR, 2)
	set(0xD2, "CMP", DPINDIR, 2)
	set(0xF2, "SBC", DPINDIR, 2)
	set(0x11, "ORA", INDIRY, 2)
	set(0x31, "AND", INDIRY, 2)
	set(0x51, "EOR", INDIRY, 2)
	set(0x71, "ADC", INDIRY, 2)
	set(0xD1, "CMP", INDIRY, 2)
	set(0xF1, "SBC", INDIRY, 2)
	set(0x93, "STA", STKIY, 2)
	set(0x13, "ORA", STKIY, 2)
	set(0x33, "AND", STKIY, 2)
	set(0x53, "EOR", STKIY, 2)
	set(0x73, "ADC", STKIY, 2)
	set(0xB3, "LDA", STKIY, 2)
	set(0xD3, "CMP", STKIY, 2)
	set(0xF3, "SBC", STKIY, 2)
	set(0x82, "BRL", REL16, 3)
	set(0xC6, "DEC", DP, 2)
	set(0xD6, "DEC", DPX, 2)
	set(0xE6, "INC", DP, 2)
	set(0xF6, "INC", DPX, 2)
	set(0x26, "ROL", DP, 2)
	set(0x36, "ROL", DPX, 2)
	set(0x66, "ROR", DP, 2)
	set(0x76, "ROR", DPX, 2)
	set(0x06, "ASL", DP, 2)
	set(0x16, "ASL", DPX, 2)
	set(0x46, "LSR", DP, 2)
	set(0x56, "LSR", DPX, 2)
	set(0x24, "BIT", DP, 2)
	set(0x34, "BIT", DPX, 2)
	set(0x04, "TSB", DP, 2)
	set(0x14, "TRB", DP, 2)
	set(0x03, "ORA", STK, 2)
	set(0x23, "AND", STK, 2)
	set(0x43, "EOR", STK, 2)
	set(0x63, "ADC", STK, 2)
	set(0x83, "STA", STK, 2)
	set(0xA3, "LDA", STK, 2)
	set(0xC3, "CMP", STK, 2)
	set(0xE3, "SBC", STK, 2)
	set(0xD4, "PEI", DP, 2)
	set(0xC2, "REP", IMM, 2)
	set(0xE2, "SEP", IMM, 2)
	set(0x00, "BRK", IMM, 2)
	set(0x02, "COP", IMM, 2)
	set(0x42, "WDM", IMM, 2)
	set(0x10, "BPL", REL, 2)
	set(0x30, "BMI", REL, 2)
	set(0xF0, "BEQ", REL, 2)
	set(0xD0, "BNE", REL, 2)
	set(0x90, "BCC", REL, 2)
	set(0xB0, "BCS", REL, 2)
	set(0x50, "BVC", REL, 2)
	set(0x70, "BVS", REL, 2)
	set(0x80, "BRA", REL, 2)

	set(0x9C, "STZ", ABS, 3)
	set(0x9E, "STZ", ABSX, 3)
	set(0xAD, "LDA", ABS, 3)
	set(0xBD, "LDA", ABSX, 3)
	set(0xB9, "LDA", ABSY, 3)
	set(0x8D, "STA", ABS, 3)
	set(0x9D, "STA", ABSX, 3)
	set(0x99, "STA", ABSY, 3)
	set(0xAE, "LDX", ABS, 3)
	set(0xBE, "LDX", ABSY, 3)
	set(0xAC, "LDY", ABS, 3)
	set(0xBC, "LDY", ABSX, 3)
	set(0x8E, "STX", ABS, 3)
	set(0x8C, "STY", ABS, 3)
	set(0xEC, "CPX", ABS, 3)
	set(0xE4, "CPX", DP, 2)
	set(0xCC, "CPY", ABS, 3)
	set(0xC4, "CPY", DP, 2)
	set(0x2D, "AND", ABS, 3)
	set(0x3D, "AND", ABSX, 3)
	set(0x39, "AND", ABSY, 3)
	set(0x0D, "ORA", ABS, 3)
	set(0x1D, "ORA", ABSX, 3)
	set(0x19, "ORA", ABSY, 3)
	set(0x4D, "EOR", ABS, 3)
	set(0x5D, "EOR", ABSX, 3)
	set(0x59, "EOR", ABSY, 3)
	set(0x6D, "ADC", ABS, 3)
	set(0x7D, "ADC", ABSX, 3)
	set(0x79, "ADC", ABSY, 3)
	set(0xED, "SBC", ABS, 3)
	set(0xFD, "SBC", ABSX, 3)
	set(0xF9, "SBC", ABSY, 3)
	set(0xCD, "CMP", ABS, 3)
	set(0xDD, "CMP", ABSX, 3)
	set(0xD9, "CMP", ABSY, 3)
	set(0xCE, "DEC", ABS, 3)
	set(0xDE, "DEC", ABSX, 3)
	set(0xEE, "INC", ABS, 3)
	set(0xFE, "INC", ABSX, 3)
	set(0x2E, "ROL", ABS, 3)
	set(0x3E, "ROL", ABSX, 3)
	set(0x6E, "ROR", ABS, 3)
	set(0x7E, "ROR", ABSX, 3)
	set(0x0E, "ASL", ABS, 3)
	set(0x1E, "ASL", ABSX, 3)
	set(0x4E, "LSR", ABS, 3)
	set(0x5E, "LSR", ABSX, 3)
	set(0x2C, "BIT", ABS, 3)
	set(0x3C, "BIT", ABSX, 3)
	set(0x0C, "TSB", ABS, 3)
	set(0x1C, "TRB", ABS, 3)
	set(0x4C, "JMP", ABS, 3)
	set(0x6C, "JMP", INDIR, 3)
	set(0x7C, "JMP", INDIRX, 3)
	set(0xDC, "JMP", INDIR, 3)
	set(0x20, "JSR", ABS, 3)
	set(0xFC, "JSR", INDIRX, 3)
	set(0xF4, "PEA", ABS, 3)
	set(0x62, "PER", REL16, 3)
	set(0x44, "MVP", IMM, 3)
	set(0x54, "MVN", IMM, 3)

	set(0xAF, "LDA", LONG, 4)
	set(0xBF, "LDA", LONGX, 4)
	set(0x8F, "STA", LONG, 4)
	set(0x9F, "STA", LONGX, 4)
	set(0x0F, "ORA", LONG, 4)
	set(0x1F, "ORA", LONGX, 4)
	set(0x2F, "AND", LONG, 4)
	set(0x3F, "AND", LONGX, 4)
	set(0x4F, "EOR", LONG, 4)
	set(0x5F, "EOR", LONGX, 4)
	set(0x6F, "ADC", LONG, 4)
	set(0x7F, "ADC", LONGX, 4)
	set(0xCF, "CMP", LONG, 4)
	set(0xDF, "CMP", LONGX, 4)
	set(0xEF, "SBC", LONG, 4)
	set(0xFF, "SBC", LONGX, 4)
	set(0x5C, "JMP", LONG, 4)
	set(0x22, "JSL", LONG, 4)

	for _, dynamic := range []struct {
		opcode   byte
		mnemonic string
		kind     lengthKind
	}{
		{0xA9, "LDA", lengthM}, {0x09, "ORA", lengthM}, {0x29, "AND", lengthM}, {0x49, "EOR", lengthM},
		{0x69, "ADC", lengthM}, {0xE9, "SBC", lengthM}, {0xC9, "CMP", lengthM}, {0x89, "BIT", lengthM},
		{0xA2, "LDX", lengthX}, {0xA0, "LDY", lengthX}, {0xE0, "CPX", lengthX}, {0xC0, "CPY", lengthX},
	} {
		table[dynamic.opcode] = spec{mnemonic: dynamic.mnemonic, mode: IMM, kind: dynamic.kind}
	}
	return table
}

// Decode decodes one instruction from data[offset].
func Decode(data []byte, offset int, pc uint16, bank byte, m, x uint8) (*Instruction, error) {
	if offset < 0 || offset >= len(data) {
		return nil, fmt.Errorf("decode offset %d outside %d-byte input", offset, len(data))
	}
	opcode := data[offset]
	specification := opcodeTable[opcode]
	if specification.mnemonic == "" {
		return nil, nil
	}
	length := specification.instructionLength(m, x)
	if int(length) > len(data)-offset {
		return nil, fmt.Errorf("truncated %s at $%02X:%04X: need %d bytes, have %d", specification.mnemonic, bank, pc, length, len(data)-offset)
	}
	byteAt := func(relative int) byte { return data[offset+relative] }
	word := func() uint32 { return uint32(byteAt(1)) | uint32(byteAt(2))<<8 }
	long := func() uint32 { return word() | uint32(byteAt(3))<<16 }
	var operand uint32
	switch specification.mode {
	case REL:
		delta := int8(byteAt(1))
		operand = uint32(uint16(int(pc) + 2 + int(delta)))
	case REL16:
		delta := int16(word())
		operand = uint32(uint16(int(pc) + 3 + int(delta)))
	case LONG, LONGX:
		operand = long()
	case ABS, ABSX, ABSY, INDIR, INDIRX:
		operand = word()
	case DP, DPX, DPY, STK, INDIRY, INDIRLY, INDIRL, INDIRDPX, DPINDIR, STKIY:
		operand = uint32(byteAt(1))
	case IMM:
		if length == 2 {
			operand = uint32(byteAt(1))
		} else {
			operand = word()
		}
	}
	return &Instruction{
		Address: uint32(bank)<<16 | uint32(pc), Opcode: opcode,
		Mnemonic: specification.mnemonic, Mode: specification.mode,
		Operand: operand, Length: length, M: 1, X: 1,
	}, nil
}

// ValidateDecodedInstructions applies the same conservative plausibility
// checks as the Python decoder.
func ValidateDecodedInstructions(instructions []Instruction, bank byte) bool {
	_ = bank
	for _, instruction := range instructions {
		if instruction.Mnemonic == "JSL" {
			targetBank := byte(instruction.Operand >> 16)
			if targetBank > 0x0d && targetBank != 0x7e && targetBank != 0x7f {
				return false
			}
		}
		if (instruction.Mode == LONG || instruction.Mode == LONGX) && instruction.Mnemonic != "JSL" {
			addressBank := byte(instruction.Operand >> 16)
			if addressBank > 0x0d && addressBank != 0x7e && addressBank != 0x7f {
				return false
			}
		}
		if instruction.Mnemonic == "JSR" && instruction.Operand < 0x0800 {
			return false
		}
		if instruction.Mnemonic == "BRK" || instruction.Mnemonic == "COP" {
			return false
		}
	}
	return true
}

// OpcodeCount is exposed for parity tests.
func OpcodeCount() int {
	count := 0
	for _, specification := range opcodeTable {
		if specification.mnemonic != "" {
			count++
		}
	}
	return count
}
