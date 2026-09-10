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

// Conditional inventory, deliberately separate from return-value proofs and
// generation. A post-call path assumes a stack-neutral normal return. In
// particular, these findings cannot authorize raising a caller's runtime limit.
const frameLifetimeWindow = 64
const frameLifetimeStates = 4096

type ShadowFrameBoundary struct {
	PC               uint32           `json:"site_pc"`
	LiveMX           analysis.MXState `json:"live_mx"`
	Mnemonic         string           `json:"mnemonic"`
	InstructionBytes string           `json:"instruction_bytes"`
	Target           *uint32          `json:"target_pc,omitempty"`
	Continuation     *uint32          `json:"continuation_pc,omitempty"`
	SPDelta          int16            `json:"S_minus_entry_S"`
	Unprotected      uint8            `json:"unprotected_entry_byte_mask"`
	SavedCopies      uint8            `json:"modeled_saved_copy_byte_mask"`
	PullPCs          []uint32         `json:"entry_byte_pull_sites,omitempty"`
	WritePCs         []uint32         `json:"entry_byte_write_sites,omitempty"`
	Status           string           `json:"status"`
	Obligations      []string         `json:"proof_obligations,omitempty"`
}

type ShadowFrameLifetimeEntry struct {
	EntryPC         uint32                     `json:"entry_pc"`
	EntryMX         analysis.MXState           `json:"entry_mx"`
	FrameBytes      int                        `json:"assumed_native_call_frame_bytes"`
	Boundaries      []ShadowFrameBoundary      `json:"review_boundaries,omitempty"`
	Blockers        []ShadowReturnValueBlocker `json:"blockers,omitempty"`
	BlockersOmitted int                        `json:"blockers_omitted,omitempty"`
	BudgetHit       bool                       `json:"budget_exhausted,omitempty"`
}

type ShadowFrameLifetimes struct {
	Scope             string                     `json:"scope"`
	EntryContracts    int                        `json:"selected_entry_frame_contracts"`
	BoundaryContexts  int                        `json:"review_boundary_contexts"`
	UniqueSourceSites int                        `json:"review_source_sites"`
	SourceMXSites     int                        `json:"review_source_mx_sites"`
	BlockedContracts  int                        `json:"incomplete_entry_frame_contracts"`
	BudgetContracts   int                        `json:"budget_entry_frame_contracts"`
	Entries           []ShadowFrameLifetimeEntry `json:"entries,omitempty"`
	Obligations       []string                   `json:"proof_obligations"`
}

// Tokens 1..3 identify original entry bytes, 4..7 saved M/X, and zero an
// unknown value. Unlike stack height alone, this distinguishes PLA/PHA from
// PLA/LDA/PHA. Unknown does not mean a byte is definitely different.
type frameLifetimeState struct {
	key     decoder.DecodeKey
	sp      int16
	stack   [2*frameLifetimeWindow + 4]uint8
	regs    [3][2]uint8
	d       [2]uint8
	db      uint8
	pulls   [3]uint32
	writes  [3]uint32
	pulled  uint8
	written uint8
	hazards uint8
}

func (s *frameLifetimeState) read(slot int16, width int) ([2]uint8, bool) {
	var value [2]uint8
	if slot < -frameLifetimeWindow || int(slot)+width > frameLifetimeWindow+4 {
		return value, false
	}
	copy(value[:width], s.stack[int(slot)+frameLifetimeWindow:int(slot)+frameLifetimeWindow+width])
	return value, true
}

func (s *frameLifetimeState) write(slot int16, width int, value [2]uint8) bool {
	if slot < -frameLifetimeWindow || int(slot)+width > frameLifetimeWindow+4 {
		return false
	}
	copy(s.stack[int(slot)+frameLifetimeWindow:int(slot)+frameLifetimeWindow+width], value[:width])
	for b := range width {
		if n := int(slot) + b; n >= 1 && n <= 3 {
			s.writes[n-1] = s.key.PC
			s.written |= 1 << (n - 1)
		}
	}
	return true
}

func (s *frameLifetimeState) push(value [2]uint8, width int) bool {
	s.sp -= int16(width)
	return s.sp >= -frameLifetimeWindow && s.write(s.sp+1, width, value)
}

func (s *frameLifetimeState) pull(width int) ([2]uint8, bool) {
	value, ok := s.read(s.sp+1, width)
	for b := range width {
		if n := int(s.sp) + 1 + b; n >= 1 && n <= 3 {
			s.pulls[n-1] = s.key.PC
			s.pulled |= 1 << (n - 1)
		}
	}
	s.sp += int16(width)
	return value, ok && s.sp <= frameLifetimeWindow
}

func frameLifetimeHook(cfg *config.Config, pc uint32) bool {
	if cfg == nil {
		return false
	}
	_, a := cfg.HLEFunctions[uint16(pc)]
	_, b := cfg.HLEFunctionsIf[uint16(pc)]
	_, c := cfg.HLEDispatch[uint16(pc)]
	return a || b || c || slices.Contains(cfg.HLESPCUpload, uint16(pc))
}

// Return opcodes select hypotheses, not entry kinds. Tail-only or mixed-return
// graphs are checked under both normal-call frame hypotheses. RTI graphs are
// explicitly blocked rather than interpreting a saved status byte as a PC.
func auditShadowFrameLifetimes(g *decoder.Graph, cfg *config.Config) []ShadowFrameLifetimeEntry {
	pulls, boundaries, rts, rtl, rti := false, false, false, false, false
	for _, d := range g.Instructions {
		if d == nil || d.Instruction == nil {
			continue
		}
		i := d.Instruction
		switch i.Mnemonic {
		case "PLA", "PLX", "PLY", "PLB", "PLD", "PLP":
			pulls = true
		case "JSR", "JSL", "JMP", "JML":
			boundaries = true
		case "RTS":
			rts = rts || i.DispatchKind == ""
		case "RTL":
			rtl = rtl || i.DispatchKind == ""
		case "RTI":
			rti = true
		}
		pulls = pulls || i.Mode == cpu65816.STK && i.Mnemonic == "STA"
	}
	if !pulls || !boundaries {
		return nil
	}
	frames := []int{2, 3}
	if rts != rtl {
		if rts {
			frames = []int{2}
		} else {
			frames = []int{3}
		}
	}
	var results []ShadowFrameLifetimeEntry
	for _, frame := range frames {
		if rti {
			results = append(results, ShadowFrameLifetimeEntry{EntryPC: g.Entry.PC, EntryMX: analysis.MXState{M: g.Entry.M, X: g.Entry.X}, FrameBytes: frame, Blockers: []ShadowReturnValueBlocker{{PC: g.Entry.PC, Reason: "interrupt_entry_frame_contract_not_modeled"}}})
		} else {
			results = append(results, auditShadowFrameLifetime(g, cfg, frame))
		}
	}
	return results
}

func auditShadowFrameLifetime(g *decoder.Graph, cfg *config.Config, frame int) ShadowFrameLifetimeEntry {
	r := ShadowFrameLifetimeEntry{EntryPC: g.Entry.PC, EntryMX: analysis.MXState{M: g.Entry.M, X: g.Entry.X}, FrameBytes: frame}
	blockers := make(map[ShadowReturnValueBlocker]bool)
	block := func(pc uint32, reason string) { blockers[ShadowReturnValueBlocker{PC: pc, Reason: reason}] = true }
	initial := frameLifetimeState{key: g.Entry}
	for n := 1; n <= frame; n++ {
		initial.stack[frameLifetimeWindow+n] = uint8(n)
	}
	queue := []frameLifetimeState{initial}
	seen := make(map[frameLifetimeState]bool)
	findings := make(map[string]ShadowFrameBoundary)
	for len(queue) > 0 {
		s := queue[0]
		queue = queue[1:]
		if seen[s] {
			continue
		}
		if len(seen) >= frameLifetimeStates {
			block(g.Entry.PC, "state_budget")
			r.BudgetHit = true
			break
		}
		seen[s] = true
		d := g.Instructions[s.key]
		if d == nil || d.Instruction == nil {
			block(s.key.PC, "external_or_missing_decoded_edge")
			continue
		}
		i := d.Instruction
		if frameLifetimeHook(cfg, s.key.PC) {
			block(s.key.PC, "HLE_contract_not_inherited_from_ROM")
			continue
		}
		if i.DispatchKind != "" || i.DispatchEntries != nil {
			block(s.key.PC, "collapsed_dispatch_contract")
			continue
		}
		call := i.Mnemonic == "JSR" || i.Mnemonic == "JSL"
		jump := i.Mnemonic == "JMP" || i.Mnemonic == "JML"
		if call || jump {
			if f, ok := frameLifetimeBoundary(s, i, frame); ok {
				findings[shadowStoredJSONKey(f)] = f
			}
		}
		m, x := 2-int(s.key.M), 2-int(s.key.X)
		nextM, nextX := s.key.M, s.key.X
		ok, reason := true, ""
		load := func(reg, width int, value [2]uint8) {
			copy(s.regs[reg][:width], value[:width])
			if reg != 0 && width == 1 {
				s.regs[reg][1] = 0
			}
		}
		switch i.Mnemonic {
		case "RTS", "RTL", "RTI":
			continue
		case "JSR", "JSL":
			// Preserve ACTIVE stack bytes ONLY as a conditional post-call
			// contract. The callee overwrites the new frame and may use any
			// inactive scratch below it. Popped bytes cannot survive by accident.
			width := 2
			if i.Mnemonic == "JSL" {
				width = 3
			}
			ok = s.write(s.sp-int16(width)+1, width-1, [2]uint8{})
			ok = s.write(s.sp, 1, [2]uint8{}) && ok
			for n := -frameLifetimeWindow; n <= int(s.sp); n++ {
				s.stack[frameLifetimeWindow+n] = 0
			}
			s.hazards |= returnAuditCall
			s.regs, s.d, s.db = [3][2]uint8{}, [2]uint8{}, 0
		case "TCS", "TXS", "XCE", "BRK", "COP", "WDM", "WAI", "STP":
			reason = "unsupported_stack_or_control_effect"
		case "PHA":
			ok = s.push(s.regs[0], m)
		case "PHX":
			ok = s.push(s.regs[1], x)
		case "PHY":
			ok = s.push(s.regs[2], x)
		case "PHB":
			ok = s.push([2]uint8{s.db}, 1)
		case "PHK":
			ok = s.push([2]uint8{}, 1)
		case "PHD":
			ok = s.push(s.d, 2)
		case "PEA", "PEI", "PER":
			ok = s.push([2]uint8{}, 2)
		case "PHP":
			ok = s.push([2]uint8{4 + s.key.M*2 + s.key.X}, 1)
		case "PLA", "PLX", "PLY":
			reg, width := 0, m
			if i.Mnemonic == "PLX" {
				reg, width = 1, x
			}
			if i.Mnemonic == "PLY" {
				reg, width = 2, x
			}
			var value [2]uint8
			value, ok = s.pull(width)
			load(reg, width, value)
		case "PLD":
			s.d, ok = s.pull(2)
		case "PLB":
			var value [2]uint8
			value, ok = s.pull(1)
			s.db = value[0]
		case "PLP":
			var value [2]uint8
			value, ok = s.pull(1)
			if value[0] < 4 || value[0] > 7 {
				reason = "PLP_value_not_known_saved_status"
			} else {
				nextM, nextX = (value[0]-4)/2, (value[0]-4)%2
			}
		case "REP", "SEP":
			v := uint8(0)
			if i.Mnemonic == "SEP" {
				v = 1
			}
			if i.Operand&0x20 != 0 {
				nextM = v
			}
			if i.Operand&0x10 != 0 {
				nextX = v
			}
		case "LDA", "LDX", "LDY":
			reg, width := 0, m
			if i.Mnemonic == "LDX" {
				reg, width = 1, x
			}
			if i.Mnemonic == "LDY" {
				reg, width = 2, x
			}
			var value [2]uint8
			if i.Mode == cpu65816.STK {
				value, ok = s.read(s.sp+int16(i.Operand&255), width)
			}
			load(reg, width, value)
		case "STA":
			if i.Mode == cpu65816.STK {
				ok = s.write(s.sp+int16(i.Operand&255), m, s.regs[0])
			} else {
				s.hazards |= returnAuditWrite
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
		case "TCD":
			s.d = s.regs[0]
		case "TDC":
			s.regs[0] = s.d
		case "TSC":
			s.regs[0] = [2]uint8{}
		case "TSX":
			load(1, x, [2]uint8{})
		case "XBA":
			s.regs[0][0], s.regs[0][1] = s.regs[0][1], s.regs[0][0]
		case "INX", "DEX":
			load(1, x, [2]uint8{})
		case "INY", "DEY":
			load(2, x, [2]uint8{})
		case "ADC", "SBC", "AND", "ORA", "EOR":
			load(0, m, [2]uint8{})
		case "INC", "DEC", "ASL", "LSR", "ROL", "ROR":
			if i.Mode == cpu65816.ACC {
				load(0, m, [2]uint8{})
			} else {
				s.hazards |= returnAuditWrite
			}
		case "JMP", "JML":
			if i.Mode != cpu65816.ABS && i.Mode != cpu65816.LONG {
				reason = "dynamic_control_edge"
			}
		case "NOP", "CLC", "SEC", "CLI", "SEI", "CLV", "CLD", "SED", "CMP", "CPX", "CPY", "BIT", "BRA", "BRL", "BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BEQ", "BNE":
		default:
			if i.Mnemonic == "MVN" || i.Mnemonic == "MVP" {
				s.regs, s.db = [3][2]uint8{}, 0
				s.hazards |= returnAuditWrite
			} else if shadowDBMemoryWrite(i.Opcode, i.Mode) {
				s.hazards |= returnAuditWrite
			} else {
				reason = "unsupported_instruction_effect"
			}
		}
		if !ok || s.sp < -frameLifetimeWindow || s.sp > frameLifetimeWindow {
			reason, r.BudgetHit = "stack_window_budget", true
		}
		if reason != "" {
			block(s.key.PC, reason)
			continue
		}
		if len(d.Successors) == 0 {
			block(s.key.PC, "no_decoded_successor")
		}
		for _, key := range d.Successors {
			if !call && (key.M != nextM || key.X != nextX) {
				block(key.PC, "decoded_MX_disagrees_with_local_status")
				continue
			}
			next := s
			next.key = key
			if key.X == 1 {
				next.regs[1][1], next.regs[2][1] = 0, 0
			}
			queue = append(queue, next)
		}
	}
	for _, f := range findings {
		r.Boundaries = append(r.Boundaries, f)
	}
	sort.Slice(r.Boundaries, func(i, j int) bool {
		a, b := r.Boundaries[i], r.Boundaries[j]
		if a.PC != b.PC {
			return a.PC < b.PC
		}
		return shadowStoredJSONKey(a) < shadowStoredJSONKey(b)
	})
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
	return r
}

func frameLifetimeBoundary(s frameLifetimeState, i *cpu65816.Instruction, frame int) (ShadowFrameBoundary, bool) {
	f := ShadowFrameBoundary{PC: s.key.PC, LiveMX: analysis.MXState{M: s.key.M, X: s.key.X}, Mnemonic: i.Mnemonic, SPDelta: s.sp}
	for n := 1; n <= frame; n++ {
		mask := uint8(1 << (n - 1))
		if (s.pulled|s.written)&mask == 0 {
			continue
		}
		if s.sp < int16(n) && s.stack[frameLifetimeWindow+n] == uint8(n) {
			continue
		}
		f.Unprotected |= 1 << (n - 1)
		if s.pulled&mask != 0 {
			f.PullPCs = append(f.PullPCs, s.pulls[n-1])
		}
		if s.written&mask != 0 {
			f.WritePCs = append(f.WritePCs, s.writes[n-1])
		}
	}
	if f.Unprotected == 0 {
		return f, false
	}
	copyToken := func(token uint8) {
		if token >= 1 && int(token) <= frame {
			f.SavedCopies |= 1 << (token - 1)
		}
	}
	for _, reg := range s.regs {
		for _, token := range reg {
			copyToken(token)
		}
	}
	for _, token := range s.d {
		copyToken(token)
	}
	copyToken(s.db)
	// Popped, inactive memory does not retain ownership. Only active local
	// stack slots and modeled registers count as possible saved copies.
	for n := int(s.sp) + 1; n <= frameLifetimeWindow+3; n++ {
		copyToken(s.stack[frameLifetimeWindow+n])
	}
	f.SavedCopies &= f.Unprotected
	f.Status = "entry_bytes_not_proven_restored"
	if f.SavedCopies == f.Unprotected {
		f.Status = "entry_bytes_extracted_with_modeled_copies"
	}
	f.InstructionBytes = fmt.Sprintf("%02X", i.Opcode)
	for b := 0; b < int(i.Length)-1; b++ {
		f.InstructionBytes += fmt.Sprintf(" %02X", byte(i.Operand>>uint(8*b)))
	}
	if i.Mode == cpu65816.ABS || i.Mode == cpu65816.LONG {
		target := s.key.PC&0xff0000 | i.Operand&0xffff
		if i.Mode == cpu65816.LONG {
			target = i.Operand & 0xffffff
		}
		f.Target = &target
	}
	if i.Mnemonic == "JSR" || i.Mnemonic == "JSL" {
		next := s.key.PC&0xff0000 | uint32(uint16(s.key.PC)+uint16(i.Length))
		f.Continuation = &next
	}
	if s.hazards&returnAuditCall != 0 {
		f.Obligations = append(f.Obligations, "preceding_calls_return_normally_with_zero_net_stack_effect_and_preserve_stack_bytes")
	}
	if s.hazards&returnAuditWrite != 0 {
		f.Obligations = append(f.Obligations, "memory_and_hardware_writes_do_not_alias_modeled_stack_bytes")
	}
	slices.Sort(f.PullPCs)
	f.PullPCs = slices.Compact(f.PullPCs)
	slices.Sort(f.WritePCs)
	f.WritePCs = slices.Compact(f.WritePCs)
	return f, true
}

func collectShadowFrameLifetimes(results []shadowDecodeResult) ShadowFrameLifetimes {
	r := ShadowFrameLifetimes{Scope: "report_only_conditional_native_frame_lifetime_at_calls_and_jumps", Obligations: []string{
		"entry_kind_native_mode_nonwrapping_writable_stack_and_decoded_MX_are_assumptions",
		"entry_bytes_not_proven_restored_does_not_prove_permanent_frame_retirement",
		"saved_copy_inventory_excludes_unmodeled_memory_and_callee_effects",
		"decoded_paths_are_not_runtime_reachability_or_garbage_classification",
		"interrupts_and_hardware_must_preserve_the_conditional_stack_contract",
		"callee_cleanup_and_ancestor_frame_ownership_require_separate_proofs",
		"no_exit_facts_roots_cfg_HLE_runtime_boundary_or_generation_changes",
	}}
	entries := make(map[string]ShadowFrameLifetimeEntry)
	for _, result := range results {
		for _, e := range result.frameLifetimes {
			entries[shadowStoredJSONKey(e)] = e
		}
	}
	sites, mxSites := make(map[uint32]bool), make(map[decoder.Variant]bool)
	for _, e := range entries {
		r.Entries = append(r.Entries, e)
		if len(e.Blockers) > 0 {
			r.BlockedContracts++
		}
		if e.BudgetHit {
			r.BudgetContracts++
		}
		for _, f := range e.Boundaries {
			r.BoundaryContexts++
			sites[f.PC], mxSites[decoder.Variant{Address: f.PC, M: f.LiveMX.M, X: f.LiveMX.X}] = true, true
		}
	}
	sort.Slice(r.Entries, func(i, j int) bool {
		a, b := r.Entries[i], r.Entries[j]
		if a.EntryPC != b.EntryPC {
			return a.EntryPC < b.EntryPC
		}
		if a.EntryMX != b.EntryMX {
			return a.EntryMX.M*2+a.EntryMX.X < b.EntryMX.M*2+b.EntryMX.X
		}
		if a.FrameBytes != b.FrameBytes {
			return a.FrameBytes < b.FrameBytes
		}
		return shadowStoredJSONKey(a) < shadowStoredJSONKey(b)
	})
	r.EntryContracts, r.UniqueSourceSites, r.SourceMXSites = len(r.Entries), len(sites), len(mxSites)
	return r
}

func writeShadowFrameLifetimes(out io.Writer, r ShadowFrameLifetimes, verbose bool) {
	fmt.Fprintf(out, "frame-lifetime audit (report-only): %d entry/frame contracts; %d review contexts -> %d source sites / %d source-M/X sites; incomplete=%d budget=%d (not runtime failures)\n", r.EntryContracts, r.BoundaryContexts, r.UniqueSourceSites, r.SourceMXSites, r.BlockedContracts, r.BudgetContracts)
	if !verbose {
		return
	}
	for _, e := range r.Entries {
		for _, f := range e.Boundaries {
			target, continuation := "dynamic", "none"
			if f.Target != nil {
				target = shadowAddress(*f.Target)
			}
			if f.Continuation != nil {
				continuation = shadowAddress(*f.Continuation)
			}
			fmt.Fprintf(out, "[FRAME-LIFETIME] %s %s %s M%dX%d target=%s continuation=%s entry=%s M%dX%d assumed-frame=%d S-entry=%d unprotected-mask=%X saved-copy-mask=%X status=%s pulls=%s writes=%s obligations=%v\n", shadowAddress(f.PC), f.InstructionBytes, f.Mnemonic, f.LiveMX.M, f.LiveMX.X, target, continuation, shadowAddress(e.EntryPC), e.EntryMX.M, e.EntryMX.X, e.FrameBytes, f.SPDelta, f.Unprotected, f.SavedCopies, f.Status, frameLifetimeAddresses(f.PullPCs), frameLifetimeAddresses(f.WritePCs), f.Obligations)
		}
		for _, b := range e.Blockers {
			fmt.Fprintf(out, "[FRAME-LIFETIME-BLOCKED] entry=%s M%dX%d assumed-frame=%d site=%s reason=%s\n", shadowAddress(e.EntryPC), e.EntryMX.M, e.EntryMX.X, e.FrameBytes, shadowAddress(b.PC), b.Reason)
		}
		if e.BlockersOmitted != 0 {
			fmt.Fprintf(out, "  entry=%s M%dX%d assumed-frame=%d additional blockers omitted=%d\n", shadowAddress(e.EntryPC), e.EntryMX.M, e.EntryMX.X, e.FrameBytes, e.BlockersOmitted)
		}
	}
}

func frameLifetimeAddresses(pcs []uint32) []string {
	result := make([]string, 0, len(pcs))
	for _, pc := range pcs {
		result = append(result, shadowAddress(pc))
	}
	return result
}
