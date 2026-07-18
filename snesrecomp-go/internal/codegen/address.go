package codegen

import (
	"fmt"

	"github.com/DerrickGold/snesrecomp-go/internal/ir"
)

var registerField = map[ir.Reg]string{
	ir.A: "cpu->A", ir.B: "((uint8)((cpu->A >> 8) & 0xFF))",
	ir.X: "cpu->X", ir.Y: "cpu->Y", ir.S: "cpu->S", ir.D: "cpu->D",
	ir.DB: "cpu->DB", ir.PB: "cpu->PB", ir.P: "cpu->P",
	ir.M: "cpu->m_flag", ir.XF: "cpu->x_flag", ir.E: "cpu->emulation",
	ir.N: "cpu->_flag_N", ir.V: "cpu->_flag_V", ir.ZF: "cpu->_flag_Z",
	ir.C: "cpu->_flag_C", ir.I: "cpu->_flag_I", ir.DF: "cpu->_flag_D",
}

func valueName(value ir.Value) string { return fmt.Sprintf("_v%d", value.ID) }
func regName(register ir.Reg) string  { return registerField[register] }

func addressExpression(segment ir.SegRef) (string, string, error) {
	index := ""
	if segment.Index != nil && *segment.Index == ir.X {
		index = " + cpu->X"
	} else if segment.Index != nil && *segment.Index == ir.Y {
		index = " + cpu->Y"
	}
	switch segment.Kind {
	case ir.Direct:
		return "0x7E", fmt.Sprintf("(uint16)(cpu->D + 0x%04x%s)", segment.Offset, index), nil
	case ir.AbsoluteBank:
		if segment.Index == nil {
			return "cpu->DB", fmt.Sprintf("(uint16)(0x%04x)", segment.Offset), nil
		}
		indexRegister := "cpu->Y"
		if *segment.Index == ir.X {
			indexRegister = "cpu->X"
		}
		effective := fmt.Sprintf("(((uint32)cpu->DB << 16) + (uint32)0x%04x + (uint32)%s)", segment.Offset, indexRegister)
		return fmt.Sprintf("(uint8)((%s) >> 16)", effective), fmt.Sprintf("(uint16)(%s)", effective), nil
	case ir.Long:
		bank := byte(0)
		if segment.Bank != nil {
			bank = *segment.Bank
		}
		if segment.Index == nil {
			return fmt.Sprintf("0x%02x", bank), fmt.Sprintf("(uint16)(0x%04x)", segment.Offset), nil
		}
		indexRegister := "cpu->Y"
		if *segment.Index == ir.X {
			indexRegister = "cpu->X"
		}
		effective := fmt.Sprintf("((uint32)0x%06x + (uint32)%s)", uint32(bank)<<16|segment.Offset&0xffff, indexRegister)
		return fmt.Sprintf("(uint8)((%s) >> 16)", effective), fmt.Sprintf("(uint16)(%s)", effective), nil
	case ir.Stack:
		return "0x00", fmt.Sprintf("(uint16)(cpu->S + 0x%04x)", segment.Offset), nil
	case ir.DPIndirect:
		pointerAddress := fmt.Sprintf("(uint16)(cpu->D + 0x%04x)", segment.Offset)
		pointer := fmt.Sprintf("cpu_read16_dp(cpu, %s)", pointerAddress)
		if segment.Index == nil {
			return "cpu->DB", fmt.Sprintf("(uint16)(%s)", pointer), nil
		}
		indexRegister := "cpu->Y"
		if *segment.Index == ir.X {
			indexRegister = "cpu->X"
		}
		effective := fmt.Sprintf("(((uint32)cpu->DB << 16) + (uint32)(%s) + (uint32)%s)", pointer, indexRegister)
		return fmt.Sprintf("(uint8)((%s) >> 16)", effective), fmt.Sprintf("(uint16)(%s)", effective), nil
	case ir.DPIndirectLong:
		pointerAddress := fmt.Sprintf("(uint16)(cpu->D + 0x%04x)", segment.Offset)
		pointerLow := fmt.Sprintf("cpu_read16_dp(cpu, %s)", pointerAddress)
		pointerBank := fmt.Sprintf("cpu_read8(cpu, 0x00, (uint16)(%s + 2))", pointerAddress)
		if segment.Index == nil {
			return pointerBank, fmt.Sprintf("(uint16)(%s)", pointerLow), nil
		}
		indexRegister := "cpu->Y"
		if *segment.Index == ir.X {
			indexRegister = "cpu->X"
		}
		effective := fmt.Sprintf("(((uint32)(%s) << 16) + (uint32)(%s) + (uint32)%s)", pointerBank, pointerLow, indexRegister)
		return fmt.Sprintf("(uint8)((%s) >> 16)", effective), fmt.Sprintf("(uint16)(%s)", effective), nil
	case ir.AbsoluteIndirect:
		return "cpu->PB", fmt.Sprintf("cpu_read16(cpu, cpu->PB, (uint16)0x%04x)", segment.Offset), nil
	case ir.AbsoluteIndirectX:
		return "cpu->PB", fmt.Sprintf("cpu_read16(cpu, cpu->PB, (uint16)(0x%04x + cpu->X))", segment.Offset), nil
	case ir.AbsoluteIndirectLong:
		address := fmt.Sprintf("(uint16)0x%04x", segment.Offset)
		return fmt.Sprintf("cpu_read8(cpu, 0x00, (uint16)(%s + 2))", address), fmt.Sprintf("cpu_read16(cpu, 0x00, %s)", address), nil
	case ir.DPIndirectX:
		pointerAddress := fmt.Sprintf("(uint16)(cpu->D + 0x%04x + cpu->X)", segment.Offset)
		return "cpu->DB", fmt.Sprintf("cpu_read16_dp(cpu, %s)", pointerAddress), nil
	case ir.StackRelativeIndirectY:
		pointerAddress := fmt.Sprintf("(uint16)(cpu->S + 0x%04x)", segment.Offset)
		pointer := fmt.Sprintf("cpu_read16_dp(cpu, %s)", pointerAddress)
		effective := fmt.Sprintf("(((uint32)cpu->DB << 16) + (uint32)(%s) + (uint32)cpu->Y)", pointer)
		return fmt.Sprintf("(uint8)((%s) >> 16)", effective), fmt.Sprintf("(uint16)(%s)", effective), nil
	default:
		return "", "", fmt.Errorf("unsupported segment kind %q", segment.Kind)
	}
}
