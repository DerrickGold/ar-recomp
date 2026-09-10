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

// This is an audit of the existing decoded graph, not an exit-M/X proof.
// Positive depth means guest bytes pushed below the assumed entry S. Calls
// have zero NET depth only under an explicit normal-return contract; neither
// that contract nor the decoded widths are established by this analysis.
const (
	returnAuditDepthLimit = 64
	returnAuditStateLimit = 4096
	returnAuditUnknown    = int16(32767)
)

const (
	returnAuditCall = uint8(1 << iota)
	returnAuditWrite
	returnAuditOpaque
	returnAuditHLE
)

type ShadowReturnContext struct {
	EntryPC             uint32           `json:"entry_pc"`
	EntryMX             analysis.MXState `json:"entry_mx"`
	Depths              []int16          `json:"local_stack_depths,omitempty"`
	UnknownDepth        bool             `json:"unknown_stack_depth,omitempty"`
	Shapes              []string         `json:"frame_shapes"`
	Obligations         []string         `json:"proof_obligations,omitempty"`
	LegacyExitCandidate bool             `json:"legacy_local_exit_candidate"`
	Incomplete          bool             `json:"incomplete_audit,omitempty"`
}

type ShadowReturnSite struct {
	PC               uint32                `json:"site_pc"`
	Mnemonic         string                `json:"mnemonic"`
	InstructionBytes string                `json:"instruction_bytes"`
	LiveMX           analysis.MXState      `json:"live_mx"`
	Contexts         []ShadowReturnContext `json:"contexts"`
}

type ShadowReturnAudit struct {
	Scope                string             `json:"scope"`
	RawReturnContexts    int                `json:"raw_return_contexts"`
	UniqueSourceSites    int                `json:"unique_source_sites"`
	SourceMXSites        int                `json:"source_mx_sites"`
	FrameReviewSites     int                `json:"frame_review_source_sites"`
	CandidateReviewSites int                `json:"legacy_candidate_review_source_sites"`
	BudgetEntries        int                `json:"budget_entry_variants"`
	ShapeSourceSites     map[string]int     `json:"shape_source_sites"`
	HLEAffectedSites     int                `json:"HLE_affected_source_sites"`
	Sites                []ShadowReturnSite `json:"sites,omitempty"`
	Obligations          []string           `json:"proof_obligations"`
}

type shadowReturnAuditResult struct {
	sites     []ShadowReturnSite
	budgetHit bool
}

type shadowReturnState struct {
	key     decoder.DecodeKey
	depth   int16
	written uint8 // entry S+1/S+2/S+3 explicitly overwritten
	hazards uint8
}

func shadowReturnShape(depth int16, frame int, written uint8) string {
	switch {
	case depth == returnAuditUnknown:
		return "unknown_stack_position"
	case depth >= int16(frame):
		return "locally_pushed_frame"
	case depth > 0:
		return "mixed_local_and_entry_frame"
	case depth < 0:
		return "past_entry_frame"
	case written&uint8((1<<frame)-1) != 0:
		return "entry_frame_written"
	default:
		return "entry_frame_position_only"
	}
}

func auditShadowReturns(graph *decoder.Graph, cfg *config.Config) shadowReturnAuditResult {
	result := shadowReturnAuditResult{}
	type siteKey struct {
		pc   uint32
		m, x uint8
	}
	bySite := make(map[siteKey]*ShadowReturnSite)
	for _, key := range graph.Order {
		d := graph.Instructions[key]
		if d == nil || d.Instruction == nil {
			continue
		}
		i := d.Instruction
		if i.Mnemonic != "RTS" && i.Mnemonic != "RTL" && i.Mnemonic != "RTI" {
			continue
		}
		k := siteKey{key.PC, key.M, key.X}
		if bySite[k] == nil {
			bySite[k] = &ShadowReturnSite{PC: key.PC, Mnemonic: i.Mnemonic, InstructionBytes: fmt.Sprintf("%02X", i.Opcode), LiveMX: analysis.MXState{M: key.M, X: key.X}, Contexts: []ShadowReturnContext{{EntryPC: graph.Entry.PC, EntryMX: analysis.MXState{M: graph.Entry.M, X: graph.Entry.X}, LegacyExitCandidate: i.DispatchKind != "rts_trick"}}}
		}
	}
	if len(bySite) == 0 {
		return result
	}
	hook := func(pc uint32) bool {
		if cfg == nil {
			return false
		}
		_, a := cfg.HLEFunctions[uint16(pc)]
		_, b := cfg.HLEFunctionsIf[uint16(pc)]
		_, c := cfg.HLEDispatch[uint16(pc)]
		return a || b || c || slices.Contains(cfg.HLESPCUpload, uint16(pc))
	}
	queue := []shadowReturnState{{key: graph.Entry}}
	seen := make(map[shadowReturnState]bool)
	incomplete := false
	for len(queue) > 0 {
		state := queue[0]
		queue = queue[1:]
		if seen[state] {
			continue
		}
		if len(seen) >= returnAuditStateLimit {
			result.budgetHit, incomplete = true, true
			break
		}
		seen[state] = true
		d := graph.Instructions[state.key]
		if d == nil || d.Instruction == nil {
			incomplete = true
			continue
		}
		i := d.Instruction
		if hook(state.key.PC) {
			state.depth = returnAuditUnknown
			state.hazards |= returnAuditHLE
		}
		if site := bySite[siteKey{state.key.PC, state.key.M, state.key.X}]; site != nil {
			c := &site.Contexts[0]
			if state.depth == returnAuditUnknown {
				c.UnknownDepth = true
			} else {
				c.Depths = append(c.Depths, state.depth)
			}
			frame := 2
			if i.Mnemonic == "RTL" {
				frame = 3
			}
			shape := shadowReturnShape(state.depth, frame, state.written)
			if i.Mnemonic == "RTI" {
				shape = "interrupt_frame_not_normal_call_exit"
			}
			if i.DispatchKind == "rts_trick" {
				shape = "recognized_dispatch_not_normal_call_exit"
			}
			c.Shapes = append(c.Shapes, shape)
			for _, obligation := range []struct {
				bit  uint8
				text string
			}{
				{returnAuditCall, "callee_normal_return_and_stack_effects"},
				{returnAuditWrite, "memory_writes_may_alias_return_frame"},
				{returnAuditOpaque, "opaque_stack_or_collapsed_control_effect"},
				{returnAuditHLE, "HLE_contract_not_inherited_from_ROM"},
			} {
				if state.hazards&obligation.bit != 0 {
					c.Obligations = append(c.Obligations, obligation.text)
				}
			}
			continue
		}
		// A collapsed dispatch can consume pushes absent from the surviving
		// graph. Do not derive a physical stack effect from its carrier opcode.
		if i.DispatchKind != "" || i.DispatchEntries != nil {
			state.depth = returnAuditUnknown
			state.hazards |= returnAuditOpaque
		} else if i.Mnemonic == "JSR" || i.Mnemonic == "JSL" {
			state.hazards |= returnAuditCall
		} else {
			count := int16(0)
			switch i.Opcode {
			case 0x08, 0x8b, 0x4b:
				count = 1
			case 0x0b, 0xf4, 0xd4, 0x62:
				count = 2
			case 0x48:
				count = 2 - int16(state.key.M)
			case 0xda, 0x5a:
				count = 2 - int16(state.key.X)
			case 0x28, 0xab:
				count = -1
			case 0x2b:
				count = -2
			case 0x68:
				count = -(2 - int16(state.key.M))
			case 0xfa, 0x7a:
				count = -(2 - int16(state.key.X))
			case 0x1b, 0x9a, 0xfb, 0x00, 0x02, 0x42:
				state.depth = returnAuditUnknown
				state.hazards |= returnAuditOpaque
			case 0x44, 0x54:
				state.hazards |= returnAuditWrite
			default:
				if shadowDBMemoryWrite(i.Opcode, i.Mode) {
					if i.Mode == cpu65816.STK && state.depth != returnAuditUnknown {
						// Stack-relative STA has an exact native bank-zero
						// address relative to entry S, unlike a DP spelling.
						start := int16(i.Operand&0xff) - state.depth
						for b := int16(0); b < 2-int16(state.key.M); b++ {
							if slot := start + b; slot >= 1 && slot <= 3 {
								state.written |= 1 << uint(slot-1)
							}
						}
					} else {
						state.hazards |= returnAuditWrite
					}
				}
			}
			if state.depth != returnAuditUnknown {
				for b := int16(0); b < count; b++ {
					// A push after pulling entry bytes overwrites those
					// slots. Balanced height alone does not preserve PC.
					if slot := -state.depth - b; slot >= 1 && slot <= 3 {
						state.written |= 1 << uint(slot-1)
					}
				}
				state.depth += count
				if state.depth < -returnAuditDepthLimit || state.depth > returnAuditDepthLimit {
					state.depth = returnAuditUnknown
					state.hazards |= returnAuditOpaque
					result.budgetHit = true
				}
			}
		}
		for _, successor := range d.Successors {
			if graph.Instructions[successor] == nil {
				incomplete = true
				continue
			}
			next := state
			next.key = successor
			queue = append(queue, next)
		}
	}
	for _, site := range bySite {
		c := &site.Contexts[0]
		if len(c.Shapes) == 0 {
			c.Shapes = []string{"not_reached_in_audit"}
			c.Incomplete = true
		}
		c.Incomplete = c.Incomplete || incomplete || result.budgetHit
		slices.Sort(c.Depths)
		c.Depths = slices.Compact(c.Depths)
		slices.Sort(c.Shapes)
		c.Shapes = slices.Compact(c.Shapes)
		slices.Sort(c.Obligations)
		c.Obligations = slices.Compact(c.Obligations)
		result.sites = append(result.sites, *site)
	}
	sort.Slice(result.sites, func(i, j int) bool {
		a, b := result.sites[i], result.sites[j]
		if a.PC != b.PC {
			return a.PC < b.PC
		}
		return a.LiveMX.M*2+a.LiveMX.X < b.LiveMX.M*2+b.LiveMX.X
	})
	return result
}

func collectShadowReturnAudit(results []shadowDecodeResult) ShadowReturnAudit {
	r := ShadowReturnAudit{Scope: "report_only_decoded_native_return_frame_shapes", Obligations: []string{"entry_kind_and_call_frame_not_proven", "decoded_MX_and_PLP_values_not_verified", "callee_and_interrupt_stack_contracts", "memory_aliases_and_return_PC_values", "decoded_paths_not_runtime_reachability", "no_exit_facts_roots_cfg_HLE_or_generation_changes"}}
	bySite := make(map[string]*ShadowReturnSite)
	budgets := make(map[decoder.Variant]bool)
	for _, result := range results {
		if result.returnAudit.budgetHit {
			budgets[result.entry] = true
		}
		for _, site := range result.returnAudit.sites {
			r.RawReturnContexts += len(site.Contexts)
			key := fmt.Sprintf("%06X/%d/%d", site.PC, site.LiveMX.M, site.LiveMX.X)
			if prior := bySite[key]; prior != nil {
				prior.Contexts = append(prior.Contexts, site.Contexts...)
			} else {
				copy := site
				copy.Contexts = slices.Clone(site.Contexts)
				bySite[key] = &copy
			}
		}
	}
	unique, review, candidates := make(map[uint32]bool), make(map[uint32]bool), make(map[uint32]bool)
	shapes, hooks := make(map[string]map[uint32]bool), make(map[uint32]bool)
	for _, site := range bySite {
		unique[site.PC] = true
		sort.Slice(site.Contexts, func(i, j int) bool {
			a, b := site.Contexts[i], site.Contexts[j]
			if a.EntryPC != b.EntryPC {
				return a.EntryPC < b.EntryPC
			}
			if a.EntryMX != b.EntryMX {
				return a.EntryMX.M*2+a.EntryMX.X < b.EntryMX.M*2+b.EntryMX.X
			}
			return shadowStoredJSONKey(a) < shadowStoredJSONKey(b)
		})
		site.Contexts = slices.CompactFunc(site.Contexts, func(a, b ShadowReturnContext) bool { return shadowStoredJSONKey(a) == shadowStoredJSONKey(b) })
		for _, c := range site.Contexts {
			if slices.Contains(c.Obligations, "HLE_contract_not_inherited_from_ROM") {
				hooks[site.PC] = true
			}
			for _, shape := range c.Shapes {
				if shapes[shape] == nil {
					shapes[shape] = make(map[uint32]bool)
				}
				shapes[shape][site.PC] = true
				if shape != "entry_frame_position_only" && shape != "recognized_dispatch_not_normal_call_exit" {
					review[site.PC] = true
					if c.LegacyExitCandidate {
						candidates[site.PC] = true
					}
				}
			}
		}
		r.Sites = append(r.Sites, *site)
	}
	sort.Slice(r.Sites, func(i, j int) bool {
		a, b := r.Sites[i], r.Sites[j]
		if a.PC != b.PC {
			return a.PC < b.PC
		}
		return a.LiveMX.M*2+a.LiveMX.X < b.LiveMX.M*2+b.LiveMX.X
	})
	r.UniqueSourceSites, r.SourceMXSites, r.FrameReviewSites, r.CandidateReviewSites, r.BudgetEntries = len(unique), len(r.Sites), len(review), len(candidates), len(budgets)
	r.ShapeSourceSites = make(map[string]int)
	for shape, sites := range shapes {
		r.ShapeSourceSites[shape] = len(sites)
	}
	r.HLEAffectedSites = len(hooks)
	return r
}

func writeShadowReturnAudit(output io.Writer, r ShadowReturnAudit, verbose bool) {
	fmt.Fprintf(output, "return-frame audit (report-only): %d decoded contexts -> %d source sites / %d source-M/X sites; non-entry/unknown frame review=%d, legacy-local-exit candidates=%d; budget entries=%d (not runtime failures)\n", r.RawReturnContexts, r.UniqueSourceSites, r.SourceMXSites, r.FrameReviewSites, r.CandidateReviewSites, r.BudgetEntries)
	fmt.Fprintf(output, "  return shapes (overlapping source counts): pushed=%d mixed=%d past-entry=%d written-entry=%d unknown=%d interrupt=%d; HLE-affected=%d\n", r.ShapeSourceSites["locally_pushed_frame"], r.ShapeSourceSites["mixed_local_and_entry_frame"], r.ShapeSourceSites["past_entry_frame"], r.ShapeSourceSites["entry_frame_written"], r.ShapeSourceSites["unknown_stack_position"], r.ShapeSourceSites["interrupt_frame_not_normal_call_exit"], r.HLEAffectedSites)
	if !verbose {
		return
	}
	for _, site := range r.Sites {
		for _, c := range site.Contexts {
			if len(c.Shapes) == 1 && c.Shapes[0] == "entry_frame_position_only" && !c.Incomplete {
				continue
			}
			fmt.Fprintf(output, "[RETURN-FRAME] %s %s %s M%dX%d entry=%s M%dX%d depths=%v unknown=%t shapes=%v legacy-candidate=%t incomplete=%t obligations=%v\n", shadowAddress(site.PC), site.InstructionBytes, site.Mnemonic, site.LiveMX.M, site.LiveMX.X, shadowAddress(c.EntryPC), c.EntryMX.M, c.EntryMX.X, c.Depths, c.UnknownDepth, c.Shapes, c.LegacyExitCandidate, c.Incomplete, c.Obligations)
		}
	}
}
