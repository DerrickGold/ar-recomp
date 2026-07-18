package codegen

import "fmt"

func opMask(width int) string {
	if width == 1 {
		return "0xFF"
	}
	return "0xFFFF"
}
func signBit(width int) string {
	if width == 1 {
		return "0x80"
	}
	return "0x8000"
}
func carryBit(width int) string {
	if width == 1 {
		return "0x100"
	}
	return "0x10000"
}
func overflowBit(width int) string {
	if width == 1 {
		return "0x40"
	}
	return "0x4000"
}
func ctype(width int) string {
	if width == 1 {
		return "uint8"
	}
	return "uint16"
}
func masked(expression string, width int) string {
	return fmt.Sprintf("(%s & %s)", expression, opMask(width))
}
func setNZNoP(source string, width int) []string {
	return []string{
		fmt.Sprintf("cpu->_flag_Z = ((%s) == 0) ? 1 : 0;", source),
		fmt.Sprintf("cpu->_flag_N = (((%s) & %s) != 0) ? 1 : 0;", source, signBit(width)),
	}
}
func setNZ(source string, width int) []string {
	return append(setNZNoP(source, width), "cpu->P = (uint8)((cpu->P & ~0x82) | (cpu->_flag_Z ? 0x02 : 0) | (cpu->_flag_N ? 0x80 : 0));")
}
func setCarryFromBit(source, mask string) string {
	return fmt.Sprintf("cpu->_flag_C = ((%s) & %s) ? 1 : 0;", source, mask)
}
func setCarryFromOverflow(temp string, width int, addition bool) string {
	if addition {
		return fmt.Sprintf("cpu->_flag_C = (%s & %s) ? 1 : 0;", temp, carryBit(width))
	}
	return fmt.Sprintf("cpu->_flag_C = (%s & %s) ? 0 : 1;", temp, carryBit(width))
}
func setVADC(lhs, rhs, output string, width int) string {
	return fmt.Sprintf("cpu->_flag_V = (((%s ^ %s) & (%s ^ %s) & %s) != 0) ? 1 : 0;", lhs, output, rhs, output, signBit(width))
}
func setVSBC(lhs, rhs, output string, width int) string {
	return fmt.Sprintf("cpu->_flag_V = (((%s ^ %s) & (%s ^ %s) & %s) != 0) ? 1 : 0;", lhs, rhs, lhs, output, signBit(width))
}
func preserveHigh(field, low string) string {
	return fmt.Sprintf("(uint16)((%s & 0xFF00) | ((%s) & 0xFF))", field, low)
}
func zeroExtendLow(low string) string { return fmt.Sprintf("(uint16)((%s) & 0xFF)", low) }
func lowByte(field string) string     { return fmt.Sprintf("(uint8)(%s & 0xFF)", field) }
func readFunction(width int) string {
	if width == 1 {
		return "cpu_read8"
	}
	return "cpu_read16"
}
func writeFunction(width int) string {
	if width == 1 {
		return "cpu_write8"
	}
	return "cpu_write16"
}

func pushByte(value string) []string {
	return []string{fmt.Sprintf("cpu_write8(cpu, 0x00, cpu->S, %s);", value), "cpu->S = (uint16)(cpu->S - 1);"}
}
func pushWord(value string) []string {
	return []string{"cpu->S = (uint16)(cpu->S - 1);", fmt.Sprintf("cpu_write16(cpu, 0x00, cpu->S, %s);", value), "cpu->S = (uint16)(cpu->S - 1);"}
}
func popByte(target string) []string {
	return []string{"cpu->S = (uint16)(cpu->S + 1);", fmt.Sprintf("%s = cpu_read8(cpu, 0x00, cpu->S);", target)}
}
func popWord(target string) []string {
	return []string{"cpu->S = (uint16)(cpu->S + 1);", fmt.Sprintf("%s = cpu_read16(cpu, 0x00, cpu->S);", target), "cpu->S = (uint16)(cpu->S + 1);"}
}
func tracedStack(opID string, delta int, body []string) []string {
	lines := []string{"{", "  uint16 _old_s = cpu->S;"}
	lines = appendIndented(lines, body, "  ")
	lines = append(lines, fmt.Sprintf("  cpu_trace_stack_op(cpu, 0, %s, _old_s, %d);", opID, delta), "}")
	return lines
}
func modifyP(mask byte, sep bool) []string {
	verb, kind, label := "& ~", 0, "REP"
	if sep {
		verb, kind, label = "| ", 1, "SEP"
	}
	return []string{
		"uint8 _old_p = cpu->P;",
		"cpu_mirrors_to_p(cpu);",
		fmt.Sprintf("cpu->P = (uint8)(cpu->P %s0x%02x);", verb, mask),
		"cpu_p_to_mirrors(cpu);",
		fmt.Sprintf("cpu_trace_px_record(cpu, 0, %d /*%s*/, _old_p, cpu->P);", kind, label),
	}
}

func appendIndented(destination, source []string, indent string) []string {
	for _, line := range source {
		destination = append(destination, indent+line)
	}
	return destination
}
