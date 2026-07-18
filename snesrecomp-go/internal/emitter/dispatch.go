package emitter

import (
	"fmt"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func dispatchBaseName(context *codegen.Context, address uint32) string {
	if name := context.Names[address&0xffffff]; name != "" {
		return name
	}
	return fmt.Sprintf("bank_%02X_%04X", byte(address>>16), uint16(address))
}

func demandAllVariants(context *codegen.Context, address uint32) {
	for m := uint8(0); m < 2; m++ {
		for x := uint8(0); x < 2; x++ {
			context.Demands[codegen.Variant{Address: address & 0xffffff, M: m, X: x}] = struct{}{}
		}
	}
}

func pbCallEnvelope(targetBank byte, callee string) []string {
	return []string{
		"uint8 _saved_pb = cpu->PB;",
		fmt.Sprintf("cpu_trace_pb_change(cpu, 0, _saved_pb, 0x%02x, CPU_TR_JSL);", targetBank),
		fmt.Sprintf("cpu->PB = 0x%02x;", targetBank),
		fmt.Sprintf("RecompReturn _r = %s(cpu);", callee),
		"cpu_trace_pb_change(cpu, 0, cpu->PB, _saved_pb, CPU_TR_RTL);",
		"cpu->PB = _saved_pb;",
		"if (_r != RECOMP_RETURN_NORMAL) {",
		"  cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_r, 0);",
		"  cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);",
		"  return (_r == RECOMP_RETURN_TAILCALL ? _r : (RecompReturn)((int)_r - 1));",
		"}",
	}
}

func emitJSLDispatch(context *codegen.Context, instruction *cpu65816.Instruction) []string {
	entries := instruction.DispatchEntries
	lines := []string{
		"{ /* JSL dispatch — short=2B / long=3B table */",
		fmt.Sprintf("  static const uint16 _disp_n = %d;", len(entries)),
		"  uint16 _idx = (uint16)(cpu->A & 0xFF);",
		"  if (_idx >= _disp_n) { return RECOMP_RETURN_NORMAL; /* dispatch OOB */ }",
		"  {",
		"    uint8 _old_p = cpu->P;",
		"    cpu_mirrors_to_p(cpu);",
		"    cpu->P = (uint8)(cpu->P | 0x30);",
		"    cpu_p_to_mirrors(cpu);",
		"    cpu_trace_px_record(cpu, 0, 1 /*SEP*/, _old_p, cpu->P);",
		"  }",
		"  cpu->host_return_valid = 0;  /* dispatch-trampoline target */",
		"  switch (_idx) {",
	}
	for index, entry := range entries {
		if entry == 0 {
			lines = append(lines, fmt.Sprintf("    case %d: break;  /* null entry */", index))
			continue
		}
		address := entry & 0xffffff
		if instruction.DispatchKind != "long" {
			address = uint32(byte(instruction.Address>>16))<<16 | entry&0xffff
		}
		context.Demands[codegen.Variant{Address: address, M: 1, X: 1}] = struct{}{}
		name := dispatchBaseName(context, address) + "_M1X1"
		lines = append(lines, fmt.Sprintf("    case %d: {", index))
		for _, statement := range pbCallEnvelope(byte(address>>16), name) {
			lines = append(lines, "      "+statement)
		}
		lines = append(lines, "    } break;")
	}
	return append(lines, "    default: break;", "  }", "  return RECOMP_RETURN_NORMAL; /* dispatch is a terminator */", "}")
}

func emitIndirectDispatch(context *codegen.Context, instruction *cpu65816.Instruction, local map[decoder.DecodeKey]struct{}) []string {
	if instruction.DispatchIndexReg != "A" {
		return emitIndexedIndirectDispatch(context, instruction, local)
	}
	entries := instruction.DispatchEntries
	site := instruction.Address & 0xffffff
	isJSR := instruction.Mnemonic == "JSR"
	isCallReturn := instruction.DispatchReturn != nil
	isJSRLike := isJSR || isCallReturn
	m, x := instruction.M&1, instruction.X&1
	if instruction.DispatchTerminal {
		m, x = 1, 1
	} else {
		if instruction.DispatchSEP&0x20 != 0 {
			m = 1
		}
		if instruction.DispatchSEP&0x10 != 0 {
			x = 1
		}
	}
	comment := "indirect dispatch terminator: cfg-resolved target list"
	if isJSR {
		comment = "indirect dispatch call: cfg-resolved target list"
	}
	if instruction.DispatchTerminal {
		comment = "RTS-stack dispatch terminator: cfg-resolved target list"
	}
	lines := []string{fmt.Sprintf("{ /* %s */", comment)}
	if isCallReturn {
		lines = append(lines, "  cpu->host_return_valid = 1;  /* PHA/RTS jump-table call (return frame pre-pushed by PHY) */")
	} else if isJSR {
		returnAddress := uint16(site + 2)
		lines = append(lines, fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", byte(returnAddress>>8)), fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", byte(returnAddress)), "  cpu->host_return_valid = 1;  /* indirect JSR call */")
	} else {
		lines = append(lines, "  cpu->host_return_valid = _hrv;  /* JMP/JML indirect tail dispatch */")
	}
	if instruction.DispatchSEP != 0 {
		lines = append(lines, fmt.Sprintf("  { /* SEP #$%02X (sat between PHA and dispatching RTS in the original code) */", instruction.DispatchSEP))
		lines = append(lines, "    uint8 _old_p = cpu->P;", "    cpu_mirrors_to_p(cpu);", fmt.Sprintf("    cpu->P = (uint8)(cpu->P | 0x%02x);", instruction.DispatchSEP), "    cpu_p_to_mirrors(cpu);", "    cpu_trace_px_record(cpu, 0, 1 /*SEP*/, _old_p, cpu->P);", "  }")
	}
	lines = append(lines, "  uint16 _val = (uint16)(cpu->A & 0xFFFF);  /* PHA'd handler-1 value */", "  switch (_val) {")
	seen := make(map[uint16]struct{})
	for _, entry := range entries {
		if entry == 0 {
			continue
		}
		address := entry & 0xffffff
		caseValue := uint16(entry - 1)
		if _, found := seen[caseValue]; found {
			continue
		}
		seen[caseValue] = struct{}{}
		baseName := dispatchBaseName(context, address)
		demandAllVariants(context, address)
		lines = append(lines, fmt.Sprintf("    case 0x%04x: {  /* -> %s */", caseValue, baseName), "      uint8 _saved_pb = cpu->PB;", fmt.Sprintf("      cpu_trace_pb_change(cpu, 0, _saved_pb, 0x%02x, CPU_TR_JSL);", byte(address>>16)), fmt.Sprintf("      cpu->PB = 0x%02x;", byte(address>>16)), "      RecompReturn _r;", "      switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {")
		preCall := ""
		if !isJSRLike {
			preCall = "cpu_tailcall_inherit_return_context(_entry_s, _hrv); "
		}
		lines = append(lines, codegen.VariantDispatchCases(context, address, baseName, "        ", preCall)...)
		lines = append(lines, "      }", "      cpu_trace_pb_change(cpu, 0, cpu->PB, _saved_pb, CPU_TR_RTL);", "      cpu->PB = _saved_pb;", "      if (_r != RECOMP_RETURN_NORMAL) {", "        cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_r, 0);", "        cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);", "        return (_r == RECOMP_RETURN_TAILCALL ? _r : (RecompReturn)((int)_r - 1));", "      }")
		if isJSRLike {
			lines = append(lines, "      break;")
		} else {
			lines = append(lines, "      return RECOMP_RETURN_NORMAL;")
		}
		lines = append(lines, "    }")
	}
	lines = append(lines, "    default:", fmt.Sprintf("      (void)cpu_trace_dispatch_oob(cpu, 0x%06x, _val);", site))
	if isCallReturn {
		phyBytes := 2
		if instruction.X&1 != 0 {
			phyBytes = 1
		}
		lines = append(lines, fmt.Sprintf("      cpu->S = (uint16)(cpu->S + %du);  /* undo is_call_ret's pre-pushed PHY frame (unknown value) */", phyBytes), "      break;")
	} else if isJSR {
		lines = append(lines, "      break;")
	} else {
		lines = append(lines, "      return RECOMP_RETURN_NORMAL;")
	}
	lines = append(lines, "  }")
	if isCallReturn {
		lines = append(lines, dispatchReturnTransfer(context, instruction, local, m, x)...)
	} else if isJSR {
		lines = append(lines, "  /* fall through to post-JSR block */")
	}
	return append(lines, "}")
}

func dispatchReturnTransfer(context *codegen.Context, instruction *cpu65816.Instruction, local map[decoder.DecodeKey]struct{}, m, x uint8) []string {
	if instruction.DispatchReturn == nil {
		return nil
	}
	address := uint32(byte(instruction.Address>>16))<<16 | uint32(*instruction.DispatchReturn)
	key := decoder.DecodeKey{PC: address, M: m & 1, X: x & 1}
	if _, found := local[key]; found {
		return []string{"  goto " + label(key) + ";"}
	}
	name := context.Names[address]
	if name == "" {
		return []string{"  goto " + label(key) + ";"}
	}
	context.Demands[codegen.Variant{Address: address, M: m & 1, X: x & 1}] = struct{}{}
	target := fmt.Sprintf("0x%06xu", address)
	return []string{fmt.Sprintf("  { if (!_hrv) { cpu->host_return_valid = _hrv; cpu_tailcall_inherit_return_context(_entry_s, _hrv); cpu_tailcall_request(%s, _entry_s, %s); RecompStackPop(); return RECOMP_RETURN_TAILCALL; } RecompStackPop(); return cpu_dispatch_pc_from(cpu, %s, _entry_s, %s); }  /* dispatch-ret tail-call: $%04X is registered func %s_M%dX%d */", target, target, target, target, *instruction.DispatchReturn, name, m&1, x&1)}
}

func emitIndexedIndirectDispatch(context *codegen.Context, instruction *cpu65816.Instruction, local map[decoder.DecodeKey]struct{}) []string {
	entries := instruction.DispatchEntries
	site := instruction.Address & 0xffffff
	isJSR := instruction.Mnemonic == "JSR"
	isCallReturn := instruction.DispatchReturn != nil
	isJSRLike := isJSR || isCallReturn
	m, x := instruction.M&1, instruction.X&1
	if instruction.DispatchTerminal {
		m, x = 1, 1
	} else {
		if instruction.DispatchSEP&0x20 != 0 {
			m = 1
		}
		if instruction.DispatchSEP&0x10 != 0 {
			x = 1
		}
	}
	comment := "indirect dispatch terminator: cfg-resolved target list"
	if isJSR {
		comment = "indirect dispatch call: cfg-resolved target list"
	}
	if instruction.DispatchTerminal {
		comment = "RTS-stack dispatch terminator: cfg-resolved target list"
	}
	lines := []string{fmt.Sprintf("{ /* %s */", comment)}
	if isCallReturn {
		lines = append(lines, "  cpu->host_return_valid = 1;  /* PHA/RTS jump-table call (return frame pre-pushed by PHY) */")
	} else if isJSR {
		ret := uint16(site + 2)
		lines = append(lines, fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", byte(ret>>8)), fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", byte(ret)), "  cpu->host_return_valid = 1;  /* indirect JSR call */")
	} else {
		lines = append(lines, "  cpu->host_return_valid = _hrv;  /* JMP/JML indirect tail dispatch */")
	}

	// Absolute-indirect single pointer: switch on the loaded pointer value.
	if instruction.Mode == cpu65816.INDIR && len(instruction.DispatchTableBase) == 1 {
		lines = append(lines, fmt.Sprintf("  uint16 _target = cpu_read16(cpu, cpu->PB, (uint16)0x%04x);  /* absolute indirect dispatch: switch on the loaded pointer */", uint16(instruction.Operand)))
		if instruction.DispatchTerminal {
			lines = append(lines, sepDispatchLines("  ", 0x30)...)
		}
		lines = append(lines, "  switch (_target) {")
		seen := make(map[uint32]struct{})
		for _, entry := range entries {
			if entry == 0 {
				continue
			}
			address := entry & 0xffffff
			caseValue := address & 0xffff
			if instruction.DispatchKind == "long" {
				caseValue = address
			}
			if _, duplicate := seen[caseValue]; duplicate {
				continue
			}
			seen[caseValue] = struct{}{}
			base := dispatchBaseName(context, address)
			lines = append(lines, fmt.Sprintf("    case 0x%04x: {", caseValue))
			lines = append(lines, dispatchCaseBody(context, address, base, isJSRLike, instruction.DispatchTerminal, m, x, "      ")...)
			if isJSRLike {
				lines = append(lines, "      break;")
			} else {
				lines = append(lines, "      return RECOMP_RETURN_NORMAL;")
			}
			lines = append(lines, "    }")
		}
		lines = append(lines, "    default: break;", "  }")
		if isCallReturn {
			lines = append(lines, dispatchReturnTransfer(context, instruction, local, m, x)...)
		} else if isJSR {
			lines = append(lines, "  /* fall through to post-JSR block */")
		} else {
			lines = append(lines, fmt.Sprintf("  return cpu_trace_dispatch_oob(cpu, 0x%06x, _target);", site))
		}
		return append(lines, "}")
	}

	indexField := "X"
	if instruction.DispatchIndexReg == "Y" {
		indexField = "Y"
	}
	entrySize := 2
	if instruction.DispatchKind == "long" {
		entrySize = 3
	}
	if len(instruction.DispatchTableBase) >= 2 {
		lines = append(lines, fmt.Sprintf("  uint16 _idx = (uint16)(cpu->%s & 0xFFFF);  /* parallel byte tables: register already holds logical index */", indexField))
	} else {
		lines = append(lines, fmt.Sprintf("  uint16 _idx = (uint16)((cpu->%s & 0xFFFF) / %d);  /* entry_size=%d (%s); ASL[*N] + TAX in asm => %s is byte offset, divide back to logical index */", indexField, entrySize, entrySize, instruction.DispatchKind, indexField))
	}
	if instruction.DispatchTerminal {
		lines = append(lines, sepDispatchLines("  ", 0x30)...)
	}
	lines = append(lines, fmt.Sprintf("  static const uint16 _disp_n = %d;", len(entries)), "  if (_idx >= _disp_n) {")
	if isCallReturn {
		phyBytes := 2
		if instruction.X&1 != 0 {
			phyBytes = 1
		}
		lines = append(lines, fmt.Sprintf("    (void)cpu_trace_dispatch_oob(cpu, 0x%06x, _idx);", site), fmt.Sprintf("    cpu->S = (uint16)(cpu->S + %du);  /* undo is_call_ret's pre-pushed PHY frame (OOB index) */", phyBytes))
	} else if isJSRLike {
		lines = append(lines, fmt.Sprintf("    (void)cpu_trace_dispatch_oob(cpu, 0x%06x, _idx);", site))
	} else {
		lines = append(lines, fmt.Sprintf("    return cpu_trace_dispatch_oob(cpu, 0x%06x, _idx);", site))
	}
	lines = append(lines, "  }", "  switch (_idx) {")
	for index, entry := range entries {
		if entry == 0 {
			if isJSRLike {
				lines = append(lines, fmt.Sprintf("    case %d: break; /* null entry */", index))
			} else {
				lines = append(lines, fmt.Sprintf("    case %d: return RECOMP_RETURN_NORMAL; /* null entry */", index))
			}
			continue
		}
		address := entry & 0xffffff
		base := dispatchBaseName(context, address)
		lines = append(lines, fmt.Sprintf("    case %d: {", index))
		lines = append(lines, dispatchCaseBody(context, address, base, isJSRLike, instruction.DispatchTerminal, m, x, "      ")...)
		if isJSRLike {
			lines = append(lines, "      break;")
		} else {
			lines = append(lines, "      return RECOMP_RETURN_NORMAL;")
		}
		lines = append(lines, "    }")
	}
	lines = append(lines, "    default: break; /* unreachable: gated above */", "  }")
	if isCallReturn {
		lines = append(lines, dispatchReturnTransfer(context, instruction, local, m, x)...)
	} else if isJSR {
		lines = append(lines, "  /* fall through to post-JSR block */")
	} else {
		lines = append(lines, fmt.Sprintf("  return cpu_trace_dispatch_oob(cpu, 0x%06x, _idx);", site))
	}
	return append(lines, "}")
}

func sepDispatchLines(indent string, mask byte) []string {
	return []string{indent + "{", indent + "  uint8 _old_p = cpu->P;", indent + "  cpu_mirrors_to_p(cpu);", fmt.Sprintf("%s  cpu->P = (uint8)(cpu->P | 0x%02x);", indent, mask), indent + "  cpu_p_to_mirrors(cpu);", indent + "  cpu_trace_px_record(cpu, 0, 1 /*SEP*/, _old_p, cpu->P);", indent + "}"}
}

func dispatchCaseBody(context *codegen.Context, address uint32, base string, isJSRLike, terminal bool, m, x uint8, indent string) []string {
	if terminal {
		context.Demands[codegen.Variant{Address: address, M: m & 1, X: x & 1}] = struct{}{}
		name := fmt.Sprintf("%s_M%dX%d", base, m&1, x&1)
		lines := []string{indent + "uint8 _saved_pb = cpu->PB;", fmt.Sprintf("%scpu_trace_pb_change(cpu, 0, _saved_pb, 0x%02x, CPU_TR_JSL);", indent, byte(address>>16)), fmt.Sprintf("%scpu->PB = 0x%02x;", indent, byte(address>>16))}
		if !isJSRLike {
			lines = append(lines, indent+"cpu_tailcall_inherit_return_context(_entry_s, _hrv);")
		}
		lines = append(lines, fmt.Sprintf("%sRecompReturn _r = %s(cpu);", indent, name), indent+"cpu_trace_pb_change(cpu, 0, cpu->PB, _saved_pb, CPU_TR_RTL);", indent+"cpu->PB = _saved_pb;", indent+"if (_r != RECOMP_RETURN_NORMAL) {", indent+"  cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_r, 0);", indent+"  cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);", indent+"  return (_r == RECOMP_RETURN_TAILCALL ? _r : (RecompReturn)((int)_r - 1));", indent+"}")
		return lines
	}
	demandAllVariants(context, address)
	lines := []string{indent + "uint8 _saved_pb = cpu->PB;", fmt.Sprintf("%scpu_trace_pb_change(cpu, 0, _saved_pb, 0x%02x, CPU_TR_JSL);", indent, byte(address>>16)), fmt.Sprintf("%scpu->PB = 0x%02x;", indent, byte(address>>16)), indent + "RecompReturn _r;", indent + "switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {"}
	pre := ""
	if !isJSRLike {
		pre = "cpu_tailcall_inherit_return_context(_entry_s, _hrv); "
	}
	lines = append(lines, codegen.VariantDispatchCases(context, address, base, indent+"  ", pre)...)
	return append(lines, indent+"}", indent+"cpu_trace_pb_change(cpu, 0, cpu->PB, _saved_pb, CPU_TR_RTL);", indent+"cpu->PB = _saved_pb;", indent+"if (_r != RECOMP_RETURN_NORMAL) {", indent+"  cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_r, 0);", indent+"  cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);", indent+"  return (_r == RECOMP_RETURN_TAILCALL ? _r : (RecompReturn)((int)_r - 1));", indent+"}")
}
