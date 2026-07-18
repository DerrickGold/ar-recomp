// Package lowering lowers decoded 65C816 instructions into v2 IR operations.
package lowering

import (
	"fmt"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/ir"
)

type ValueFactory func() ir.Value
type handler func(*cpu65816.Instruction, ValueFactory) []ir.Op

func segmentFor(instruction *cpu65816.Instruction) ir.SegRef {
	index := func(reg ir.Reg) *ir.Reg { return &reg }
	bank := func(value byte) *byte { return &value }
	switch instruction.Mode {
	case cpu65816.DP:
		return ir.SegRef{Kind: ir.Direct, Offset: instruction.Operand}
	case cpu65816.DPX:
		return ir.SegRef{Kind: ir.Direct, Offset: instruction.Operand, Index: index(ir.X)}
	case cpu65816.DPY:
		return ir.SegRef{Kind: ir.Direct, Offset: instruction.Operand, Index: index(ir.Y)}
	case cpu65816.ABS:
		return ir.SegRef{Kind: ir.AbsoluteBank, Offset: instruction.Operand}
	case cpu65816.ABSX:
		return ir.SegRef{Kind: ir.AbsoluteBank, Offset: instruction.Operand, Index: index(ir.X)}
	case cpu65816.ABSY:
		return ir.SegRef{Kind: ir.AbsoluteBank, Offset: instruction.Operand, Index: index(ir.Y)}
	case cpu65816.LONG:
		return ir.SegRef{Kind: ir.Long, Offset: instruction.Operand & 0xffff, Bank: bank(byte(instruction.Operand >> 16))}
	case cpu65816.LONGX:
		return ir.SegRef{Kind: ir.Long, Offset: instruction.Operand & 0xffff, Bank: bank(byte(instruction.Operand >> 16)), Index: index(ir.X)}
	case cpu65816.STK:
		return ir.SegRef{Kind: ir.Stack, Offset: instruction.Operand}
	case cpu65816.STKIY:
		return ir.SegRef{Kind: ir.StackRelativeIndirectY, Offset: instruction.Operand}
	case cpu65816.INDIR:
		return ir.SegRef{Kind: ir.AbsoluteIndirect, Offset: instruction.Operand}
	case cpu65816.INDIRX:
		return ir.SegRef{Kind: ir.AbsoluteIndirectX, Offset: instruction.Operand}
	case cpu65816.DPINDIR:
		return ir.SegRef{Kind: ir.DPIndirect, Offset: instruction.Operand}
	case cpu65816.INDIRY:
		return ir.SegRef{Kind: ir.DPIndirect, Offset: instruction.Operand, Index: index(ir.Y)}
	case cpu65816.INDIRDPX:
		return ir.SegRef{Kind: ir.DPIndirectX, Offset: instruction.Operand}
	case cpu65816.INDIRL:
		return ir.SegRef{Kind: ir.DPIndirectLong, Offset: instruction.Operand}
	case cpu65816.INDIRLY:
		return ir.SegRef{Kind: ir.DPIndirectLong, Offset: instruction.Operand, Index: index(ir.Y)}
	default:
		panic(fmt.Sprintf("addressing mode %s is not memory-referencing", instruction.Mode))
	}
}

func widthA(instruction *cpu65816.Instruction) int {
	if instruction.M&1 != 0 {
		return 1
	}
	return 2
}

func widthX(instruction *cpu65816.Instruction) int {
	if instruction.X&1 != 0 {
		return 1
	}
	return 2
}

func Lower(instruction *cpu65816.Instruction, factory ValueFactory) []ir.Op {
	if lower, ok := handlers[instruction.Mnemonic]; ok {
		return lower(instruction, factory)
	}
	return []ir.Op{ir.Nop{}}
}

func load(reg ir.Reg, width func(*cpu65816.Instruction) int) handler {
	return func(instruction *cpu65816.Instruction, factory ValueFactory) []ir.Op {
		w, value := width(instruction), factory()
		var first ir.Op
		if instruction.Mode == cpu65816.IMM {
			first = ir.ConstI{Value: instruction.Operand, Width: w, Out: value}
		} else {
			first = ir.Read{Seg: segmentFor(instruction), Width: w, Out: value}
		}
		return []ir.Op{first, ir.WriteReg{Reg: reg, Src: value}, ir.SetNZ{Src: value, Width: w}}
	}
}

func store(reg ir.Reg, width func(*cpu65816.Instruction) int) handler {
	return func(instruction *cpu65816.Instruction, factory ValueFactory) []ir.Op {
		value := factory()
		return []ir.Op{ir.ReadReg{Reg: reg, Out: value}, ir.Write{Seg: segmentFor(instruction), Src: value, Width: width(instruction)}}
	}
}

func alu(kind ir.AluKind, reg ir.Reg, width func(*cpu65816.Instruction) int) handler {
	return func(instruction *cpu65816.Instruction, factory ValueFactory) []ir.Op {
		w, rhs := width(instruction), factory()
		operations := make([]ir.Op, 0, 4)
		if instruction.Mode == cpu65816.IMM {
			operations = append(operations, ir.ConstI{Value: instruction.Operand, Width: w, Out: rhs})
		} else {
			operations = append(operations, ir.Read{Seg: segmentFor(instruction), Width: w, Out: rhs})
		}
		lhs := factory()
		operations = append(operations, ir.ReadReg{Reg: reg, Out: lhs})
		var output *ir.Value
		if kind != ir.Compare {
			value := factory()
			output = &value
		}
		operations = append(operations, ir.Alu{Kind: kind, LHS: lhs, RHS: rhs, Width: w, Out: output})
		if output != nil {
			operations = append(operations, ir.WriteReg{Reg: reg, Src: *output})
		}
		return operations
	}
}

func shift(kind ir.ShiftKind) handler {
	return func(instruction *cpu65816.Instruction, factory ValueFactory) []ir.Op {
		w, source, output := widthA(instruction), factory(), factory()
		if instruction.Mode == cpu65816.ACC {
			return []ir.Op{ir.ReadReg{Reg: ir.A, Out: source}, ir.Shift{Kind: kind, Src: source, Width: w, Out: output}, ir.WriteReg{Reg: ir.A, Src: output}}
		}
		segment := segmentFor(instruction)
		return []ir.Op{ir.Read{Seg: segment, Width: w, Out: source}, ir.Shift{Kind: kind, Src: source, Width: w, Out: output}, ir.Write{Seg: segment, Src: output, Width: w}}
	}
}

func transfer(source, destination ir.Reg) handler {
	return func(*cpu65816.Instruction, ValueFactory) []ir.Op {
		return []ir.Op{ir.Transfer{Source: source, Destination: destination}}
	}
}

func setFlag(flag ir.Reg, value int) handler {
	return func(*cpu65816.Instruction, ValueFactory) []ir.Op {
		return []ir.Op{ir.SetFlag{Flag: flag, Value: value}}
	}
}

func pushReg(reg ir.Reg) handler {
	return func(instruction *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		m, x := instruction.M, instruction.X
		return []ir.Op{ir.PushReg{Reg: reg, StaticM: &m, StaticX: &x}}
	}
}

func pullReg(reg ir.Reg) handler {
	return func(instruction *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		m, x := instruction.M, instruction.X
		return []ir.Op{ir.PullReg{Reg: reg, StaticM: &m, StaticX: &x}}
	}
}

func branch(flag ir.Reg, takeIf int) handler {
	return func(instruction *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		if instruction.ConstantZFold {
			return []ir.Op{ir.Goto{}}
		}
		return []ir.Op{ir.CondBranch{Flag: flag, TakeIf: takeIf}}
	}
}

var handlers = map[string]handler{
	"LDA": load(ir.A, widthA), "LDX": load(ir.X, widthX), "LDY": load(ir.Y, widthX),
	"STA": store(ir.A, widthA), "STX": store(ir.X, widthX), "STY": store(ir.Y, widthX),
	"ADC": alu(ir.Add, ir.A, widthA), "SBC": alu(ir.Sub, ir.A, widthA), "AND": alu(ir.And, ir.A, widthA),
	"ORA": alu(ir.Or, ir.A, widthA), "EOR": alu(ir.Xor, ir.A, widthA), "CMP": alu(ir.Compare, ir.A, widthA),
	"CPX": alu(ir.Compare, ir.X, widthX), "CPY": alu(ir.Compare, ir.Y, widthX),
	"ASL": shift(ir.ASL), "LSR": shift(ir.LSR), "ROL": shift(ir.ROL), "ROR": shift(ir.ROR),
	"TAX": transfer(ir.A, ir.X), "TXA": transfer(ir.X, ir.A), "TAY": transfer(ir.A, ir.Y), "TYA": transfer(ir.Y, ir.A),
	"TXY": transfer(ir.X, ir.Y), "TYX": transfer(ir.Y, ir.X), "TSX": transfer(ir.S, ir.X), "TXS": transfer(ir.X, ir.S),
	"TCD": transfer(ir.A, ir.D), "TDC": transfer(ir.D, ir.A), "TCS": transfer(ir.A, ir.S), "TSC": transfer(ir.S, ir.A),
	"CLC": setFlag(ir.C, 0), "SEC": setFlag(ir.C, 1), "CLI": setFlag(ir.I, 0), "SEI": setFlag(ir.I, 1),
	"CLD": setFlag(ir.DF, 0), "SED": setFlag(ir.DF, 1), "CLV": setFlag(ir.V, 0),
	"PHA": pushReg(ir.A), "PLA": pullReg(ir.A), "PHX": pushReg(ir.X), "PLX": pullReg(ir.X),
	"PHY": pushReg(ir.Y), "PLY": pullReg(ir.Y), "PHB": pushReg(ir.DB), "PLB": pullReg(ir.DB),
	"PHD": pushReg(ir.D), "PLD": pullReg(ir.D), "PHK": pushReg(ir.PB), "PHP": pushReg(ir.P), "PLP": pullReg(ir.P),
	"BPL": branch(ir.N, 0), "BMI": branch(ir.N, 1), "BVC": branch(ir.V, 0), "BVS": branch(ir.V, 1),
	"BCC": branch(ir.C, 0), "BCS": branch(ir.C, 1), "BNE": branch(ir.ZF, 0), "BEQ": branch(ir.ZF, 1),
}

func init() {
	handlers["STZ"] = func(instruction *cpu65816.Instruction, factory ValueFactory) []ir.Op {
		zero := factory()
		w := widthA(instruction)
		return []ir.Op{ir.ConstI{Value: 0, Width: w, Out: zero}, ir.Write{Seg: segmentFor(instruction), Src: zero, Width: w}}
	}
	handlers["INC"] = increment(1)
	handlers["DEC"] = increment(-1)
	handlers["INX"] = fixedIncrement(ir.X, 1)
	handlers["INY"] = fixedIncrement(ir.Y, 1)
	handlers["DEX"] = fixedIncrement(ir.X, -1)
	handlers["DEY"] = fixedIncrement(ir.Y, -1)
	handlers["BIT"] = func(instruction *cpu65816.Instruction, factory ValueFactory) []ir.Op {
		w, operand := widthA(instruction), factory()
		if instruction.Mode == cpu65816.IMM {
			return []ir.Op{ir.ConstI{Value: instruction.Operand, Width: w, Out: operand}, ir.BitTest{Operand: operand, Width: w, ZOnly: true}}
		}
		return []ir.Op{ir.Read{Seg: segmentFor(instruction), Width: w, Out: operand}, ir.BitTest{Operand: operand, Width: w}}
	}
	handlers["TSB"] = func(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		return []ir.Op{ir.BitSetMem{Seg: segmentFor(i), Width: widthA(i)}}
	}
	handlers["TRB"] = func(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		return []ir.Op{ir.BitClearMem{Seg: segmentFor(i), Width: widthA(i)}}
	}
	handlers["REP"] = func(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		return []ir.Op{ir.RepFlags{Mask: byte(i.Operand)}}
	}
	handlers["SEP"] = func(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		return []ir.Op{ir.SepFlags{Mask: byte(i.Operand)}}
	}
	handlers["XCE"] = singleton(ir.ExchangeCarryEmulation{})
	handlers["XBA"] = singleton(ir.ExchangeAccumulatorBytes{})
	handlers["BRA"] = singleton(ir.Goto{})
	handlers["BRL"] = singleton(ir.Goto{})
	handlers["JMP"] = lowerJump
	handlers["JSR"] = lowerJSR
	handlers["JSL"] = lowerJSL
	handlers["RTS"] = lowerReturn(false, false)
	handlers["RTL"] = lowerReturn(true, false)
	handlers["RTI"] = lowerReturn(true, true)
	handlers["NOP"] = singleton(ir.Nop{})
	handlers["WDM"] = singleton(ir.Nop{})
	handlers["BRK"] = singleton(ir.Break{COP: false})
	handlers["COP"] = singleton(ir.Break{COP: true})
	handlers["STP"] = singleton(ir.Stop{Wait: false})
	handlers["WAI"] = singleton(ir.Stop{Wait: true})
	handlers["PEA"] = pushEffective(ir.AbsoluteBank)
	handlers["PER"] = pushEffective(ir.AbsoluteBank)
	handlers["PEI"] = pushEffective(ir.DPIndirect)
	handlers["MVN"] = blockMove("mvn")
	handlers["MVP"] = blockMove("mvp")
}

func increment(delta int) handler {
	return func(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		if i.Mode == cpu65816.ACC {
			return []ir.Op{ir.IncReg{Reg: ir.A, Delta: delta}}
		}
		return []ir.Op{ir.IncMem{Seg: segmentFor(i), Width: widthA(i), Delta: delta}}
	}
}
func fixedIncrement(reg ir.Reg, delta int) handler {
	return func(*cpu65816.Instruction, ValueFactory) []ir.Op { return []ir.Op{ir.IncReg{Reg: reg, Delta: delta}} }
}
func singleton(op ir.Op) handler {
	return func(*cpu65816.Instruction, ValueFactory) []ir.Op { return []ir.Op{op} }
}
func lowerJump(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
	if i.Mode == cpu65816.INDIR || i.Mode == cpu65816.INDIRX {
		return []ir.Op{ir.IndirectGoto{Seg: segmentFor(i)}}
	}
	return []ir.Op{ir.Goto{}}
}
func lowerJSR(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
	source := i.Address & 0xffffff
	if i.Mode == cpu65816.INDIRX {
		base := uint16(i.Operand)
		return []ir.Op{ir.Call{Indirect: true, EntryM: i.M, EntryX: i.X, SourcePC: &source, TableBase: &base}}
	}
	target := uint32(byte(i.Address>>16))<<16 | (i.Operand & 0xffff)
	return []ir.Op{ir.Call{Target: &target, EntryM: i.M, EntryX: i.X, SourcePC: &source}}
}
func lowerJSL(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
	source, target := i.Address&0xffffff, i.Operand
	return []ir.Op{ir.Call{Target: &target, Long: true, EntryM: i.M, EntryX: i.X, SourcePC: &source}}
}
func lowerReturn(long, interrupt bool) handler {
	return func(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		source := i.Address & 0xffffff
		return []ir.Op{ir.Return{Long: long, Interrupt: interrupt, SourcePC: &source}}
	}
}
func pushEffective(kind ir.SegKind) handler {
	return func(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		return []ir.Op{ir.PushEffectiveAddress{Seg: ir.SegRef{Kind: kind, Offset: i.Operand}}}
	}
}
func blockMove(direction string) handler {
	return func(i *cpu65816.Instruction, _ ValueFactory) []ir.Op {
		return []ir.Op{ir.BlockMove{Direction: direction, DestinationBank: byte(i.Operand), SourceBank: byte(i.Operand >> 8)}}
	}
}

func KnownMnemonics() map[string]struct{} {
	result := make(map[string]struct{}, len(handlers))
	for mnemonic := range handlers {
		result[mnemonic] = struct{}{}
	}
	return result
}
