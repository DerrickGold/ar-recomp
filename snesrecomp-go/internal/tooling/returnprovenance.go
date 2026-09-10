package tooling

import (
	"fmt"
	"io"
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// Independent, report-only abstract execution. Neither a balanced frame nor
// a syntactic ADC near RTS establishes the value actually consumed by RTS.
// Bytes retain their word identity, including through partial overwrites.
const returnValueLimit = 32
const returnValueStates = 512

const (
	returnValueUnknown = uint8(iota)
	returnValueConstant
	returnValuePC
	returnValueBank
	returnValueStatus
	returnValueBits // lane is a known-bit mask; value contains only known ones
)

type returnByte struct {
	kind  uint8
	lane  uint8
	value uint16
}
type returnWord [2]returnByte

func returnSymbol(kind uint8, value uint16) returnWord {
	if kind == returnValueConstant {
		return returnWord{{kind: kind, value: value & 255}, {kind: kind, value: value >> 8}}
	}
	return returnWord{{kind: kind, value: value}, {kind: kind, lane: 1, value: value}}
}

func (w returnWord) symbol() (uint8, uint16) {
	if w[0].kind == returnValueConstant && w[1].kind == returnValueConstant {
		return returnValueConstant, w[0].value | w[1].value<<8
	}
	if w[0].kind == returnValuePC && w[1].kind == returnValuePC && w[0].lane == 0 && w[1].lane == 1 && w[0].value == w[1].value {
		return returnValuePC, w[0].value
	}
	return returnValueUnknown, 0
}

type returnValueState struct {
	key            decoder.DecodeKey
	context        int   // interned report-only call frame; zero is the queried entry
	sp             int16 // native S - entry S; no emulation-page wrapping assumed
	stack          [returnValueLimit*2 + 4]returnByte
	regs           [3]returnWord // A (including B), X, Y
	db             returnByte    // populated only by the opt-in stack-alias query
	carry, decimal int8          // -1 unknown, 0 clear, 1 set
}

func (s *returnValueState) read(slot int16, width int) (returnWord, bool) {
	if slot < -returnValueLimit || int(slot)+width > returnValueLimit+4 {
		return returnWord{}, false
	}
	var w returnWord
	for b := range width {
		w[b] = s.stack[int(slot)+returnValueLimit+b]
	}
	return w, true
}

func (s *returnValueState) write(slot int16, width int, w returnWord) bool {
	if slot < -returnValueLimit || int(slot)+width > returnValueLimit+4 {
		return false
	}
	for b := range width {
		s.stack[int(slot)+returnValueLimit+b] = w[b]
	}
	return true
}

func (s *returnValueState) push(w returnWord, width int) bool {
	if s.sp-int16(width) < -returnValueLimit {
		return false
	}
	s.sp -= int16(width)
	return s.write(s.sp+1, width, w)
}

func (s *returnValueState) pull(width int) (returnWord, bool) {
	w, ok := s.read(s.sp+1, width)
	s.sp += int16(width)
	return w, ok && s.sp <= returnValueLimit
}

type ShadowReturnValueBlocker struct {
	PC     uint32 `json:"site_pc"`
	Reason string `json:"reason"`
}

type ShadowReturnValueEntry struct {
	EntryPC         uint32                     `json:"entry_pc"`
	EntryMX         analysis.MXState           `json:"entry_mx"`
	Frame           string                     `json:"assumed_entry_frame"`
	Status          string                     `json:"status"`
	ReturnPCs       []uint32                   `json:"return_sites,omitempty"`
	Adjustments     []uint16                   `json:"stacked_PC_addends_mod_65536,omitempty"`
	ReadsIncoming   bool                       `json:"reads_incoming_return_bytes,omitempty"`
	Blockers        []ShadowReturnValueBlocker `json:"blockers,omitempty"`
	BlockersOmitted int                        `json:"blockers_omitted,omitempty"`
	hasBlockedCall  bool                       // preserve query selection independently of display truncation
	hasBlockedWrite bool
}

type ShadowReturnProvenance struct {
	Scope         string                   `json:"scope"`
	EntryVariants int                      `json:"selected_entry_variants"`
	Statuses      map[string]int           `json:"status_entry_variants"`
	Entries       []ShadowReturnValueEntry `json:"entries,omitempty"`
	Obligations   []string                 `json:"proof_obligations"`
}

// Selection is just an inventory filter, never a code-ownership heuristic.
func collectShadowReturnValues(g *decoder.Graph, cfg *config.Config) *ShadowReturnValueEntry {
	interesting, hasReturn := false, false
	for _, d := range g.Instructions {
		i := d.Instruction
		if i == nil {
			continue
		}
		hasReturn = hasReturn || i.Mnemonic == "RTS" || i.Mnemonic == "RTL"
		switch i.Mnemonic {
		case "PLA", "PLX", "PLY", "PLP", "PEA", "PEI", "PER":
			interesting = true
		}
		interesting = interesting || i.Mode == cpu65816.STK
	}
	if !interesting || !hasReturn {
		return nil
	}
	r := auditShadowReturnValues(g, cfg)
	return &r
}

func auditShadowReturnValues(g *decoder.Graph, cfg *config.Config) ShadowReturnValueEntry {
	return auditShadowReturnContext(g, cfg, nil)
}

func auditShadowReturnContext(g *decoder.Graph, cfg *config.Config, calls *returnCallContext) ShadowReturnValueEntry {
	r := ShadowReturnValueEntry{EntryPC: g.Entry.PC, EntryMX: analysis.MXState{M: g.Entry.M, X: g.Entry.X}, Status: "unproven"}
	frame := 0
	for _, d := range g.Instructions {
		if d.Instruction == nil {
			continue
		}
		switch d.Instruction.Mnemonic {
		case "RTS":
			frame |= 1
		case "RTL":
			frame |= 2
		}
	}
	if frame == 2 {
		r.Frame = "native_JSL"
	} else {
		r.Frame = "native_JSR"
	}
	blockers := make(map[ShadowReturnValueBlocker]bool)
	block := func(pc uint32, reason string) {
		blockers[ShadowReturnValueBlocker{pc, reason}] = true
		if reason == "callee_return_value_and_stack_contract" {
			r.hasBlockedCall = true
		}
		if reason == "memory_write_may_alias_return_frame" {
			r.hasBlockedWrite = true
		}
	}
	if frame == 3 {
		block(g.Entry.PC, "mixed_RTS_RTL_entry_frame_contract")
	}
	initial := returnValueState{key: g.Entry, carry: -1, decimal: -1}
	initial.write(1, 2, returnSymbol(returnValuePC, 0))
	if frame == 2 {
		initial.write(3, 1, returnWord{{kind: returnValueBank}})
	}
	queue := []returnValueState{initial}
	seen := make(map[returnValueState]bool)
	for len(queue) > 0 {
		s := queue[0]
		queue = queue[1:]
		if seen[s] {
			continue
		}
		if len(seen) >= returnValueStates {
			block(g.Entry.PC, "state_budget")
			break
		}
		seen[s] = true
		active, activeCfg := g, cfg
		if calls != nil && s.context != 0 {
			active, activeCfg, _ = calls.load(calls.frames[s.context].callee)
		}
		if active == nil {
			block(s.key.PC, "missing_call_context_program")
			continue
		}
		d := active.Instructions[s.key]
		if d == nil || d.Instruction == nil {
			block(s.key.PC, "external_or_missing_decoded_edge")
			continue
		}
		i := d.Instruction
		if activeCfg != nil {
			_, a := activeCfg.HLEFunctions[uint16(s.key.PC)]
			_, b := activeCfg.HLEFunctionsIf[uint16(s.key.PC)]
			_, c := activeCfg.HLEDispatch[uint16(s.key.PC)]
			if a || b || c || slices.Contains(activeCfg.HLESPCUpload, uint16(s.key.PC)) {
				block(s.key.PC, "HLE_contract_not_inherited_from_ROM")
				continue
			}
		}
		if i.DispatchKind != "" || i.DispatchEntries != nil {
			block(s.key.PC, "collapsed_dispatch_contract")
			continue
		}
		if i.Mnemonic == "RTS" || i.Mnemonic == "RTL" {
			if calls != nil && s.context != 0 {
				next, reason := calls.resume(s, i)
				if reason != "" {
					block(s.key.PC, reason)
				} else {
					queue = append(queue, next...)
				}
				continue
			}
			r.ReturnPCs = append(r.ReturnPCs, s.key.PC)
			w, _ := s.read(1, 2)
			kind, value := w.symbol()
			bank, _ := s.read(3, 1)
			switch {
			case s.sp != 0:
				block(s.key.PC, "return_not_at_entry_frame")
			case kind != returnValuePC:
				block(s.key.PC, "return_PC_not_incoming_PC_plus_constant")
			case frame == 2 && bank[0].kind != returnValueBank:
				block(s.key.PC, "return_bank_not_preserved")
			default:
				r.Adjustments = append(r.Adjustments, value)
			}
			continue
		}
		if calls != nil && (i.Mnemonic == "JSR" || i.Mnemonic == "JSL") {
			next, reason := calls.enter(s, active, i)
			if reason != "" {
				block(s.key.PC, reason)
			} else {
				queue = append(queue, next)
			}
			continue
		}
		m, x := 2-int(s.key.M), 2-int(s.key.X)
		nextM, nextX := s.key.M, s.key.X
		ok, reason := true, ""
		load := func(reg, width int, w returnWord) {
			for b := range width {
				s.regs[reg][b] = w[b]
			}
			if reg != 0 && width == 1 {
				s.regs[reg][1] = returnByte{kind: returnValueConstant}
			}
		}
		readStack := func(slot int16, width int) returnWord {
			w, valid := s.read(slot, width)
			ok = ok && valid
			for b := range width {
				if w[b].kind == returnValuePC || w[b].kind == returnValueBank {
					r.ReadsIncoming = true
				}
			}
			return w
		}
		switch i.Mnemonic {
		case "JSR", "JSL":
			reason = "callee_return_value_and_stack_contract"
		case "RTI":
			reason = "interrupt_return_contract"
		case "TCS", "TXS", "XCE", "BRK", "COP", "WDM", "WAI", "STP":
			reason = "unsupported_stack_or_control_effect"
		case "NOP", "CLI", "SEI", "CLV", "BRA", "BRL", "BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BEQ", "BNE":
		case "JMP":
			if i.Mode != cpu65816.ABS {
				reason = "dynamic_control_edge"
			}
		case "CLC":
			s.carry = 0
		case "SEC":
			s.carry = 1
		case "CLD":
			s.decimal = 0
		case "SED":
			s.decimal = 1
		case "REP", "SEP":
			v := int8(0)
			if i.Mnemonic == "SEP" {
				v = 1
			}
			if i.Operand&1 != 0 {
				s.carry = v
			}
			if i.Operand&8 != 0 {
				s.decimal = v
			}
			if i.Operand&0x20 != 0 {
				nextM = uint8(v)
			}
			if i.Operand&0x10 != 0 {
				nextX = uint8(v)
			}
		case "PHP":
			v := uint16(s.key.M)<<6 | uint16(s.key.X)<<4 | uint16(s.decimal+1)<<2 | uint16(s.carry+1)
			ok = s.push(returnWord{{kind: returnValueStatus, value: v}}, 1)
		case "PLP":
			w := readStack(s.sp+1, 1)
			s.sp++
			if w[0].kind != returnValueStatus {
				reason = "PLP_value_not_known_saved_status"
			} else {
				v := w[0].value
				nextM, nextX = uint8(v>>6&1), uint8(v>>4&1)
				s.decimal, s.carry = int8(v>>2&3)-1, int8(v&3)-1
			}
		case "PHA":
			ok = s.push(s.regs[0], m)
		case "PHX":
			ok = s.push(s.regs[1], x)
		case "PHY":
			ok = s.push(s.regs[2], x)
		case "PLA", "PLX", "PLY":
			reg, width := 0, m
			if i.Mnemonic == "PLX" {
				reg, width = 1, x
			}
			if i.Mnemonic == "PLY" {
				reg, width = 2, x
			}
			load(reg, width, readStack(s.sp+1, width))
			s.sp += int16(width)
		case "PHB", "PHK":
			w := returnWord{}
			if calls != nil && calls.stackAliases {
				w[0] = s.db
				if i.Mnemonic == "PHK" {
					w = returnSymbol(returnValueConstant, uint16(s.key.PC>>16))
				}
			}
			ok = s.push(w, 1)
		case "PHD", "PEI", "PER":
			ok = s.push(returnWord{}, 2)
		case "PEA":
			ok = s.push(returnSymbol(returnValueConstant, uint16(i.Operand)), 2)
		case "PLB":
			var w returnWord
			w, ok = s.pull(1)
			if calls != nil && calls.stackAliases {
				s.db = w[0]
			}
		case "PLD":
			_, ok = s.pull(2)
		case "LDA", "LDX", "LDY":
			reg, width := 0, m
			if i.Mnemonic == "LDX" {
				reg, width = 1, x
			}
			if i.Mnemonic == "LDY" {
				reg, width = 2, x
			}
			w := returnWord{}
			if i.Mode == cpu65816.IMM {
				w = returnSymbol(returnValueConstant, uint16(i.Operand))
			}
			if i.Mode == cpu65816.STK {
				w = readStack(s.sp+int16(i.Operand&255), width)
			}
			load(reg, width, w)
		case "STA":
			if i.Mode != cpu65816.STK {
				reason = "memory_write_may_alias_return_frame"
			} else {
				ok = s.write(s.sp+int16(i.Operand&255), m, s.regs[0])
			}
		case "TAX":
			load(1, x, s.regs[0])
		case "TAY":
			load(2, x, s.regs[0])
		case "TXA":
			load(0, m, s.regs[1])
		case "TYA":
			load(0, m, s.regs[2])
		case "TXY":
			load(2, x, s.regs[1])
		case "TYX":
			load(1, x, s.regs[2])
		case "TSC", "TDC":
			s.regs[0] = returnWord{}
		case "TSX":
			load(1, x, returnWord{})
		case "TCD": // D changes memory addressing, never native stack-relative addressing.
		case "XBA":
			s.regs[0][0], s.regs[0][1] = s.regs[0][1], s.regs[0][0]
		case "INX", "INY", "DEX", "DEY", "INC", "DEC":
			reg, width, delta := 0, m, uint16(1)
			if i.Mnemonic == "INX" || i.Mnemonic == "DEX" {
				reg, width = 1, x
			}
			if i.Mnemonic == "INY" || i.Mnemonic == "DEY" {
				reg, width = 2, x
			}
			if i.Mnemonic == "DEX" || i.Mnemonic == "DEY" || i.Mnemonic == "DEC" {
				delta = 65535
			}
			if reg == 0 && i.Mode != cpu65816.ACC {
				reason = "memory_write_may_alias_return_frame"
				break
			}
			kind, value := s.regs[reg].symbol()
			w := returnWord{}
			if width == 2 && kind != returnValueUnknown {
				w = returnSymbol(kind, value+delta)
			}
			load(reg, width, w)
		case "ADC", "SBC":
			kind, value := s.regs[0].symbol()
			w := returnWord{}
			if kind == returnValuePC && (s.decimal != 0 || s.carry < 0) {
				reason = "return_arithmetic_requires_known_binary_mode_and_carry"
			}
			if m == 2 && i.Mode == cpu65816.IMM && kind != returnValueUnknown && s.decimal == 0 && s.carry >= 0 {
				delta := uint16(i.Operand) + uint16(s.carry)
				if i.Mnemonic == "SBC" {
					delta = uint16(0) - uint16(i.Operand) - uint16(1-s.carry)
				}
				w = returnSymbol(kind, value+delta)
			}
			load(0, m, w)
			s.carry = -1
		case "CMP", "CPX", "CPY":
			s.carry = -1
		case "BIT":
		case "AND", "ORA", "EOR":
			w := returnWord{}
			if calls != nil && calls.stackAliases && i.Mode == cpu65816.IMM {
				w = returnLogicBits(i.Mnemonic, s.regs[0], m, uint16(i.Operand))
			}
			load(0, m, w)
		case "ASL", "LSR", "ROL", "ROR":
			if i.Mode != cpu65816.ACC {
				reason = "memory_write_may_alias_return_frame"
			} else {
				w, carry := returnWord{}, int8(-1)
				if calls != nil && calls.stackAliases {
					w, carry = returnShiftBits(i.Mnemonic, s.regs[0], m, s.carry)
				}
				load(0, m, w)
				s.carry = carry
			}
		default:
			if shadowDBMemoryWrite(i.Opcode, i.Mode) || i.Mnemonic == "MVN" || i.Mnemonic == "MVP" {
				reason = "memory_write_may_alias_return_frame"
			} else {
				reason = "unsupported_instruction_effect"
			}
		}
		if reason == "memory_write_may_alias_return_frame" && calls != nil && calls.stackAliases {
			write := auditReturnWrite(s, i)
			calls.writes[shadowStoredJSONKey(write)] = write
			if write.Status == "disjoint_unmirrored_WRAM" {
				reason = ""
			}
		}
		if !ok || s.sp < -returnValueLimit || s.sp > returnValueLimit {
			reason = "stack_window_budget"
		}
		if reason != "" {
			block(s.key.PC, reason)
			continue
		}
		if nextX == 1 {
			s.regs[1][1], s.regs[2][1] = returnByte{kind: returnValueConstant}, returnByte{kind: returnValueConstant}
		}
		if len(d.Successors) == 0 {
			block(s.key.PC, "no_decoded_normal_successor")
		}
		for _, key := range d.Successors {
			if key.M != nextM || key.X != nextX {
				block(key.PC, "decoded_MX_disagrees_with_saved_status")
				continue
			}
			next := s
			next.key = key
			queue = append(queue, next)
		}
	}
	slices.Sort(r.ReturnPCs)
	r.ReturnPCs = slices.Compact(r.ReturnPCs)
	slices.Sort(r.Adjustments)
	r.Adjustments = slices.Compact(r.Adjustments)
	if len(r.ReturnPCs) == 0 && len(blockers) == 0 {
		block(g.Entry.PC, "no_modeled_return")
	}
	for b := range blockers {
		r.Blockers = append(r.Blockers, b)
	}
	sort.Slice(r.Blockers, func(i, j int) bool {
		a, b := r.Blockers[i], r.Blockers[j]
		if a.PC != b.PC {
			return a.PC < b.PC
		}
		return a.Reason < b.Reason
	})
	if len(r.Blockers) > 8 {
		r.BlockersOmitted = len(r.Blockers) - 8
		r.Blockers = r.Blockers[:8]
	}
	if len(blockers) == 0 {
		switch {
		case len(r.Adjustments) != 1:
			r.Status = "path_dependent_adjustment"
		case r.Adjustments[0] == 0:
			r.Status = "conditional_incoming_PC_preserved"
		default:
			r.Status = "conditional_constant_adjustment"
		}
	}
	return r
}

func collectShadowReturnProvenance(results []shadowDecodeResult) ShadowReturnProvenance {
	r := ShadowReturnProvenance{Scope: "report_only_selected_native_return_value_contracts", Statuses: make(map[string]int), Obligations: []string{
		"selected_decoded_normal_return_graphs_with_pulls_stack_relative_operands_or_address_pushes",
		"entry_call_kind_native_mode_and_decoded_MX_are_conditional_not_reachability_proofs",
		"modeled_stack_window_is_writable_nonaliasing_memory_not_hardware_or_ROM",
		"interrupt_and_hardware_writes_must_preserve_the_guest_frame",
		"all_modeled_return_paths_not_a_termination_proof",
		"constant_adjustment_does_not_prove_inline_data_or_authorize_skipping_callsite_bytes",
		"no_exit_facts_roots_cfg_HLE_or_generation_changes",
	}}
	for _, result := range results {
		if result.returnValues != nil {
			r.Entries = append(r.Entries, *result.returnValues)
		}
	}
	sort.Slice(r.Entries, func(i, j int) bool {
		a, b := r.Entries[i], r.Entries[j]
		if a.EntryPC != b.EntryPC {
			return a.EntryPC < b.EntryPC
		}
		return a.EntryMX.M*2+a.EntryMX.X < b.EntryMX.M*2+b.EntryMX.X
	})
	r.Entries = slices.CompactFunc(r.Entries, func(a, b ShadowReturnValueEntry) bool { return shadowStoredJSONKey(a) == shadowStoredJSONKey(b) })
	r.EntryVariants = len(r.Entries)
	for _, e := range r.Entries {
		r.Statuses[e.Status]++
	}
	return r
}

func writeShadowReturnProvenance(output io.Writer, r ShadowReturnProvenance, verbose bool) {
	fmt.Fprintf(output, "return-address provenance (report-only): %d selected entry variants; conditional preserved=%d constant-adjusted=%d path-dependent=%d unproven=%d (not inline-data or runtime-reachability facts)\n", r.EntryVariants, r.Statuses["conditional_incoming_PC_preserved"], r.Statuses["conditional_constant_adjustment"], r.Statuses["path_dependent_adjustment"], r.Statuses["unproven"])
	if !verbose {
		return
	}
	for _, e := range r.Entries {
		if !e.ReadsIncoming && e.Status == "conditional_incoming_PC_preserved" {
			continue
		}
		fmt.Fprintf(output, "[RETURN-VALUE] %s M%dX%d %s %s addends=%v reads-entry=%t blockers=%v omitted=%d\n", shadowAddress(e.EntryPC), e.EntryMX.M, e.EntryMX.X, e.Frame, e.Status, e.Adjustments, e.ReadsIncoming, e.Blockers, e.BlockersOmitted)
	}
}
