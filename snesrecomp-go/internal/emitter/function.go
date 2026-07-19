// Package emitter connects decoding, CFG construction, IR lowering, and C
// code generation for complete recomp functions.
package emitter

import (
	"fmt"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/cfg"
	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/ir"
	"github.com/DerrickGold/snesrecomp-go/internal/lowering"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

type FunctionOptions struct {
	Name              string
	End               *uint16
	EntrySOffset      int
	Decode            decoder.Options
	Codegen           *codegen.Context
	ExcludeRanges     [][2]uint16
	TailCallPC        *uint16
	TailCallTarget    string
	HLESPCUpload      bool
	HLEFunction       string
	HLEDispatch       map[uint16]string
	ExitMX            *decoder.MX
	UnresolvedAllowed bool
}

type FunctionResult struct {
	Source                    string
	Graph                     *decoder.Graph
	CFG                       *cfg.Graph
	GarbageBRK                *uint16
	GarbageEvidence           []GarbageEvidence
	Equivalences              []VariantEquivalence
	SuppressedIndirectCalls   []decoder.SuppressedIndirectCall
	ConstantZFolds            []decoder.ConstantZFold
	DispatchTargetsSuppressed []decoder.DispatchTargetSuppressed
	UnresolvedIndirects       []decoder.UnresolvedIndirect
}

// VariantEquivalence records that every instruction shape reached by From is
// also present at the same PC in To. The regen driver accumulates these facts
// across emit passes and uses them to route pruned runtime M/X cases.
type VariantEquivalence struct {
	Address  uint32
	From, To [2]uint8
}

// GarbageEvidence says a BRK in the current variant lands strictly inside an
// instruction decoded by Sibling. Regen keeps all such evidence so it can
// solve the survivor/prune decision without using a sibling that is itself
// being discarded as speculative garbage.
type GarbageEvidence struct {
	Sibling [2]uint8
	BRKPC   uint16
}

type instructionShape struct {
	Mnemonic string
	Length   uint8
}

var allMXVariants = [][2]uint8{{0, 0}, {0, 1}, {1, 0}, {1, 1}}

func validVariantsAt(valid map[uint32]map[[2]uint8]struct{}, address uint32) map[[2]uint8]struct{} {
	address &= 0xffffff
	result := valid[address]
	if len(result) == 0 {
		bank := byte(address >> 16)
		if bank < 0x40 || (bank >= 0x80 && bank < 0xc0) {
			result = valid[address^0x800000]
		}
	}
	if len(result) != 0 {
		return result
	}
	result = make(map[[2]uint8]struct{}, len(allMXVariants))
	for _, pair := range allMXVariants {
		result[pair] = struct{}{}
	}
	return result
}

// findGarbageEvidence collects the raw sibling relationships used by the
// split-immediate check. The regen solver decides which siblings survive;
// detectGarbageVariant emits the diagnostic only after that map exists.
func findGarbageEvidence(image rom.Image, bank byte, start uint16, entryM, entryX uint8, graph *decoder.Graph, end *uint16) []GarbageEvidence {
	if graph == nil {
		return nil
	}
	var brks []uint16
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		if decoded.Instruction.Mnemonic == "BRK" {
			brks = append(brks, uint16(decoded.Key.PC))
		}
	}
	if len(brks) == 0 {
		return nil
	}
	var result []GarbageEvidence
	for _, pair := range [][2]uint8{{entryM ^ 1, entryX}, {entryM, entryX ^ 1}} {
		pair[0], pair[1] = pair[0]&1, pair[1]&1
		sibling, err := decoder.DecodeFunction(image, bank, start, pair[0], pair[1], decoder.Options{End: end})
		if err != nil {
			continue
		}
		for _, key := range sibling.Order {
			other := sibling.Instructions[key]
			q := uint16(other.Key.PC)
			length := uint16(other.Instruction.Length)
			for _, pc := range brks {
				if q < pc && pc < q+length {
					result = append(result, GarbageEvidence{Sibling: pair, BRKPC: pc})
					break
				}
			}
		}
	}
	return result
}

func detectGarbageVariant(address uint32, evidence []GarbageEvidence, valid map[uint32]map[[2]uint8]struct{}) *uint16 {
	if len(valid) == 0 {
		return nil
	}
	survivors := validVariantsAt(valid, address)
	for _, item := range evidence {
		if _, found := survivors[item.Sibling]; found {
			pc := item.BRKPC
			return &pc
		}
	}
	return nil
}

// findEquivalentVariants implements the legacy emitter's one-way coverage
// proof: every (PC, mnemonic, length) in this decode must appear identically
// in the sibling. It deliberately compares all raw siblings before pruning.
func findEquivalentVariants(image rom.Image, bank byte, start uint16, entryM, entryX uint8, graph *decoder.Graph, end *uint16) []VariantEquivalence {
	if graph == nil {
		return nil
	}
	shape := make(map[uint16]instructionShape)
	for _, key := range graph.Order {
		decoded := graph.Instructions[key]
		shape[uint16(decoded.Key.PC)] = instructionShape{decoded.Instruction.Mnemonic, decoded.Instruction.Length}
	}
	if len(shape) < 2 {
		return nil
	}
	from := [2]uint8{entryM & 1, entryX & 1}
	address := decoder.Address24(bank, start)
	var result []VariantEquivalence
	for _, pair := range allMXVariants {
		if pair == from {
			continue
		}
		sibling, err := decoder.DecodeFunction(image, bank, start, pair[0], pair[1], decoder.Options{End: end})
		if err != nil {
			continue
		}
		siblingShape := make(map[uint16]instructionShape)
		for _, key := range sibling.Order {
			decoded := sibling.Instructions[key]
			siblingShape[uint16(decoded.Key.PC)] = instructionShape{decoded.Instruction.Mnemonic, decoded.Instruction.Length}
		}
		covered := true
		for pc, current := range shape {
			if siblingShape[pc] != current {
				covered = false
				break
			}
		}
		if covered {
			result = append(result, VariantEquivalence{Address: address, From: from, To: pair})
		}
	}
	return result
}

type instructionIR struct {
	decoded *decoder.DecodedInstruction
	ops     []ir.Op
}

func EmitFunction(image rom.Image, bank byte, start uint16, entryM, entryX uint8, options FunctionOptions) (*FunctionResult, error) {
	baseName := options.Name
	if baseName == "" {
		baseName = fmt.Sprintf("bank_%02X_%04X", bank, start)
	}
	name := fmt.Sprintf("%s_M%dX%d", baseName, entryM&1, entryX&1)
	entryPC := decoder.Address24(bank, start)
	if options.HLESPCUpload {
		return &FunctionResult{Source: emitSPCStub(name, baseName, entryPC)}, nil
	}
	decodeOptions := options.Decode
	decodeOptions.End = options.End
	graph, err := decoder.DecodeFunction(image, bank, start, entryM, entryX, decodeOptions)
	if err != nil {
		if options.HLEFunction != "" {
			return &FunctionResult{Source: emitHLEStub(name, options.HLEFunction, entryPC)}, nil
		}
		return nil, err
	}
	controlFlow := cfg.Build(graph)
	garbageEvidence := findGarbageEvidence(image, bank, start, entryM, entryX, graph, options.End)
	result := &FunctionResult{
		Graph: graph, CFG: controlFlow,
		GarbageEvidence:           garbageEvidence,
		Equivalences:              findEquivalentVariants(image, bank, start, entryM, entryX, graph, options.End),
		SuppressedIndirectCalls:   append([]decoder.SuppressedIndirectCall(nil), graph.SuppressedIndirectCalls...),
		ConstantZFolds:            append([]decoder.ConstantZFold(nil), graph.ConstantZFolds...),
		DispatchTargetsSuppressed: append([]decoder.DispatchTargetSuppressed(nil), graph.DispatchTargetsSuppressed...),
		UnresolvedIndirects:       append([]decoder.UnresolvedIndirect(nil), graph.UnresolvedIndirects...),
	}
	if options.Codegen != nil {
		result.GarbageBRK = detectGarbageVariant(entryPC, garbageEvidence, options.Codegen.ValidVariants)
	}
	if len(result.UnresolvedIndirects) > 0 && !options.UnresolvedAllowed {
		return result, fmt.Errorf("%s has %d unresolved indirect control-flow sites", name, len(result.UnresolvedIndirects))
	}

	order := depthFirstOrder(controlFlow)
	local := make(map[decoder.DecodeKey]struct{}, len(order))
	for _, key := range order {
		local[key] = struct{}{}
	}
	nextValue := 0
	valueFactory := func() ir.Value {
		nextValue++
		return ir.Value{ID: nextValue}
	}
	lowered := make(map[decoder.DecodeKey][]instructionIR, len(order))
	for _, key := range order {
		for _, instruction := range controlFlow.Blocks[key].Instructions {
			lowered[key] = append(lowered[key], instructionIR{instruction, lowering.Lower(instruction.Instruction, valueFactory)})
		}
	}

	context := options.Codegen
	if context == nil {
		context = codegen.NewContext()
	}
	context.CurrentName = name
	if options.ExitMX != nil && options.ExitMX.M >= 0 && options.ExitMX.X >= 0 {
		m, x := uint8(options.ExitMX.M)&1, uint8(options.ExitMX.X)&1
		context.CurrentExitM, context.CurrentExitX = &m, &x
	} else {
		context.CurrentExitM, context.CurrentExitX = nil, nil
	}
	blockLines := make(map[decoder.DecodeKey][]string, len(order))
	for _, key := range order {
		block := controlFlow.Blocks[key]
		var lines []string
		terminated := false
		for _, pair := range lowered[key] {
			instruction := pair.decoded.Instruction
			context.CurrentSite = instruction.Address & 0xffffff
			for _, operation := range pair.ops {
				switch op := operation.(type) {
				case ir.CondBranch:
					fall, taken := successor(block.Successors, 0), successor(block.Successors, 1)
					if taken != nil {
						lines = append(lines, fmt.Sprintf("if (%s == %d) { %s }", flagExpression(op.Flag), op.TakeIf, gotoOrTail(context, name, bank, key, *taken, local, options)))
					}
					if fall != nil {
						lines = append(lines, gotoOrTail(context, name, bank, key, *fall, local, options)+" /* fall-through */")
					}
					terminated = true
				case ir.Goto:
					if len(instruction.DispatchEntries) > 0 {
						lines = append(lines, emitJSLDispatch(context, instruction)...)
					} else if target := successor(block.Successors, 0); target != nil {
						lines = append(lines, gotoOrTail(context, name, bank, key, *target, local, options))
					} else if instruction.Mnemonic == "JMP" && instruction.Mode == cpu65816.LONG {
						targetAddress := instruction.Operand & 0xffffff
						targetName := context.Names[targetAddress]
						if targetName == "" && !validLoROMCodeAddress(image, targetAddress) {
							lines = append(lines, fmt.Sprintf("return RECOMP_RETURN_NORMAL; /* cross-bank JML to $%06X skipped — not a valid LoROM code address (decoder followed garbage operand past an RTS) */", targetAddress))
							terminated = true
							break
						}
						if targetName == "" {
							targetName = fmt.Sprintf("bank_%02X_%04X", byte(targetAddress>>16), uint16(targetAddress))
						}
						context.Demands[codegen.Variant{Address: targetAddress, M: key.M & 1, X: key.X & 1}] = struct{}{}
						suffixName := fmt.Sprintf("%s_M%dX%d", targetName, key.M&1, key.X&1)
						lines = append(lines,
							fmt.Sprintf("cpu->PB = 0x%02X; /* JML into bank $%02X */", byte(targetAddress>>16), byte(targetAddress>>16)),
							tailCallStatement(suffixName+"(cpu)", fmt.Sprintf("/* tail-call cross-bank into %s at $%06X (JML unresolved successor) */", suffixName, targetAddress), nil),
						)
					} else {
						lines = append(lines, fmt.Sprintf("return cpu_trace_unresolved_goto_trap(cpu, 0x%06X, 0x%06X, \"%s\", \"unknown\");", key.PC&0xffffff, instruction.Operand&0xffffff, name))
					}
					terminated = true
				case ir.IndirectGoto:
					if len(instruction.DispatchEntries) > 0 {
						lines = append(lines, emitIndirectDispatch(context, instruction, local)...)
					} else if helper := options.HLEDispatch[uint16(instruction.Address)]; helper != "" {
						lines = append(lines, fmt.Sprintf("{ extern RecompReturn %s(CpuState *cpu); RecompReturn _r = %s(cpu); RecompStackPop(); return _r; } /* hle_dispatch $%06X — host-side dispatcher */", helper, helper, instruction.Address&0xffffff))
					} else {
						lines = append(lines, fmt.Sprintf("return cpu_trace_dispatch_oob(cpu, 0x%06x, 0xFFFF); /* unresolved IndirectGoto — HLE pending */", instruction.Address&0xffffff))
					}
					terminated = true
				case ir.PushReg:
					if len(instruction.DispatchEntries) > 0 {
						lines = append(lines, emitIndirectDispatch(context, instruction, local)...)
						terminated = true
					} else {
						emitted, emitErr := codegen.EmitOperation(context, op)
						if emitErr != nil {
							return result, emitErr
						}
						lines = append(lines, emitted...)
					}
				case ir.Call:
					if len(instruction.DispatchEntries) > 0 {
						if instruction.DispatchIndexReg == "X" || instruction.DispatchIndexReg == "Y" || instruction.DispatchIndexReg == "A" {
							lines = append(lines, emitIndirectDispatch(context, instruction, local)...)
						} else {
							lines = append(lines, emitJSLDispatch(context, instruction)...)
						}
						if instruction.Mnemonic != "JSR" {
							terminated = true
						}
					} else {
						emitted, emitErr := codegen.EmitOperation(context, op)
						if emitErr != nil {
							return result, emitErr
						}
						lines = append(lines, emitted...)
					}
				case ir.Return:
					if instruction.DispatchKind == "rts_trick" {
						lines = append(lines, emitRTSDispatchGuard(instruction, local)...)
					}
					emitted, emitErr := codegen.EmitOperation(context, op)
					if emitErr != nil {
						return result, emitErr
					}
					lines = append(lines, emitted...)
					terminated = true
				default:
					emitted, emitErr := codegen.EmitOperation(context, operation)
					if emitErr != nil {
						return result, emitErr
					}
					lines = append(lines, emitted...)
				}
			}
		}
		if !terminated {
			switch len(block.Successors) {
			case 1:
				lines = append(lines, gotoOrTail(context, name, bank, key, block.Successors[0], local, options)+" /* implicit fall-through */")
			case 0:
				lines = append(lines, "return RECOMP_RETURN_NORMAL; /* no terminator, no successor */")
			default:
				lines = append(lines, gotoOrTail(context, name, bank, key, block.Successors[0], local, options)+" /* conservative multi-successor fall-through */")
			}
		}
		blockLines[key] = lines
	}

	var source []string
	source = append(source,
		fmt.Sprintf("RecompReturn %s(CpuState *cpu) {", name),
		"  extern const char *g_last_recomp_func;",
		fmt.Sprintf("  g_last_recomp_func = \"%s\";", name),
		fmt.Sprintf("  RecompStackPush(\"%s\");", name),
		fmt.Sprintf("  cpu_dbg_funcname(\"%s\");", name),
		fmt.Sprintf("  cpu_trace_func_entry(cpu, 0x%06X, \"%s\");", entryPC, name),
		fmt.Sprintf("  ar_entry_mx_check(cpu, %d, %d, \"%s\", 0x%06X);", entryM&1, entryX&1, name, entryPC),
	)
	if result.GarbageBRK != nil {
		source = append(source, fmt.Sprintf("  ar_garbage_variant_trap(cpu, \"%s\", 0x%06X);  /* split-immediate BRK at $%04X */", name, entryPC, *result.GarbageBRK))
	}
	source = append(source,
		"  RecompReturn _pending_skip = RECOMP_RETURN_NORMAL;",
		"  (void)_pending_skip;  /* unused if no NLR site in this fn */",
	)
	if options.EntrySOffset == 0 {
		source = append(source, "  uint16 _entry_s = cpu->S;")
	} else {
		sign := "+"
		if options.EntrySOffset < 0 {
			sign = "-"
		}
		source = append(source, fmt.Sprintf("  uint16 _entry_s = (uint16)(cpu->S %s %du);  /* entry_s_offset:%d — caller left stack imbalanced */", sign, abs(options.EntrySOffset), options.EntrySOffset))
	}
	source = append(source,
		"  uint8 _hrv = cpu->host_return_valid;",
		"  if (cpu_take_tailcall_return_context(&_entry_s, &_hrv)) {",
		"    cpu->host_return_valid = _hrv;",
		"  }",
		"  (void)_entry_s;  /* used by trampoline balance check */",
		"  (void)_hrv;",
		"  if (g_recomp_stack_top >= 1) { g_cpu_entry_s[g_recomp_stack_top - 1] = _entry_s; g_cpu_entry_hrv[g_recomp_stack_top - 1] = _hrv; }",
	)
	for _, key := range order {
		source = append(source, "  "+label(key)+":", fmt.Sprintf("    cpu_trace_block(cpu, 0x%06X);", key.PC&0xffffff), "    WatchdogCheck();")
		for _, line := range blockLines[key] {
			if strings.HasPrefix(strings.TrimSpace(line), "return") {
				source = append(source, "    RecompStackPop();")
			}
			source = append(source, "    "+line)
		}
	}
	source = append(source, "  RecompStackPop();", "  return RECOMP_RETURN_NORMAL;", "}")
	result.Source = strings.Join(source, "\n") + "\n"
	if options.HLEFunction != "" {
		result.Source = emitHLEStub(name, options.HLEFunction, entryPC)
	}
	return result, nil
}

func depthFirstOrder(graph *cfg.Graph) []decoder.DecodeKey {
	var order []decoder.DecodeKey
	visited := make(map[decoder.DecodeKey]struct{})
	var visit func(decoder.DecodeKey)
	visit = func(key decoder.DecodeKey) {
		if _, found := visited[key]; found || graph.Blocks[key] == nil {
			return
		}
		visited[key] = struct{}{}
		order = append(order, key)
		for _, successor := range graph.Blocks[key].Successors {
			visit(successor)
		}
	}
	visit(graph.Entry)
	for _, key := range graph.Order {
		visit(key)
	}
	return order
}

func label(key decoder.DecodeKey) string {
	return fmt.Sprintf("L_%04X_M%dX%d", uint16(key.PC), key.M&1, key.X&1)
}

func successor(successors []decoder.DecodeKey, index int) *decoder.DecodeKey {
	if index < 0 || index >= len(successors) {
		return nil
	}
	return &successors[index]
}

func gotoOrTail(context *codegen.Context, functionName string, bank byte, source, target decoder.DecodeKey, local map[decoder.DecodeKey]struct{}, options FunctionOptions) string {
	if _, found := local[target]; found {
		return "goto " + label(target) + ";"
	}
	pc := uint16(target.PC)
	for _, excluded := range options.ExcludeRanges {
		if pc >= excluded[0] && pc < excluded[1] {
			return fmt.Sprintf("return RECOMP_RETURN_NORMAL; /* %s HLE-replaced (cfg exclude_range %04X-%04X) */", label(target), excluded[0], excluded[1])
		}
	}
	targetAddress := decoder.Address24(bank, pc)
	targetName := ""
	if options.TailCallPC != nil && *options.TailCallPC == pc {
		targetName = options.TailCallTarget
	}
	if targetName == "" {
		targetName = context.Names[targetAddress]
	}
	if targetName != "" {
		context.Demands[codegen.Variant{Address: targetAddress, M: target.M & 1, X: target.X & 1}] = struct{}{}
		suffixName := fmt.Sprintf("%s_M%dX%d", targetName, target.M&1, target.X&1)
		comment := fmt.Sprintf("/* tail-call past end: into %s at $%04X */", suffixName, pc)
		if options.TailCallPC != nil && *options.TailCallPC == pc {
			comment = fmt.Sprintf("/* tail_call into sibling fn at $%04X (cfg tail_call: directive) */", pc)
		}
		return tailCallStatement(suffixName+"(cpu)", comment, &targetAddress)
	}
	return fmt.Sprintf("return cpu_trace_unresolved_goto_trap(cpu, 0x%06X, 0x%06X, \"%s\", \"%s\");", source.PC&0xffffff, targetAddress, functionName, label(target))
}

// tailCallStatement mirrors the Option-1 cpu->S ABI used by the Python
// emitter. Same-bank tail transfers need a trampoline: a dispatched frame
// yields to the existing driving loop, while a paired root starts a local
// driving loop. Cross-bank JML transfers use a direct C call because the JSL
// wrapper above the chain owns restoring PB.
func tailCallStatement(callExpression, comment string, trampolinePC *uint32) string {
	if trampolinePC == nil {
		return fmt.Sprintf("{ cpu->host_return_valid = _hrv; cpu_tailcall_inherit_return_context(_entry_s, _hrv); RecompReturn _tc = %s; RecompStackPop(); return _tc; }  %s", callExpression, comment)
	}
	target := fmt.Sprintf("0x%06xu", *trampolinePC&0xffffff)
	return fmt.Sprintf("{ if (!_hrv) { cpu->host_return_valid = _hrv; cpu_tailcall_inherit_return_context(_entry_s, _hrv); cpu_tailcall_request(%s, _entry_s, %s); RecompStackPop(); return RECOMP_RETURN_TAILCALL; } RecompStackPop(); return cpu_dispatch_pc_from(cpu, %s, _entry_s, %s); }  %s", target, target, target, target, comment)
}

func validLoROMCodeAddress(image rom.Image, address uint32) bool {
	offset, err := rom.LoROMOffset(byte(address>>16), uint16(address))
	return err == nil && offset >= 0 && offset < len(image)
}

func flagExpression(register ir.Reg) string {
	return map[ir.Reg]string{ir.N: "cpu->_flag_N", ir.V: "cpu->_flag_V", ir.C: "cpu->_flag_C", ir.ZF: "cpu->_flag_Z"}[register]
}

// emitRTSDispatchGuard emits the RTS-trick dispatch switch for a `rts_dispatch`
// site: read the popped target off the stack and `goto` the matching in-function
// label instead of taking a host return.
//
// WIDTH-EXACTNESS GATE (2026-07-19). The `goto` targets are labels named
// L_<pc>_M<m>X<x>, where m/x are the widths THIS INSTRUCTION was DECODED at —
// they are not a function of the runtime flags. Control, however, arrives here
// from a hardware RTS whose runtime m/x can differ from the decode. Jumping into
// a sibling-width decode is silent memory corruption, not a mispredict: the
// target block's PHA/PLA/PHY/PLY are emitted at the decoded width, so an 8-bit
// push gets popped as 16-bit (or vice versa) and the emulated stack shifts by one
// byte. Downstream, the next RTS reads its return address misaligned and
// dispatches to a plausible-but-wrong PC.
//
// Observed in ActRaiser: $03:9EF4's guard entered L_9E32_M0X0 while runtime m=1,
// so $9E39's PLA popped 2 bytes for the 1-byte PHA at $9E09 (odd +1 drift), after
// which $9EF4 popped the $A0D1 jump-table base as a return address.
//
// So: only take the goto when the runtime widths match the labels' decode widths;
// otherwise fall through to the generic host return, which is exactly what an
// unregistered target already does. This is upstream snesrecomp's invariant
// ("a combination without an exact AOT body ... never borrows a sibling decoded at
// a different operand width", mstan 03e389e) minus their LLE interpreter tier —
// our generic return is the fallback instead, by design.
//
// Each case collects the target's decoded variants and emits per-variant
// runtime m/x guards, refusing to jump into any wrong-width sibling.
func emitRTSDispatchGuard(instruction *cpu65816.Instruction, local map[decoder.DecodeKey]struct{}) []string {
	bank := byte(instruction.Address >> 16)
	site := instruction.Address & 0xffffff
	lines := []string{
		"{ uint16 _rts_s = cpu->S;",
		"  uint16 _rts_t = (uint16)(((cpu_read8(cpu, 0x00, (uint16)(_rts_s + 2)) << 8) | cpu_read8(cpu, 0x00, (uint16)(_rts_s + 1))) + 1);",
		"  int _rts_mx = ((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1); (void)_rts_mx;",
		"  switch (_rts_t) {",
	}
	for _, entry := range instruction.DispatchEntries {
		targetPC := uint32(bank)<<16 | entry&0xffff
		byMX := map[uint8]decoder.DecodeKey{}
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				key := decoder.DecodeKey{PC: targetPC, M: m, X: x}
				if _, found := local[key]; found {
					byMX[(m<<1)|x] = key
				}
			}
		}
		if len(byMX) == 0 {
			lines = append(lines, fmt.Sprintf("    /* case 0x%04X: no variant decoded in this function -> default */", uint16(entry)))
			continue
		}
		lines = append(lines, fmt.Sprintf("    case 0x%04X:", uint16(entry)))
		for _, mx := range []uint8{0, 1, 2, 3} {
			key, found := byMX[mx]
			if !found {
				continue
			}
			lines = append(lines, fmt.Sprintf("      if (_rts_mx == %d) { cpu->S = (uint16)(_rts_s + 2); goto %s; }", mx, label(key)))
		}
		lines = append(lines,
			"      if (getenv(\"AR_RTSDISP_MISS\"))",
			fmt.Sprintf("        fprintf(stderr, \"[rts_dispatch_width] site=$%06X popped target=$%04X: runtime m=%%d x=%%d has no decoded body -> generic return (refused wrong-width goto)\\n\", (int)cpu->m_flag, (int)cpu->x_flag);", site, uint16(entry)),
			"      break;  /* wrong-width -> normal return */")
	}
	return append(lines,
		"    default:",
		"      if (getenv(\"AR_RTSDISP_MISS\"))",
		fmt.Sprintf("        fprintf(stderr, \"[rts_dispatch_miss] site=$%06X popped target=$%%04X (UNREGISTERED -> generic return; add to rts_dispatch) S=$%%04X m=%%d x=%%d\\n\", (unsigned)_rts_t, (unsigned)cpu->S, (int)cpu->m_flag, (int)cpu->x_flag);", site),
		"      break;  /* unknown -> normal return */",
		"  } }",
	)
}

func emitSPCStub(name, baseName string, pc uint32) string {
	return fmt.Sprintf("RecompReturn %s(CpuState *cpu) {\n  extern const char *g_last_recomp_func;\n  extern bool RtlUploadSpcImageFromDp(CpuState *cpu);\n  g_last_recomp_func = \"%s\";\n  RecompStackPush(\"%s\");\n  cpu_dbg_funcname(\"%s\");\n  cpu_trace_func_entry(cpu, 0x%06X, \"%s\");\n  cpu_trace_block(cpu, 0x%06X);\n  WatchdogCheck();\n  if (!RtlUploadSpcImageFromDp(cpu)) {\n    fprintf(stderr, \"[apu] %s HLE upload failed\\n\");\n  }\n  RecompStackPop();\n  return RECOMP_RETURN_NORMAL;\n}\n", name, name, name, name, pc, name, pc, baseName)
}

func emitHLEStub(name, helper string, pc uint32) string {
	return fmt.Sprintf("RecompReturn %s(CpuState *cpu) {\n  extern const char *g_last_recomp_func;\n  extern RecompReturn %s(CpuState *cpu);\n  g_last_recomp_func = \"%s\";\n  RecompStackPush(\"%s\");\n  cpu_dbg_funcname(\"%s\");\n  cpu_trace_func_entry(cpu, 0x%06X, \"%s\");\n  cpu_trace_block(cpu, 0x%06X);\n  WatchdogCheck();\n  RecompReturn _r = %s(cpu);\n  RecompStackPop();\n  return _r;\n}\n", name, helper, name, name, name, pc, name, pc, helper)
}

func abs(value int) int {
	if value < 0 {
		return -value
	}
	return value
}
