package codegen

import (
	"fmt"

	"github.com/DerrickGold/snesrecomp-go/internal/ir"
)

// Context owns all state that was module-global in Python codegen.py. Keeping
// it per emit job makes concurrent function emission race-free.
type Context struct {
	ROMSize           int
	Names             map[uint32]string
	ValidVariants     map[uint32]map[[2]uint8]struct{}
	CanonicalVariants map[uint32]map[[2]uint8]struct{}
	ProvenEquivalent  map[uint32]map[[2]uint8]map[[2]uint8]struct{}
	ForceVariantAt    map[uint32][2]uint8
	CurrentName       string
	CurrentSite       uint32
	CurrentExitM      *uint8
	CurrentExitX      *uint8
	Demands           map[Variant]struct{}
	Rejected          map[uint32]struct{}
}

type Variant struct {
	Address uint32
	M, X    uint8
}

func NewContext() *Context {
	return &Context{
		Names:             make(map[uint32]string),
		ValidVariants:     make(map[uint32]map[[2]uint8]struct{}),
		CanonicalVariants: make(map[uint32]map[[2]uint8]struct{}),
		ProvenEquivalent:  make(map[uint32]map[[2]uint8]map[[2]uint8]struct{}),
		ForceVariantAt:    make(map[uint32][2]uint8),
		Demands:           make(map[Variant]struct{}),
		Rejected:          make(map[uint32]struct{}),
	}
}

var allVariants = [][2]uint8{{0, 0}, {0, 1}, {1, 0}, {1, 1}}

func (context *Context) variantsAt(address uint32) [][2]uint8 {
	if context == nil || len(context.ValidVariants) == 0 {
		return allVariants
	}
	address &= 0xffffff
	valid := context.ValidVariants[address]
	if len(valid) == 0 {
		bank := byte(address >> 16)
		if bank < 0x40 || (bank >= 0x80 && bank < 0xc0) {
			valid = context.ValidVariants[address^0x800000]
		}
	}
	if len(valid) == 0 {
		return allVariants
	}
	result := make([][2]uint8, 0, len(valid))
	for _, pair := range allVariants {
		if _, found := valid[pair]; found {
			result = append(result, pair)
		}
	}
	return result
}

func nearestVariant(variants [][2]uint8, m, x uint8) [2]uint8 {
	best, bestCost := [2]uint8{}, 99
	for _, pair := range variants {
		cost := 0
		if pair[0] != m&1 {
			cost += 2
		}
		if pair[1] != x&1 {
			cost++
		}
		if cost < bestCost {
			best, bestCost = pair, cost
		}
	}
	return best
}

func (context *Context) routeVariant(address uint32, survivors [][2]uint8, m, x uint8) [2]uint8 {
	requested := [2]uint8{m & 1, x & 1}
	if byVariant := context.ProvenEquivalent[address&0xffffff]; byVariant != nil {
		for _, candidate := range allVariants {
			if _, proven := byVariant[requested][candidate]; !proven {
				continue
			}
			for _, survivor := range survivors {
				if candidate == survivor {
					return candidate
				}
			}
		}
	}
	canonical := context.CanonicalVariants[address&0xffffff]
	if len(canonical) == 0 {
		canonical = map[[2]uint8]struct{}{{1, 1}: {}}
	}
	for _, candidate := range allVariants {
		if _, isCanonical := canonical[candidate]; !isCanonical {
			continue
		}
		for _, survivor := range survivors {
			if candidate == survivor {
				return candidate
			}
		}
	}
	return nearestVariant(survivors, m, x)
}

// VariantDispatchCases emits the shared runtime-(M,X) switch policy used by
// direct calls and resolved dispatch tables.
func VariantDispatchCases(context *Context, address uint32, baseName, indent, preCall string) []string {
	survivors := context.variantsAt(address)
	valid := make(map[[2]uint8]struct{}, len(survivors))
	for _, pair := range survivors {
		valid[pair] = struct{}{}
	}
	var lines []string
	for _, pair := range allVariants {
		index := pair[0]<<1 | pair[1]
		target := pair
		comment := ""
		if _, found := valid[pair]; !found {
			target = context.routeVariant(address, survivors, pair[0], pair[1])
			comment = fmt.Sprintf("  /* M%dX%d pruned -> nearest survivor M%dX%d */", pair[0], pair[1], target[0], target[1])
		}
		lines = append(lines, fmt.Sprintf("%scase %d: %s_r = %s_M%dX%d(cpu); break;%s", indent, index, preCall, baseName, target[0], target[1], comment))
	}
	def := nearestVariant(survivors, 0, 0)
	return append(lines, fmt.Sprintf("%sdefault: %s_r = %s_M%dX%d(cpu); break;", indent, preCall, baseName, def[0], def[1]))
}

func EmitOperation(context *Context, operation ir.Op) ([]string, error) {
	if context == nil {
		context = NewContext()
	}
	switch op := operation.(type) {
	case ir.Read:
		bank, address, err := addressExpression(op.Seg)
		if err != nil {
			return nil, err
		}
		return []string{fmt.Sprintf("%s %s = %s(cpu, %s, %s);", ctype(op.Width), valueName(op.Out), readFunction(op.Width), bank, address)}, nil
	case ir.Write:
		bank, address, err := addressExpression(op.Seg)
		if err != nil {
			return nil, err
		}
		return []string{fmt.Sprintf("%s(cpu, %s, %s, %s);", writeFunction(op.Width), bank, address, valueName(op.Src))}, nil
	case ir.ReadReg:
		switch op.Reg {
		case ir.A:
			return []string{fmt.Sprintf("uint16 %s = cpu_read_a16(cpu);", valueName(op.Out))}, nil
		case ir.X:
			return []string{fmt.Sprintf("uint16 %s = cpu_read_x16(cpu);", valueName(op.Out))}, nil
		case ir.Y:
			return []string{fmt.Sprintf("uint16 %s = cpu_read_y16(cpu);", valueName(op.Out))}, nil
		default:
			return []string{fmt.Sprintf("uint16 %s = (uint16)%s;", valueName(op.Out), regName(op.Reg))}, nil
		}
	case ir.WriteReg:
		source := valueName(op.Src)
		switch op.Reg {
		case ir.A:
			return []string{fmt.Sprintf("cpu_write_a_m(cpu, (uint16)(%s));", source)}, nil
		case ir.X:
			return []string{fmt.Sprintf("cpu_write_x_x(cpu, (uint16)(%s));", source)}, nil
		case ir.Y:
			return []string{fmt.Sprintf("cpu_write_y_x(cpu, (uint16)(%s));", source)}, nil
		default:
			return []string{fmt.Sprintf("%s = %s;", regName(op.Reg), source)}, nil
		}
	case ir.ConstI:
		return []string{fmt.Sprintf("%s %s = 0x%x;", ctype(op.Width), valueName(op.Out), op.Value)}, nil
	case ir.Alu:
		return emitALU(op), nil
	case ir.Shift:
		return emitShift(op), nil
	case ir.IncReg:
		return emitIncReg(op), nil
	case ir.IncMem:
		return emitIncMem(op)
	case ir.BitTest:
		return emitBitTest(op), nil
	case ir.BitSetMem:
		return emitBitMemory(op.Seg, op.Width, false)
	case ir.BitClearMem:
		return emitBitMemory(op.Seg, op.Width, true)
	case ir.SetFlag:
		return emitSetFlag(op), nil
	case ir.SetNZ:
		return setNZ(masked(valueName(op.Src), op.Width), op.Width), nil
	case ir.RepFlags:
		return wrapModifyP(op.Mask, false), nil
	case ir.SepFlags:
		lines := wrapModifyP(op.Mask, true)
		if op.Mask&0x10 != 0 {
			lines = append(lines[:len(lines)-1], "  cpu->X = (uint16)(cpu->X & 0xFF);", "  cpu->Y = (uint16)(cpu->Y & 0xFF);", "}")
		}
		return lines, nil
	case ir.ExchangeCarryEmulation:
		return []string{
			"{", "  uint8 _old_p = cpu->P;", "  uint8 _t = cpu->emulation;",
			"  cpu->emulation = cpu->_flag_C;", "  cpu->_flag_C = _t;",
			"  if (cpu->emulation) { cpu->m_flag = 1; cpu->x_flag = 1; cpu->X = (uint16)(cpu->X & 0xFF); cpu->Y = (uint16)(cpu->Y & 0xFF); cpu->S = (uint16)(0x0100 | (cpu->S & 0xFF)); cpu_mirrors_to_p(cpu); }",
			"  cpu_trace_px_record(cpu, 0, 7 /*XCE*/, _old_p, cpu->P);", "}",
		}, nil
	case ir.ExchangeAccumulatorBytes:
		return []string{
			"{", "  uint16 _old = cpu->A;", "  cpu->A = (uint16)(((_old & 0xFF) << 8) | ((_old >> 8) & 0xFF));",
			"  cpu->_flag_Z = ((cpu->A & 0xFF) == 0) ? 1 : 0;", "  cpu->_flag_N = ((cpu->A & 0x80) != 0) ? 1 : 0;", "}",
		}, nil
	case ir.PushReg:
		return emitPushReg(op), nil
	case ir.PullReg:
		return emitPullReg(op), nil
	case ir.Push:
		if op.Width == 1 {
			return tracedStack("CPU_STACK_OP_PHA", -1, pushByte("(uint8)"+valueName(op.Src))), nil
		}
		return tracedStack("CPU_STACK_OP_PHA", -2, pushWord(valueName(op.Src))), nil
	case ir.Pull:
		if op.Width == 1 {
			return tracedStack("CPU_STACK_OP_PLA", 1, popByte("uint8 "+valueName(op.Out))), nil
		}
		return tracedStack("CPU_STACK_OP_PLA", 2, popWord("uint16 "+valueName(op.Out))), nil
	case ir.Transfer:
		return emitTransfer(op), nil
	case ir.CondBranch:
		return []string{fmt.Sprintf("if (%s == %d) { /* take branch — caller fills label */ }", regName(op.Flag), op.TakeIf)}, nil
	case ir.Goto:
		return []string{"/* Goto — caller fills label */"}, nil
	case ir.IndirectGoto:
		bank, address, err := addressExpression(op.Seg)
		if err != nil {
			return nil, err
		}
		return []string{fmt.Sprintf("/* IndirectGoto: target = (%s, %s) — caller dispatches */", bank, address)}, nil
	case ir.Call:
		return emitCall(context, op), nil
	case ir.Return:
		return emitReturn(context, op), nil
	case ir.Nop:
		return []string{"/* NOP */"}, nil
	case ir.Break:
		if op.COP {
			return []string{"if (g_cpu_cop_hook) g_cpu_cop_hook(cpu);  /* COP: software interrupt -> vector, RTI to PC+2 */"}, nil
		}
		return []string{"if (g_cpu_brk_hook) g_cpu_brk_hook(cpu);  /* BRK: software interrupt -> vector, RTI to PC+2 */"}, nil
	case ir.Stop:
		if op.Wait {
			return []string{"/* WAI: wait for interrupt — runtime hook */"}, nil
		}
		return []string{"/* STP: halt — runtime hook */"}, nil
	case ir.PushEffectiveAddress:
		return emitPushEffective(op), nil
	case ir.BlockMove:
		return emitBlockMove(op), nil
	default:
		return []string{fmt.Sprintf("/* UNHANDLED IR op %T */", operation)}, nil
	}
}

func EmitBlock(context *Context, block ir.Block, indent string) ([]string, error) {
	if indent == "" {
		indent = "  "
	}
	lines := []string{"{"}
	for _, operation := range block.Ops {
		emitted, err := EmitOperation(context, operation)
		if err != nil {
			return nil, err
		}
		lines = appendIndented(lines, emitted, indent)
	}
	return append(lines, "}"), nil
}

func wrapModifyP(mask byte, sep bool) []string {
	lines := []string{"{"}
	lines = appendIndented(lines, modifyP(mask, sep), "  ")
	return append(lines, "}")
}

func emitALU(op ir.Alu) []string {
	lhs, rhs := masked(valueName(op.LHS), op.Width), masked(valueName(op.RHS), op.Width)
	name := fmt.Sprintf("_tc%d_%d", op.LHS.ID, op.RHS.ID)
	if op.Out != nil {
		name = fmt.Sprintf("_t%d", op.Out.ID)
	}
	var lines []string
	switch op.Kind {
	case ir.Add:
		lines = append(lines, fmt.Sprintf("uint32 %s = (uint32)%s + (uint32)%s + cpu->_flag_C;", name, lhs, rhs))
		if op.Out == nil {
			return append(lines, setCarryFromOverflow(name, op.Width, true))
		}
		out := valueName(*op.Out)
		lines = append(lines, fmt.Sprintf("%s %s;", ctype(op.Width), out), "if (cpu->_flag_D) {")
		lines = append(lines, fmt.Sprintf("  uint32 _da=(uint32)%s, _db=(uint32)%s, _dc=cpu->_flag_C, _dr=0, _vci=0;", lhs, rhs))
		for digit := 0; digit < 2*op.Width; digit++ {
			shift := 4 * digit
			if digit == 2*op.Width-1 {
				lines = append(lines, "  _vci=_dc;")
			}
			lines = append(lines, fmt.Sprintf("  { uint32 _n=((_da>>%d)&0xF)+((_db>>%d)&0xF)+_dc; _dc=(_n>9)?1:0; if(_dc)_n+=6; _dr|=(_n&0xF)<<%d; }", shift, shift, shift))
		}
		topShift := 8*op.Width - 4
		lines = append(lines,
			fmt.Sprintf("  %s=(%s)_dr; cpu->_flag_C=_dc;", out, ctype(op.Width)),
			fmt.Sprintf("  uint32 _vis=(_da & (0xFu<<%d)) + (_db & (0xFu<<%d)) + (_vci<<%d);", topShift, topShift, topShift),
			fmt.Sprintf("  cpu->_flag_V = (((_da ^ _vis) & (_db ^ _vis) & %s) != 0) ? 1 : 0;", signBit(op.Width)),
			"} else {", fmt.Sprintf("  %s=(%s)%s;", out, ctype(op.Width), name),
			"  "+setCarryFromOverflow(name, op.Width, true), "  "+setVADC(lhs, rhs, out, op.Width), "}")
		return append(lines, setNZNoP(out, op.Width)...)
	case ir.Sub:
		lines = append(lines, fmt.Sprintf("uint32 %s = (uint32)%s - (uint32)%s - (1 - cpu->_flag_C);", name, lhs, rhs))
		if op.Out == nil {
			return append(lines, setCarryFromOverflow(name, op.Width, false))
		}
		out := valueName(*op.Out)
		lines = append(lines, fmt.Sprintf("%s %s;", ctype(op.Width), out), "if (cpu->_flag_D) {", fmt.Sprintf("  uint32 _da=(uint32)%s, _db=(uint32)%s, _dr=0; int _bw=1-(int)cpu->_flag_C;", lhs, rhs))
		for digit := 0; digit < 2*op.Width; digit++ {
			shift := 4 * digit
			lines = append(lines, fmt.Sprintf("  { int _n=(int)((_da>>%d)&0xF)-(int)((_db>>%d)&0xF)-_bw; if(_n<0){_n+=10;_bw=1;}else _bw=0; _dr|=((uint32)_n&0xF)<<%d; }", shift, shift, shift))
		}
		lines = append(lines, fmt.Sprintf("  %s=(%s)_dr;", out, ctype(op.Width)), "} else {", fmt.Sprintf("  %s=(%s)%s;", out, ctype(op.Width), name), "}", setCarryFromOverflow(name, op.Width, false), setVSBC(lhs, rhs, fmt.Sprintf("((%s)%s)", ctype(op.Width), name), op.Width))
		return append(lines, setNZNoP(out, op.Width)...)
	case ir.And, ir.Or, ir.Xor:
		operator := map[ir.AluKind]string{ir.And: "&", ir.Or: "|", ir.Xor: "^"}[op.Kind]
		out := valueName(*op.Out)
		lines = append(lines, fmt.Sprintf("%s %s = (%s)(%s %s %s);", ctype(op.Width), out, ctype(op.Width), valueName(op.LHS), operator, valueName(op.RHS)))
		return append(lines, setNZNoP(out, op.Width)...)
	case ir.Compare:
		lines = append(lines, fmt.Sprintf("uint32 %s = (uint32)%s - (uint32)%s;", name, lhs, rhs), fmt.Sprintf("cpu->_flag_C = (%s >= %s) ? 1 : 0;", lhs, rhs))
		return append(lines, setNZNoP(fmt.Sprintf("(%s)%s", ctype(op.Width), name), op.Width)...)
	default:
		return []string{fmt.Sprintf("/* unhandled ALU %s */", op.Kind)}
	}
}

func emitShift(op ir.Shift) []string {
	source, output, typ := masked(valueName(op.Src), op.Width), valueName(op.Out), ctype(op.Width)
	var expression, carry string
	switch op.Kind {
	case ir.ASL:
		expression, carry = source+" << 1", setCarryFromBit(source, signBit(op.Width))
	case ir.LSR:
		expression, carry = source+" >> 1", setCarryFromBit(source, "1")
	case ir.ROL:
		expression, carry = fmt.Sprintf("(%s << 1) | cpu->_flag_C", source), setCarryFromBit(source, signBit(op.Width))
	case ir.ROR:
		expression, carry = fmt.Sprintf("(%s >> 1) | ((uint%d)cpu->_flag_C << %d)", source, op.Width*8, op.Width*8-1), setCarryFromBit(source, "1")
	}
	lines := []string{fmt.Sprintf("%s %s = (%s)(%s);", typ, output, typ, expression), carry}
	return append(lines, setNZNoP(output, op.Width)...)
}

func emitIncReg(op ir.IncReg) []string {
	field, delta := regName(op.Reg), "-1"
	if op.Delta == 1 {
		delta = "1"
	}
	if op.Reg == ir.A || op.Reg == ir.X || op.Reg == ir.Y {
		flag := "cpu->m_flag"
		lowWrite := fmt.Sprintf("%s = %s;", field, preserveHigh(field, "_lo8"))
		if op.Reg != ir.A {
			flag = "cpu->x_flag"
			lowWrite = fmt.Sprintf("%s = %s;  /* x=1 zeros high byte (hw contract) */", field, zeroExtendLow("_lo8"))
		}
		lines := []string{fmt.Sprintf("if (%s) {", flag), fmt.Sprintf("  uint8 _lo8 = (%s) + (%s);", lowByte(field), delta), "  " + lowWrite}
		lines = appendIndented(lines, setNZNoP("_lo8", 1), "  ")
		lines = append(lines, "} else {", fmt.Sprintf("  %s = (uint16)((%s) + (%s));", field, field, delta))
		lines = appendIndented(lines, setNZNoP(field, 2), "  ")
		return append(lines, "}")
	}
	return append([]string{fmt.Sprintf("%s = (%s) + (%s);", field, field, delta)}, setNZNoP(field, 2)...)
}

func emitIncMem(op ir.IncMem) ([]string, error) {
	bank, address, err := addressExpression(op.Seg)
	if err != nil {
		return nil, err
	}
	delta := "-1"
	if op.Delta == 1 {
		delta = "+1"
	}
	lines := []string{"{", fmt.Sprintf("  %s _im = %s(cpu, %s, %s);", ctype(op.Width), readFunction(op.Width), bank, address), fmt.Sprintf("  _im = (%s)(_im %s);", ctype(op.Width), delta), fmt.Sprintf("  %s(cpu, %s, %s, _im);", writeFunction(op.Width), bank, address)}
	lines = appendIndented(lines, setNZNoP("_im", op.Width), "  ")
	return append(lines, "}"), nil
}

func emitBitTest(op ir.BitTest) []string {
	operand := masked(valueName(op.Operand), op.Width)
	lines := []string{"{", fmt.Sprintf("  %s _bt = (%s)(%s & %s);", ctype(op.Width), ctype(op.Width), masked("cpu->A", op.Width), operand), "  cpu->_flag_Z = (_bt == 0) ? 1 : 0;"}
	if !op.ZOnly {
		lines = append(lines, fmt.Sprintf("  cpu->_flag_N = ((%s & %s) != 0) ? 1 : 0;", operand, signBit(op.Width)), fmt.Sprintf("  cpu->_flag_V = ((%s & %s) != 0) ? 1 : 0;", operand, overflowBit(op.Width)))
	}
	return append(lines, "}")
}

func emitBitMemory(segment ir.SegRef, width int, clear bool) ([]string, error) {
	bank, address, err := addressExpression(segment)
	if err != nil {
		return nil, err
	}
	operator := "|"
	if clear {
		operator = "& ~"
	}
	return []string{"{", fmt.Sprintf("  %s _m = %s(cpu, %s, %s);", ctype(width), readFunction(width), bank, address), "  cpu->_flag_Z = ((_m & cpu->A) == 0) ? 1 : 0;", fmt.Sprintf("  %s(cpu, %s, %s, (%s)(_m %s cpu->A));", writeFunction(width), bank, address, ctype(width), operator), "}"}, nil
}

func emitSetFlag(op ir.SetFlag) []string {
	lines := []string{fmt.Sprintf("%s = %d;", regName(op.Flag), op.Value)}
	mask := map[ir.Reg]string{ir.C: "0x01", ir.ZF: "0x02", ir.I: "0x04", ir.DF: "0x08", ir.XF: "0x10", ir.M: "0x20", ir.V: "0x40", ir.N: "0x80"}[op.Flag]
	if mask == "" {
		return lines
	}
	if op.Value != 0 {
		return append(lines, fmt.Sprintf("cpu->P = (uint8)(cpu->P | %s);", mask))
	}
	return append(lines, fmt.Sprintf("cpu->P = (uint8)(cpu->P & ~%s);", mask))
}
