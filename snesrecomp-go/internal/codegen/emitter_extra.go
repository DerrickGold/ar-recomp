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

func emitCall(context *Context, op ir.Call) ([]string, error) {
	if op.Indirect {
		return emitIndirectCall(op)
	}
	if op.Target == nil {
		return []string{"/* Call: target unknown — caller dispatches */"}, nil
	}
	address := *op.Target & 0xffffff
	if invalidLoROMTarget(context, address) {
		context.Rejected[address] = struct{}{}
		name := fmt.Sprintf("bank_%02X_%04X", byte(address>>16),
			uint16(address))
		return []string{fmt.Sprintf(
			"(void)cpu_trace_unresolved_stub_trap(cpu, 0x%06x, \"%s\"); /* direct call target is outside the static LoROM code domain */",
			address, name)}, nil
	}
	baseName := context.Names[address]
	if baseName == "" {
		baseName = fmt.Sprintf("bank_%02X_%04X", byte(address>>16), uint16(address))
	}
	pinned, forcePinned := context.ForceVariantAt[context.CurrentSite&0xffffff]
	if context.ExactDirectCallMX {
		requested := [2]uint8{op.EntryM & 1, op.EntryX & 1}
		if forcePinned {
			requested = [2]uint8{pinned[0] & 1, pinned[1] & 1}
		}
		survivors := context.variantsAt(address)
		target := requested
		found := false
		for _, survivor := range survivors {
			if survivor == requested {
				found = true
				break
			}
		}
		if !found {
			var proven bool
			target, proven = context.provenVariantRoute(address, survivors, requested[0], requested[1])
			if !proven {
				return nil, fmt.Errorf(
					"exact direct call at $%06X requires $%06X M%dX%d, but that body was pruned without a variant-equivalence proof",
					context.CurrentSite&0xffffff, address, requested[0], requested[1])
			}
		}
		context.Demands[Variant{address, target[0], target[1]}] = struct{}{}
	} else {
		for _, pair := range context.variantsAt(address) {
			context.Demands[Variant{address, pair[0], pair[1]}] = struct{}{}
		}
	}
	lines := []string{"{", "  uint16 _call_s = cpu->S;"}
	lines = append(lines, emitReturnFramePush(op)...)
	if op.SourcePC != nil {
		frameBytes := 2
		continuation := fmt.Sprintf("(((uint32)cpu->PB << 16) | 0x%04xu)", uint16(*op.SourcePC+3))
		if op.Long {
			frameBytes = 3
			continuation = fmt.Sprintf("(((uint32)cpu->PB << 16) | 0x%04xu)", uint16(*op.SourcePC+4))
		}
		lines = append(lines, "  CpuReturnScope _call_owner;",
			fmt.Sprintf("  cpu_return_scope_begin(&_call_owner, cpu, %s, _entry_s, %du);", continuation, frameBytes))
	}
	if op.Long {
		lines = append(lines, "  uint8 _saved_pb = cpu->PB;", fmt.Sprintf("  cpu_trace_pb_change(cpu, 0, _saved_pb, 0x%02x, CPU_TR_JSL);", byte(address>>16)), fmt.Sprintf("  cpu->PB = 0x%02x;", byte(address>>16)))
	}
	expectedM, expectedX := op.EntryM&1, op.EntryX&1
	if forcePinned {
		expectedM, expectedX = pinned[0]&1, pinned[1]&1
	}
	lines = append(lines, fmt.Sprintf("  sr_call_mx_check(cpu, %d, %d, \"%s\", 0x%06xu);", expectedM, expectedX, context.CurrentName, context.CurrentSite&0xffffff))
	if forcePinned {
		name := fmt.Sprintf("%s_M%dX%d", baseName, pinned[0]&1, pinned[1]&1)
		lines = append(lines, fmt.Sprintf("  RecompReturn _r = %s(cpu);  /* cfg force_variant_at $%06X -> M%dX%d */", name, context.CurrentSite&0xffffff, pinned[0]&1, pinned[1]&1))
	} else if context.ExactDirectCallMX {
		survivors := context.variantsAt(address)
		target := [2]uint8{op.EntryM & 1, op.EntryX & 1}
		found := false
		for _, survivor := range survivors {
			if survivor == target {
				found = true
				break
			}
		}
		if !found {
			var proven bool
			target, proven = context.provenVariantRoute(address, survivors, target[0], target[1])
			if !proven {
				return nil, fmt.Errorf(
					"exact direct call at $%06X requires $%06X M%dX%d, but that body was pruned without a variant-equivalence proof",
					context.CurrentSite&0xffffff, address, op.EntryM&1, op.EntryX&1)
			}
		}
		lines = append(lines, fmt.Sprintf("  RecompReturn _r = %s_M%dX%d(cpu);  /* exact live M/X direct call */", baseName, target[0], target[1]))
	} else {
		lines = append(lines, "  RecompReturn _r;", "  switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {")
		lines = append(lines, VariantDispatchCases(context, address, baseName, "    ", "")...)
		lines = append(lines, "  }")
	}
	return finishCall(lines, op), nil
}

func emitIndirectCall(op ir.Call) ([]string, error) {
	if op.SourcePC == nil || op.TableBase == nil || op.Long {
		return nil, fmt.Errorf("indirect call requires a native JSR (abs,X) source and table operand")
	}
	site := *op.SourcePC & 0xffffff
	lines := []string{"{ /* native indirect JSR: live PB/X target and M/X registry */", "  uint16 _call_s = cpu->S;"}
	lines = append(lines, emitReturnFramePush(op)...)
	lines = append(lines, "  CpuReturnScope _call_owner;",
		fmt.Sprintf("  cpu_return_scope_begin(&_call_owner, cpu, (((uint32)cpu->PB << 16) | 0x%04xu), _entry_s, 2u);", uint16(site+3)),
		fmt.Sprintf("  uint16 _indirect_pointer = (uint16)(0x%04xu + cpu->X);", *op.TableBase),
		"  uint32 _indirect_target = ((uint32)cpu->PB << 16) | cpu_read16_bank_wrap(cpu, cpu->PB, _indirect_pointer);",
		"  if (!cpu_dispatch_has_entry(cpu, _indirect_target)) {",
		"    cpu_return_scope_end(&_call_owner);",
		fmt.Sprintf("    cpu_trace_trapped_dispatch(cpu, _indirect_target, 0x%06xu);", site),
		fmt.Sprintf("    return cpu_trace_unresolved_goto_trap(cpu, 0x%06xu, _indirect_target, \"unresolved indirect JSR target\", \"AOT call registry\");", site),
		"  }",
		"#if SNESRECOMP_SEMANTIC_DISPATCH_TRACE",
		fmt.Sprintf("  cpu_trace_resolved_dispatch(cpu, _indirect_target, 0x%06xu);", site),
		"#endif",
		fmt.Sprintf("  RecompReturn _r = cpu_dispatch_paired_tail_from(cpu, _indirect_target, cpu->S, 1u, 0x%06xu);", site))
	return finishCall(lines, op), nil
}

// Both static and dynamic calls own one native call boundary. The callee
// cannot make a parked/non-local return into an ordinary successful call.
func finishCall(lines []string, op ir.Call) []string {
	lines = append(lines, "  if (_r == RECOMP_RETURN_PARKED_WAIT) {")
	if op.SourcePC != nil {
		lines = append(lines, "    cpu_return_scope_end(&_call_owner);")
	}
	lines = append(lines, "    RecompStackPop(); return _r; /* preserve native parked CPU, including PB/S */", "  }")
	lines = append(lines, "  if (_r == RECOMP_RETURN_OWNED_UNWIND) {")
	if op.SourcePC != nil {
		lines = append(lines,
			"    if (!cpu_finish_owned_unwind(&_call_owner, cpu)) {",
			"      cpu_return_scope_end(&_call_owner);",
			"      return _r; /* discard this activation without restoring native S/PB */",
			"    }",
			"    _r = RECOMP_RETURN_NORMAL; /* resume this exact call once */")
	} else {
		lines = append(lines, "    return _r; /* no owned continuation at this synthetic call */")
	}
	lines = append(lines, "  }")
	if op.Long {
		lines = append(lines, "  cpu_trace_pb_change(cpu, 0, cpu->PB, _saved_pb, CPU_TR_RTL);", "  cpu->PB = _saved_pb;")
	}
	if op.SourcePC != nil {
		lines = append(lines, "  cpu_return_scope_end(&_call_owner);  /* also unwind ownership on NLR */")
	}
	lines = append(lines, "  if (_r != RECOMP_RETURN_NORMAL) {", "    cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_r, 0);", "    cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);", "    return (_r == RECOMP_RETURN_TAILCALL ? _r : (RecompReturn)((int)_r - 1));", "  }")
	if op.SourcePC != nil {
		lines = append(lines, "  if (!_call_owner.adjusted_return) /* native callee cleanup keeps its actual post-return S */")
	}
	return append(lines, "  cpu->S = _call_s;  /* stack-neutrality restore (see _call_s above) */", "}")
}

func invalidLoROMTarget(context *Context, address uint32) bool {
	if _, named := context.Names[address]; named {
		return false
	}
	offset, err := context.ROMMapper.Offset(byte(address>>16), uint16(address))
	if err != nil {
		return true
	}
	if context.ROMSize <= 0 {
		return false
	}
	if context.ROMMapper.String() == "hirom" && context.ROMSize&(context.ROMSize-1) == 0 {
		offset &= context.ROMSize - 1
	}
	return offset >= context.ROMSize
}

func emitReturnFramePush(op ir.Call) []string {
	var site uint32
	known := op.SourcePC != nil
	if known {
		site = *op.SourcePC & 0xffffff
	}
	if op.Long {
		returnAddress := uint16(0xffff)
		if known {
			returnAddress = uint16(site + 3)
		}
		return []string{"  /* JSL return frame -> cpu->S (Option-1) */", "  cpu_write8(cpu, 0x00, cpu->S, cpu->PB); cpu->S = (uint16)(cpu->S - 1);", fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", byte(returnAddress>>8)), fmt.Sprintf("  cpu_write8(cpu, 0x00, cpu->S, 0x%02x); cpu->S = (uint16)(cpu->S - 1);", byte(returnAddress)), "  cpu->host_return_valid = 1;  /* paired host caller */"}
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
		"#if SNESRECOMP_SEMANTIC_DISPATCH_TRACE",
		fmt.Sprintf("  cpu_trace_resolved_dispatch(cpu, _rpc24, 0x%06xu);", source),
		"#endif",
		"#if SNESRECOMP_TRACE",
		fmt.Sprintf("  dbg_rts_trace(cpu, 0x%06xu, _entry_s, _ret_s, _rpc24, (uint8)_hrv);", source),
		"#endif",
		fmt.Sprintf("  if (g_cpu_return_scope && _ret_s > g_cpu_return_scope->entry_stack && cpu_begin_owned_unwind(cpu, _ret_s, _rpc24, %du)) {", frameSize),
		"    return RECOMP_RETURN_OWNED_UNWIND;",
		"  }",
		"  if (_hrv && _ret_s == _entry_s) {",
	)
	if context.CurrentExitM != nil && context.CurrentExitX != nil {
		lines = append(lines, fmt.Sprintf("    sr_exit_mx_check(cpu, %d, %d, \"%s\", 0x%06xu);", *context.CurrentExitM&1, *context.CurrentExitX&1, context.CurrentName, source))
	}
	lines = append(lines,
		fmt.Sprintf("    return RECOMP_RETURN_NORMAL;  /* %s host return */ }", label),
		fmt.Sprintf("  if (_hrv && cpu_accept_adjusted_return(cpu, _entry_s, _ret_s, _rpc24, %du)) {", frameSize),
	)
	if context.CurrentExitM != nil && context.CurrentExitX != nil {
		lines = append(lines, fmt.Sprintf("    sr_exit_mx_check(cpu, %d, %d, \"%s\", 0x%06xu);", *context.CurrentExitM&1, *context.CurrentExitX&1, context.CurrentName, source))
	}
	lines = append(lines,
		fmt.Sprintf("    return RECOMP_RETURN_NORMAL;  /* %s owned callee-clean return; retain native S */ }", label),
	)
	if op.OwnFrameWord && !op.Long {
		lines = append(lines,
			"  if (_hrv && cpu_accept_return_word_relocation(cpu, _entry_s, _ret_s, _rpc24, _return_origin)) {",
		)
		if context.CurrentExitM != nil && context.CurrentExitX != nil {
			lines = append(lines, fmt.Sprintf("    sr_exit_mx_check(cpu, %d, %d, \"%s\", 0x%06xu);", *context.CurrentExitM&1, *context.CurrentExitX&1, context.CurrentName, source))
		}
		lines = append(lines, "    return RECOMP_RETURN_NORMAL; /* witnessed own-frame word relocation; retain native S */ }")
	}
	lines = append(lines,
		"  if (_hrv && !cpu->emulation && _ret_s < _entry_s && cpu->S <= _entry_s) {",
		"#if SNESRECOMP_TRACE",
		fmt.Sprintf("    cpu_trace_missing_pushed_target(cpu, _rpc24, 0x%06xu);", source),
		"#endif",
		"    cpu->PB = _rpb;",
		fmt.Sprintf("    return cpu_dispatch_paired_tail_from(cpu, _rpc24, _entry_s, _hrv, 0x%06xu); /* pushed target preserves active call ownership */", source),
		"  }",
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
		fmt.Sprintf("  sr_exit_s_check(cpu, _entry_s, _ret_s, \"%s\", 0x%06xu);", context.CurrentName, source),
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
