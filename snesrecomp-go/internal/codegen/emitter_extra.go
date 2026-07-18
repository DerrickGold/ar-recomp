package codegen

import (
	"fmt"

	"github.com/DerrickGold/snesrecomp-go/internal/ir"
)

func emitPushReg(op ir.PushReg) []string {
	field := regName(op.Reg)
	switch op.Reg {
	case ir.P:
		lines := []string{"cpu_mirrors_to_p(cpu);"}
		lines = append(lines, tracedStack("CPU_STACK_OP_PHP", -1, pushByte("(uint8)("+field+")"))...)
		return append(lines, "cpu_trace_event(cpu, 0, CPU_TR_PHP, cpu->P, 0);", "cpu_trace_px_record(cpu, 0, 4 /*PHP*/, cpu->P, cpu->P);")
	case ir.DB:
		return append(tracedStack("CPU_STACK_OP_PHB", -1, pushByte("(uint8)("+field+")")), "cpu_trace_event(cpu, 0, CPU_TR_PHB, cpu->DB, cpu->DB);")
	case ir.PB:
		return append(tracedStack("CPU_STACK_OP_PHK", -1, pushByte("(uint8)("+field+")")), "cpu_trace_event(cpu, 0, CPU_TR_PHK, cpu->PB, cpu->PB);")
	case ir.D:
		return tracedStack("CPU_STACK_OP_PHD", -2, pushWord(field))
	case ir.A:
		return pushWidthControlled(field, "m", "CPU_STACK_OP_PHA", op.StaticM)
	case ir.X:
		return pushWidthControlled(field, "x", "CPU_STACK_OP_PHX", op.StaticX)
	case ir.Y:
		return pushWidthControlled(field, "x", "CPU_STACK_OP_PHY", op.StaticX)
	default:
		return []string{fmt.Sprintf("/* TODO PushReg(%s) */", op.Reg)}
	}
}

func pushWidthControlled(field, flag, opID string, static *uint8) []string {
	if static != nil {
		body, delta := pushWord(field), -2
		if *static&1 != 0 {
			body, delta = pushByte(lowByte(field)), -1
		}
		lines := []string{"{ uint16 _old_s = cpu->S;"}
		lines = appendIndented(lines, body, "  ")
		return append(lines, fmt.Sprintf("  cpu_trace_stack_op(cpu, 0, %s, _old_s, %d); }", opID, delta))
	}
	lines := []string{"{ uint16 _old_s = cpu->S;", fmt.Sprintf("  if (cpu->%s_flag) {", flag)}
	lines = appendIndented(lines, pushByte(lowByte(field)), "    ")
	lines = append(lines, fmt.Sprintf("    cpu_trace_stack_op(cpu, 0, %s, _old_s, -1);", opID), "  } else {")
	lines = appendIndented(lines, pushWord(field), "    ")
	return append(lines, fmt.Sprintf("    cpu_trace_stack_op(cpu, 0, %s, _old_s, -2);", opID), "  } }")
}

func emitPullReg(op ir.PullReg) []string {
	field := regName(op.Reg)
	switch op.Reg {
	case ir.P:
		return []string{
			"{ uint8 _old_p = cpu->P; uint16 _old_s = cpu->S;", "  cpu->S = (uint16)(cpu->S + 1);", "  cpu->P = cpu_read8(cpu, 0x00, cpu->S);", "  cpu_p_to_mirrors(cpu);",
			"  if (cpu->x_flag) { cpu->X = (uint16)(cpu->X & 0xFF); cpu->Y = (uint16)(cpu->Y & 0xFF); }", "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PLP, _old_s, +1);",
			"  cpu_trace_event(cpu, 0, CPU_TR_PLP, _old_p, cpu->P);", "  cpu_trace_px_record(cpu, 0, 2 /*PLP*/, _old_p, cpu->P); }",
		}
	case ir.DB:
		lines := []string{"{ uint8 _old_db = cpu->DB; uint16 _old_s = cpu->S;"}
		lines = appendIndented(lines, popByte(field), "  ")
		lines = appendIndented(lines, setNZ(field, 1), "  ")
		return append(lines, "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PLB, _old_s, +1);", "  cpu_trace_db_change(cpu, 0, _old_db, cpu->DB, CPU_TR_PLB); }")
	case ir.PB:
		lines := []string{"{ uint8 _old_pb = cpu->PB; uint16 _old_s = cpu->S;"}
		lines = appendIndented(lines, popByte(field), "  ")
		lines = appendIndented(lines, setNZ(field, 1), "  ")
		return append(lines, "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PLB, _old_s, +1);", "  cpu_trace_pb_change(cpu, 0, _old_pb, cpu->PB, CPU_TR_PB_WRITE); }")
	case ir.D:
		return tracedStack("CPU_STACK_OP_PLD", 2, append(popWord(field), setNZ(field, 2)...))
	case ir.A:
		return pullWidthControlled(field, true, "CPU_STACK_OP_PLA", op.StaticM)
	case ir.X:
		return pullWidthControlled(field, false, "CPU_STACK_OP_PLX", op.StaticX)
	case ir.Y:
		return pullWidthControlled(field, false, "CPU_STACK_OP_PLY", op.StaticX)
	default:
		return []string{fmt.Sprintf("/* TODO PullReg(%s) */", op.Reg)}
	}
}

func pullWidthControlled(field string, accumulator bool, opID string, static *uint8) []string {
	pSync := "cpu->P = (uint8)((cpu->P & ~0x82) | (cpu->_flag_Z ? 0x02 : 0) | (cpu->_flag_N ? 0x80 : 0));"
	emit8 := func(indent string) []string {
		lines := popByte("uint8 _v")
		assignment := field + " = " + zeroExtendLow("_v") + ";  /* x=1 zeros high byte (hw contract) */"
		if accumulator {
			assignment = field + " = " + preserveHigh(field, "_v") + ";"
		}
		lines = append(lines, assignment)
		lines = append(lines, setNZNoP("_v", 1)...)
		_ = indent
		return lines
	}
	emit16 := func() []string { return append(popWord(field), setNZNoP(field, 2)...) }
	if static != nil {
		body, delta := emit16(), 2
		if *static&1 != 0 {
			body, delta = emit8(""), 1
		}
		lines := []string{"{ uint16 _old_s = cpu->S;"}
		lines = appendIndented(lines, body, "  ")
		return append(lines, fmt.Sprintf("  cpu_trace_stack_op(cpu, 0, %s, _old_s, +%d);", opID, delta), "  "+pSync+" }")
	}
	flag := "x"
	if accumulator {
		flag = "m"
	}
	lines := []string{"{ uint16 _old_s = cpu->S;", fmt.Sprintf("  if (cpu->%s_flag) {", flag)}
	lines = appendIndented(lines, emit8(""), "    ")
	lines = append(lines, fmt.Sprintf("    cpu_trace_stack_op(cpu, 0, %s, _old_s, +1);", opID), "  } else {")
	lines = appendIndented(lines, emit16(), "    ")
	return append(lines, fmt.Sprintf("    cpu_trace_stack_op(cpu, 0, %s, _old_s, +2);", opID), "  }", "  "+pSync+" }")
}

func emitTransfer(op ir.Transfer) []string {
	source, destination := regName(op.Source), regName(op.Destination)
	if op.Destination == ir.S {
		return []string{"{ uint16 _old_s = cpu->S;", "  " + destination + " = " + source + ";", "  /* trace_event uses extra0/extra1 for old/new S high bytes */", "  cpu_trace_event(cpu, 0, CPU_TR_DB_WRITE,", "                  (uint8)(_old_s >> 8), cpu->S); }"}
	}
	flag := ""
	if op.Destination == ir.A && op.Source != ir.D && op.Source != ir.S {
		flag = "cpu->m_flag"
	} else if op.Destination == ir.X || op.Destination == ir.Y {
		flag = "cpu->x_flag"
	}
	if flag == "" {
		return append([]string{destination + " = " + source + ";"}, setNZ(destination, 2)...)
	}
	assignment := destination + " = " + preserveHigh(destination, "_v") + ";"
	if op.Destination == ir.X || op.Destination == ir.Y {
		assignment = destination + " = " + zeroExtendLow("_v") + ";  /* x=1 zeros high byte (hw contract) */"
	}
	lines := []string{fmt.Sprintf("if (%s) {", flag), "  uint8 _v = " + lowByte(source) + ";", "  " + assignment}
	lines = appendIndented(lines, setNZNoP("_v", 1), "  ")
	lines = append(lines, "} else {", "  "+destination+" = (uint16)("+source+");")
	lines = appendIndented(lines, setNZNoP(destination, 2), "  ")
	return append(lines, "}", "cpu->P = (uint8)((cpu->P & ~0x82) | (cpu->_flag_Z ? 0x02 : 0) | (cpu->_flag_N ? 0x80 : 0));")
}

func emitCall(context *Context, op ir.Call) []string {
	if op.Indirect {
		if op.SourcePC != nil && op.TableBase != nil {
			bank := byte(*op.SourcePC >> 16)
			return []string{
				fmt.Sprintf("  ar_indirect_suppressed_log(cpu, 0x%06xu, 0x%02xu, 0x%04xu, cpu->X);", *op.SourcePC&0xffffff, bank, *op.TableBase),
				fmt.Sprintf("/* Call indirect SUPPRESSED: JSR ($%04X,X) at $%06X — cfg-required-dispatch-or-kill, no indirect_call_table authorisation */", *op.TableBase, *op.SourcePC&0xffffff),
			}
		}
		return []string{"/* Call indirect SUPPRESSED — caller dispatches */"}
	}
	if op.Target == nil {
		return []string{"/* Call: target unknown — caller dispatches */"}
	}
	address := *op.Target & 0xffffff
	if invalidLoROMTarget(context, address) {
		context.Rejected[address] = struct{}{}
		return []string{fmt.Sprintf("/* Call: target $%06X not a valid LoROM code address and no cfg name — skipped (decoder followed garbage operand past an RTS) */", address)}
	}
	baseName := context.Names[address]
	if baseName == "" {
		baseName = fmt.Sprintf("bank_%02X_%04X", byte(address>>16), uint16(address))
	}
	for _, pair := range context.variantsAt(address) {
		context.Demands[Variant{address, pair[0], pair[1]}] = struct{}{}
	}
	if pinned, found := context.ForceVariantAt[context.CurrentSite&0xffffff]; found {
		name := fmt.Sprintf("%s_M%dX%d", baseName, pinned[0]&1, pinned[1]&1)
		lines := []string{"{"}
		if op.Long {
			lines = append(lines, "  uint8 _saved_pb = cpu->PB;", fmt.Sprintf("  cpu_trace_pb_change(cpu, 0, _saved_pb, 0x%02x, CPU_TR_JSL);", byte(address>>16)), fmt.Sprintf("  cpu->PB = 0x%02x;", byte(address>>16)))
		}
		lines = append(lines, fmt.Sprintf("  RecompReturn _r = %s(cpu);  /* cfg force_variant_at $%06X -> M%dX%d */", name, context.CurrentSite&0xffffff, pinned[0]&1, pinned[1]&1))
		if op.Long {
			lines = append(lines, "  cpu_trace_pb_change(cpu, 0, cpu->PB, _saved_pb, CPU_TR_RTL);", "  cpu->PB = _saved_pb;")
		}
		return append(lines, "  if (_r != RECOMP_RETURN_NORMAL) {", "    cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_r, 0);", "    cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);", "    return (_r == RECOMP_RETURN_TAILCALL ? _r : (RecompReturn)((int)_r - 1));", "  }", "}")
	}
	lines := []string{"{", "  uint16 _call_s = cpu->S;"}
	lines = append(lines, emitReturnFramePush(op)...)
	if op.Long {
		lines = append(lines, "  uint8 _saved_pb = cpu->PB;", fmt.Sprintf("  cpu_trace_pb_change(cpu, 0, _saved_pb, 0x%02x, CPU_TR_JSL);", byte(address>>16)), fmt.Sprintf("  cpu->PB = 0x%02x;", byte(address>>16)))
	}
	lines = append(lines, fmt.Sprintf("  ar_call_mx_check(cpu, %d, %d, \"%s\", 0x%06xu);", op.EntryM&1, op.EntryX&1, context.CurrentName, context.CurrentSite&0xffffff), "  RecompReturn _r;", "  switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {")
	lines = append(lines, VariantDispatchCases(context, address, baseName, "    ", "")...)
	lines = append(lines, "  }")
	if op.Long {
		lines = append(lines, "  cpu_trace_pb_change(cpu, 0, cpu->PB, _saved_pb, CPU_TR_RTL);", "  cpu->PB = _saved_pb;")
	}
	return append(lines, "  if (_r != RECOMP_RETURN_NORMAL) {", "    cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_r, 0);", "    cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);", "    return (_r == RECOMP_RETURN_TAILCALL ? _r : (RecompReturn)((int)_r - 1));", "  }", "  cpu->S = _call_s;  /* stack-neutrality restore (see _call_s above) */", "}")
}

func invalidLoROMTarget(context *Context, address uint32) bool {
	if _, named := context.Names[address]; named {
		return false
	}
	pc := uint16(address)
	if pc < 0x8000 {
		return true
	}
	if context.ROMSize <= 0 {
		return false
	}
	offset := int(byte(address>>16)&0x7f)*0x8000 + int(pc-0x8000)
	return offset >= context.ROMSize
}

func emitReturnFramePush(op ir.Call) []string {
	var site uint32
	known := op.SourcePC != nil
	if known {
		site = *op.SourcePC & 0xffffff
	}
	if op.Long {
		returnAddress, bank := uint16(0xffff), byte(0xff)
		if known {
			returnAddress, bank = uint16(site+3), byte(site>>16)
		}
		return []string{"  /* JSL return frame -> cpu->S (Option-1) */", fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", bank), fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", byte(returnAddress>>8)), fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", byte(returnAddress)), "  cpu->host_return_valid = 1;  /* paired host caller */"}
	}
	returnAddress := uint16(0xffff)
	if known {
		returnAddress = uint16(site + 2)
	}
	return []string{"  /* JSR return frame -> cpu->S (Option-1) */", fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", byte(returnAddress>>8)), fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", byte(returnAddress)), "  cpu->host_return_valid = 1;  /* paired host caller */"}
}

func emitReturn(context *Context, op ir.Return) []string {
	if op.Interrupt {
		return []string{"cpu_trace_event(cpu, 0, CPU_TR_RTI, 0, 0);", "{ cpu->S = (uint16)(cpu->S + 1); cpu->P = cpu_read8(cpu, 0x00, cpu->S); cpu_p_to_mirrors(cpu);", "  cpu->S = (uint16)(cpu->S + 2);  /* pull + discard PC */", "  if (!cpu->emulation) cpu->S = (uint16)(cpu->S + 1);  /* native: pull + discard PB */", "  cpu_trace_px_record(cpu, 0, 3 /*RTI*/, cpu->P, cpu->P);", "  return RECOMP_RETURN_NORMAL; /* RTI: popped interrupt frame */ }"}
	}
	label, frameSize := "RTS", 2
	if op.Long {
		label, frameSize = "RTL", 3
	}
	source := uint32(0)
	if op.SourcePC != nil {
		source = *op.SourcePC & 0xffffff
	}
	lines := []string{fmt.Sprintf("{ uint16 _ret_s = cpu->S;  /* %s pop hardware return frame */", label), "  cpu->S = (uint16)(cpu->S + 1);", "  uint16 _rpcl = (uint16)cpu_read8(cpu, 0x00, cpu->S);", "  cpu->S = (uint16)(cpu->S + 1);", "  uint16 _rpch = (uint16)cpu_read8(cpu, 0x00, cpu->S);"}
	if op.Long {
		lines = append(lines, "  cpu->S = (uint16)(cpu->S + 1);", "  uint8 _rpb = cpu_read8(cpu, 0x00, cpu->S);")
	} else {
		lines = append(lines, "  uint8 _rpb = cpu->PB;")
	}
	lines = append(lines,
		"  uint32 _rpc = (uint32)((((_rpch << 8) | _rpcl) + 1) & 0xFFFFu);",
		"  uint32 _rpc24 = ((uint32)_rpb << 16) | _rpc;",
		"#if SNESRECOMP_TRACE",
		fmt.Sprintf("  dbg_rts_trace(cpu, 0x%06xu, _entry_s, _ret_s, _rpc24, (uint8)_hrv);", source),
		"#endif",
		"  if (_hrv && _ret_s == _entry_s) {",
	)
	if context.CurrentExitM != nil && context.CurrentExitX != nil {
		lines = append(lines, fmt.Sprintf("    ar_exit_mx_check(cpu, %d, %d, \"%s\", 0x%06xu);", *context.CurrentExitM&1, *context.CurrentExitX&1, context.CurrentName, source))
	}
	lines = append(lines,
		fmt.Sprintf("    return RECOMP_RETURN_NORMAL;  /* %s host return */ }", label),
		"  if (_ret_s != _entry_s && cpu_resolve_ancestor_skip(_ret_s) >= 0) {",
		"    cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);",
		"    if (cpu_dispatch_has_entry(cpu, _rpc24)) {",
		fmt.Sprintf("      cpu_tailcall_request(_rpc24, (uint16)(_ret_s + %du), 0x%06xu);", frameSize, source),
		fmt.Sprintf("      return RECOMP_RETURN_TAILCALL;  /* %s yield: flat tail-dispatch to grandparent continuation */ }", label),
		"    {",
		"      int _anc_skip = cpu_resolve_ancestor_skip(_ret_s);",
		fmt.Sprintf("      return (RecompReturn)_anc_skip;  /* %s return-to-ancestor */ }", label),
		"  }",
		"  cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);",
		fmt.Sprintf("  uint16 _miss_s = (uint16)(((_ret_s > _entry_s) ? _ret_s : _entry_s) + %du);", frameSize),
		fmt.Sprintf("  ar_exit_s_check(cpu, _entry_s, _ret_s, \"%s\", 0x%06xu);", context.CurrentName, source),
		"  if (!_hrv) {",
		fmt.Sprintf("    cpu_tailcall_request(_rpc24, _miss_s, 0x%06xu);", source),
		fmt.Sprintf("    return RECOMP_RETURN_TAILCALL;  /* %s tail-dispatch (trampolined) */ }", label),
		fmt.Sprintf("  return cpu_dispatch_pc_from(cpu, _rpc24, _miss_s, 0x%06xu);  /* %s dispatch (drive) */ }", source, label),
	)
	return lines
}

func emitPushEffective(op ir.PushEffectiveAddress) []string {
	if op.Seg.Kind == ir.AbsoluteBank {
		return []string{"{ uint16 _old_s = cpu->S;", "  cpu->S = (uint16)(cpu->S - 1);", fmt.Sprintf("  cpu_write16(cpu, 0x00, cpu->S, (uint16)0x%04x);", op.Seg.Offset), "  cpu->S = (uint16)(cpu->S - 1);", "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PEA, _old_s, -2); }"}
	}
	if op.Seg.Kind == ir.DPIndirect {
		return []string{"{ uint16 _old_s = cpu->S;", fmt.Sprintf("  uint16 _peival = cpu_read16(cpu, 0x00, (uint16)(cpu->D + 0x%04x));", op.Seg.Offset), "  cpu->S = (uint16)(cpu->S - 1);", "  cpu_write16(cpu, 0x00, cpu->S, _peival);", "  cpu->S = (uint16)(cpu->S - 1);", "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PEI, _old_s, -2); }"}
	}
	return []string{"/* TODO PushEffectiveAddress unsupported kind */"}
}

func emitBlockMove(op ir.BlockMove) []string {
	delta, trace := "+1", "CPU_TR_MVN"
	if op.Direction == "mvp" {
		delta, trace = "-1", "CPU_TR_MVP"
	}
	return []string{"{", fmt.Sprintf("  uint8 _src_b = 0x%02x;", op.SourceBank), fmt.Sprintf("  uint8 _dst_b = 0x%02x;", op.DestinationBank), "  uint8 _old_db = cpu->DB;", fmt.Sprintf("  cpu_trace_event(cpu, 0, %s, _src_b, _dst_b);", trace), "  while (cpu->A != 0xFFFF) {", "    uint8 _b = cpu_read8(cpu, _src_b, cpu->X);", "    cpu_write8(cpu, _dst_b, cpu->Y, _b);", fmt.Sprintf("    cpu->X = (uint16)(cpu->X %s);", delta), fmt.Sprintf("    cpu->Y = (uint16)(cpu->Y %s);", delta), "    cpu->A = (uint16)(cpu->A - 1);", "  }", "  cpu->DB = _dst_b;", fmt.Sprintf("  cpu_trace_db_change(cpu, 0, _old_db, _dst_b, %s);", trace), "}"}
}
