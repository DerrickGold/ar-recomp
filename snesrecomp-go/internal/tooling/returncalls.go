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
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const (
	returnCallDepthLimit   = 8
	returnCallRootLimit    = 256
	returnCallProgramLimit = 512
	returnCallCheckLimit   = 64
)

type returnCallLoader func(decoder.Variant) (*decoder.Graph, *config.Config, string)

// These are abstract query contexts, not generated activations or a runtime
// fallback. A callee must consume exactly the frame pushed by its matching call.
type returnCallFrame struct {
	parent, depth  int
	caller, callee decoder.Variant
	site           decoder.DecodeKey
	sp             int16
	width          uint8
	returnPC       uint32
}

type returnCallKey struct {
	caller, callee decoder.Variant
	pc             uint32
}

type ShadowReturnCallExit struct {
	PC uint32           `json:"return_site_pc"`
	MX analysis.MXState `json:"live_mx"`
}

type ShadowReturnCallCheck struct {
	CallerEntryPC  uint32                 `json:"caller_entry_pc"`
	CallerEntryMX  analysis.MXState       `json:"caller_entry_mx"`
	PC             uint32                 `json:"call_pc"`
	TargetPC       uint32                 `json:"target_pc"`
	CallMX         analysis.MXState       `json:"call_mx"`
	ContinuationPC uint32                 `json:"continuation_pc"`
	Status         string                 `json:"status"`
	MatchedExits   []ShadowReturnCallExit `json:"individually_matched_frame_returns,omitempty"`
}

type ShadowReturnCallEntry struct {
	ShadowReturnValueEntry
	Calls            []ShadowReturnCallCheck `json:"calls,omitempty"`
	CallsOmitted     int                     `json:"calls_omitted,omitempty"`
	Writes           []ShadowReturnWrite     `json:"write_footprints,omitempty"`
	WritesOmitted    int                     `json:"write_footprints_omitted,omitempty"`
	WriteStatuses    map[string]int          `json:"write_footprint_status_counts,omitempty"`
	DisjointWritePCs []uint32                `json:"sites_with_disjoint_write_context,omitempty"`
}

type ShadowReturnCalls struct {
	Scope            string                  `json:"scope"`
	RequestedEntries int                     `json:"requested_entry_variants"`
	EntriesOmitted   int                     `json:"entry_variants_omitted"`
	Programs         int                     `json:"loaded_exact_programs"`
	ProgramBudgetHit bool                    `json:"program_budget_hit,omitempty"`
	Statuses         map[string]int          `json:"status_entry_variants"`
	Entries          []ShadowReturnCallEntry `json:"entries,omitempty"`
	Obligations      []string                `json:"proof_obligations"`
}

type returnCallContext struct {
	load         returnCallLoader
	frames       []returnCallFrame
	frameIDs     map[returnCallFrame]int
	checks       map[returnCallKey]*ShadowReturnCallCheck
	stackAliases bool
	writes       map[string]ShadowReturnWrite
}

func (f returnCallFrame) key() returnCallKey { return returnCallKey{f.caller, f.callee, f.site.PC} }

func (c *returnCallContext) enter(s returnValueState, g *decoder.Graph, i *cpu65816.Instruction) (returnValueState, string) {
	f := returnCallFrame{parent: s.context, depth: c.frames[s.context].depth + 1, caller: decoder.Variant{Address: g.Entry.PC, M: g.Entry.M, X: g.Entry.X}, site: s.key, sp: s.sp, width: 2}
	var target uint32
	switch {
	case i.Opcode == 0x20 && i.Mode == cpu65816.ABS && i.Length == 3:
		target = s.key.PC&0xff0000 | i.Operand&0xffff
	case i.Opcode == 0x22 && i.Mode == cpu65816.LONG && i.Length == 4:
		target = i.Operand & 0xffffff
		f.width = 3
	default:
		return s, "unsupported_or_indirect_call_contract"
	}
	f.callee = decoder.Variant{Address: target, M: s.key.M, X: s.key.X}
	f.returnPC = s.key.PC&0xff0000 | uint32(uint16(s.key.PC)+uint16(i.Length))
	check := c.checks[f.key()]
	if check == nil {
		check = &ShadowReturnCallCheck{CallerEntryPC: f.caller.Address, CallerEntryMX: analysis.MXState{M: f.caller.M, X: f.caller.X}, PC: s.key.PC, TargetPC: target, CallMX: analysis.MXState{M: s.key.M, X: s.key.X}, ContinuationPC: f.returnPC}
		c.checks[f.key()] = check
	}
	fail := func(reason string) (returnValueState, string) {
		// A check is an observation inventory, not a universal call summary.
		// Keep an earlier failure even if another context can enter this call.
		if check.Status == "" || check.Status == "entered_exact_variant" {
			check.Status = reason
		}
		return s, reason
	}
	if f.depth > returnCallDepthLimit {
		return fail("abstract_call_depth_budget")
	}
	callee, _, reason := c.load(f.callee)
	if reason != "" {
		return fail(reason)
	}
	if callee == nil || callee.Entry.PC != target || callee.Entry.M != s.key.M || callee.Entry.X != s.key.X {
		return fail("callee_exact_entry_mismatch")
	}
	// Native JSL pushes bank, high PC, low PC; JSR pushes high PC, low PC.
	// Even an empty callee overwrites these bytes. A caller may have pulled
	// its own incoming frame into registers before making this call.
	if f.width == 3 && !s.push(returnSymbol(returnValueConstant, uint16(s.key.PC>>16)), 1) {
		return fail("stack_window_budget")
	}
	if !s.push(returnSymbol(returnValueConstant, uint16(f.returnPC)-1), 2) {
		return fail("stack_window_budget")
	}
	id, ok := c.frameIDs[f]
	if !ok {
		id = len(c.frames)
		c.frames = append(c.frames, f)
		c.frameIDs[f] = id
	}
	s.context, s.key = id, callee.Entry
	if check.Status == "" {
		check.Status = "entered_exact_variant"
	}
	return s, ""
}

func (c *returnCallContext) resume(s returnValueState, i *cpu65816.Instruction) ([]returnValueState, string) {
	f := c.frames[s.context]
	if (f.width == 2 && i.Mnemonic != "RTS") || (f.width == 3 && i.Mnemonic != "RTL") {
		return nil, "callee_return_opcode_does_not_match_call_frame"
	}
	if s.sp != f.sp-int16(f.width) {
		return nil, "callee_return_not_at_matching_call_frame"
	}
	w, ok := s.read(s.sp+1, 2)
	kind, value := w.symbol()
	if !ok || kind != returnValueConstant || value != uint16(f.returnPC)-1 {
		return nil, "callee_return_PC_adjusted_nonlocal_or_unknown"
	}
	if f.width == 3 {
		bank, valid := s.read(s.sp+3, 1)
		if !valid || bank[0].kind != returnValueConstant || bank[0].value != uint16(f.site.PC>>16) {
			return nil, "callee_return_bank_changed_or_unknown"
		}
	}
	check := c.checks[f.key()]
	check.MatchedExits = append(check.MatchedExits, ShadowReturnCallExit{PC: s.key.PC, MX: analysis.MXState{M: s.key.M, X: s.key.X}})
	caller, _, reason := c.load(f.caller)
	if reason != "" || caller == nil || caller.Instructions[f.site] == nil {
		return nil, "caller_continuation_program_unavailable"
	}
	var next []returnValueState
	for _, key := range caller.Instructions[f.site].Successors {
		// Do not pick a canonical exit width, invent a successor, or follow
		// synthetic dispatch-handler edges as ordinary call continuations.
		if key.PC != f.returnPC || key.M != s.key.M || key.X != s.key.X {
			continue
		}
		n := s
		n.context = f.parent
		n.sp = f.sp
		n.key = key
		next = append(next, n)
	}
	if len(next) == 0 {
		return nil, "call_exit_MX_or_continuation_absent_from_caller_decode"
	}
	return next, ""
}

func auditShadowReturnCalls(g *decoder.Graph, cfg *config.Config, load returnCallLoader) ShadowReturnCallEntry {
	return auditShadowReturnCallMode(g, cfg, load, false)
}

func auditShadowReturnCallMode(g *decoder.Graph, cfg *config.Config, load returnCallLoader, stackAliases bool) ShadowReturnCallEntry {
	ctx := &returnCallContext{load: load, frames: []returnCallFrame{{}}, frameIDs: make(map[returnCallFrame]int), checks: make(map[returnCallKey]*ShadowReturnCallCheck), stackAliases: stackAliases, writes: make(map[string]ShadowReturnWrite)}
	r := ShadowReturnCallEntry{ShadowReturnValueEntry: auditShadowReturnContext(g, cfg, ctx)}
	for _, check := range ctx.checks {
		sort.Slice(check.MatchedExits, func(i, j int) bool {
			a, b := check.MatchedExits[i], check.MatchedExits[j]
			if a.PC != b.PC {
				return a.PC < b.PC
			}
			return a.MX.M*2+a.MX.X < b.MX.M*2+b.MX.X
		})
		check.MatchedExits = slices.Compact(check.MatchedExits)
		r.Calls = append(r.Calls, *check)
	}
	sort.Slice(r.Calls, func(i, j int) bool {
		a, b := r.Calls[i], r.Calls[j]
		if a.PC != b.PC {
			return a.PC < b.PC
		}
		return shadowStoredJSONKey(a) < shadowStoredJSONKey(b)
	})
	if len(r.Calls) > returnCallCheckLimit {
		r.CallsOmitted = len(r.Calls) - returnCallCheckLimit
		r.Calls = r.Calls[:returnCallCheckLimit]
	}
	if len(ctx.writes) != 0 {
		r.WriteStatuses = make(map[string]int)
	}
	for _, write := range ctx.writes {
		r.Writes = append(r.Writes, write)
		r.WriteStatuses[write.Status]++
		if write.Status == "disjoint_unmirrored_WRAM" {
			r.DisjointWritePCs = append(r.DisjointWritePCs, write.PC)
		}
	}
	slices.Sort(r.DisjointWritePCs)
	r.DisjointWritePCs = slices.Compact(r.DisjointWritePCs)
	sort.Slice(r.Writes, func(i, j int) bool {
		a, b := r.Writes[i], r.Writes[j]
		if a.PC != b.PC {
			return a.PC < b.PC
		}
		return shadowStoredJSONKey(a) < shadowStoredJSONKey(b)
	})
	if len(r.Writes) > returnAliasWriteLimit {
		r.WritesOmitted = len(r.Writes) - returnAliasWriteLimit
		r.Writes = r.Writes[:returnAliasWriteLimit]
	}
	return r
}

func collectShadowReturnCalls(image romimage.Image, banks []shadowBank, results []shadowDecodeResult) ShadowReturnCalls {
	r := ShadowReturnCalls{Scope: "report_only_context_specific_direct_call_return_contracts", Statuses: make(map[string]int), Obligations: []string{
		"selected_previous_return_value_entries_blocked_at_calls_not_all_callable_code",
		"native_entry_frame_MX_and_writable_nonaliasing_stack_window_contracts",
		"interrupt_and_hardware_writes_preserve_the_guest_frame",
		"context_specific_matched_returns_not_universal_callee_summaries_or_termination_proofs",
		"existing_exact_variants_only_no_mirror_or_canonical_MX_substitution",
		"individually_matched_exits_do_not_close_a_query_with_other_blocked_paths",
		"HLE_contracts_not_inherited_from_ROM",
		"no_inline_data_exit_facts_roots_cfg_or_generation_changes",
	}}
	var roots []decoder.Variant
	for _, result := range results {
		if result.returnValues != nil && (result.returnValues.hasBlockedCall || slices.ContainsFunc(result.returnValues.Blockers, func(b ShadowReturnValueBlocker) bool { return b.Reason == "callee_return_value_and_stack_contract" })) {
			roots = append(roots, result.entry)
		}
	}
	return collectShadowReturnQueries(image, banks, results, roots, r, false)
}

// Each report owns its own deterministic program budget. A stronger follow-up
// query cannot consume cache slots needed by the original return-call report.
func collectShadowReturnQueries(image romimage.Image, banks []shadowBank, results []shadowDecodeResult, roots []decoder.Variant, r ShadowReturnCalls, stackAliases bool) ShadowReturnCalls {
	byEntry := make(map[decoder.Variant]shadowDecodeResult)
	configs := make(map[byte]*config.Config)
	for _, b := range banks {
		configs[b.ID] = b.Config
	}
	// Reconstruct precisely the authored boundaries used by the shadow pass,
	// including sibling variants whose decode failed. Never open a boundary
	// just because the corresponding program could not be loaded.
	siblings := make(map[byte]map[uint16]struct{})
	for _, result := range results {
		v := result.entry
		byEntry[v] = result
		b := byte(v.Address >> 16)
		if siblings[b] == nil {
			siblings[b] = make(map[uint16]struct{})
		}
		siblings[b][uint16(v.Address)] = struct{}{}
	}
	sort.Slice(roots, func(i, j int) bool {
		a, b := roots[i], roots[j]
		if a.Address != b.Address {
			return a.Address < b.Address
		}
		return a.M*2+a.X < b.M*2+b.X
	})
	roots = slices.Compact(roots)
	r.RequestedEntries = len(roots)
	if len(roots) > returnCallRootLimit {
		r.EntriesOmitted = len(roots) - returnCallRootLimit
		roots = roots[:returnCallRootLimit]
	}
	programs := make(map[decoder.Variant]*decoder.Graph)
	errors := make(map[decoder.Variant]string)
	load := func(v decoder.Variant) (*decoder.Graph, *config.Config, string) {
		cfg := configs[byte(v.Address>>16)]
		if g := programs[v]; g != nil {
			return g, cfg, ""
		}
		if reason := errors[v]; reason != "" {
			return nil, cfg, reason
		}
		result, ok := byEntry[v]
		if !ok {
			return nil, cfg, "callee_exact_variant_not_in_shadow_closure"
		}
		if cfg == nil || result.issue != nil || result.bankRecipe == nil {
			return nil, cfg, "exact_variant_decode_unavailable"
		}
		if len(programs) >= returnCallProgramLimit {
			r.ProgramBudgetHit = true
			return nil, cfg, "return_call_program_budget"
		}
		own := make(map[uint16]struct{})
		for pc := range siblings[byte(v.Address>>16)] {
			if pc != uint16(v.Address) {
				own[pc] = struct{}{}
			}
		}
		recipe := result.bankRecipe
		g, err := decoder.DecodeFunction(image, byte(v.Address>>16), uint16(v.Address), v.M, v.X, decoder.Options{End: recipe.end, DataRegions: recipe.regions, CalleeExitMX: recipe.exitMX, HLEDispatch: cfg.HLEDispatch, SiblingEntryPCs: own})
		if err != nil {
			errors[v] = "exact_variant_redecode_failed"
			return nil, cfg, errors[v]
		}
		programs[v] = g
		return g, cfg, ""
	}
	for _, v := range roots {
		g, cfg, reason := load(v)
		var entry ShadowReturnCallEntry
		if reason != "" {
			entry.ShadowReturnValueEntry = ShadowReturnValueEntry{EntryPC: v.Address, EntryMX: analysis.MXState{M: v.M, X: v.X}, Status: "unproven", Blockers: []ShadowReturnValueBlocker{{PC: v.Address, Reason: reason}}}
		} else {
			entry = auditShadowReturnCallMode(g, cfg, load, stackAliases)
		}
		r.Entries = append(r.Entries, entry)
		r.Statuses[entry.Status]++
	}
	r.Programs = len(programs)
	return r
}

func writeShadowReturnCalls(output io.Writer, r ShadowReturnCalls, verbose bool) {
	fmt.Fprintf(output, "return-call contracts (report-only): %d requested entry variants, %d omitted, %d loaded programs; conditional preserved=%d adjusted=%d path-dependent=%d unproven=%d; program-budget=%t (not new roots or runtime debt)\n", r.RequestedEntries, r.EntriesOmitted, r.Programs, r.Statuses["conditional_incoming_PC_preserved"], r.Statuses["conditional_constant_adjustment"], r.Statuses["path_dependent_adjustment"], r.Statuses["unproven"], r.ProgramBudgetHit)
	if !verbose {
		return
	}
	for _, entry := range r.Entries {
		fmt.Fprintf(output, "[RETURN-CALL] %s M%dX%d %s addends=%v blockers=%v\n", shadowAddress(entry.EntryPC), entry.EntryMX.M, entry.EntryMX.X, entry.Status, entry.Adjustments, entry.Blockers)
		for _, c := range entry.Calls {
			fmt.Fprintf(output, "  call=%s target=%s M%dX%d continuation=%s %s matched-frame-returns=%v\n", shadowAddress(c.PC), shadowAddress(c.TargetPC), c.CallMX.M, c.CallMX.X, shadowAddress(c.ContinuationPC), c.Status, c.MatchedExits)
		}
	}
}
