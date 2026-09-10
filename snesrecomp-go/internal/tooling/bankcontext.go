package tooling

import (
	"fmt"
	"io"
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

type shadowDBRecipe struct {
	end     *uint16
	regions []decoder.DataRegion
	exitMX  map[decoder.Variant]decoder.MX
}

type ShadowBankQuery struct {
	PC       uint32                `json:"pc"`
	EntryPC  uint32                `json:"entry_pc"`
	EntryMX  analysis.MXState      `json:"entry_mx"`
	Status   string                `json:"status"`
	Banks    []uint16              `json:"constant_banks,omitempty"`
	Calls    []ShadowBankCallCheck `json:"call_checks,omitempty"`
	Blockers []ShadowBankBlocker   `json:"blockers,omitempty"`
}

type ShadowBankContext struct {
	Local            ShadowBankQuery   `json:"local_query"`
	Callers          []ShadowBankQuery `json:"direct_caller_inputs,omitempty"`
	Scope            string            `json:"scope"`
	Programs         int               `json:"summary_program_count"`
	ProgramBudgetHit bool              `json:"program_budget_hit,omitempty"`
	CallersTruncated bool              `json:"callers_truncated,omitempty"`
	Obligations      []string          `json:"proof_obligations"`
}

const shadowDBProgramLimit = 256
const shadowDBCallerLimit = 32

func shadowBankQuery(p *shadowDBProgram, summaries map[decoder.Variant]shadowDBSummary, hle map[uint32]bool, target decoder.Variant) ShadowBankQuery {
	q := ShadowBankQuery{PC: target.Address, Status: "blocked"}
	if p != nil {
		q.EntryPC = p.entry.Address
		q.EntryMX = analysis.MXState{M: p.entry.M, X: p.entry.X}
	}
	r := evaluateShadowDB(p, summaries, hle, &target)
	q.Blockers = r.summary.blockers
	// Worklist joins can encounter the same call in several abstract states.
	seen := make(map[string]bool)
	for _, c := range r.checks {
		id := shadowStoredJSONKey(c)
		if !seen[id] {
			seen[id] = true
			q.Calls = append(q.Calls, c)
		}
	}
	sort.Slice(q.Calls, func(i, j int) bool {
		if q.Calls[i].PC != q.Calls[j].PC {
			return q.Calls[i].PC < q.Calls[j].PC
		}
		return shadowStoredJSONKey(q.Calls[i]) < shadowStoredJSONKey(q.Calls[j])
	})
	if !r.summary.valid {
		return q
	}
	q.Status = "local_constant_set"
	entry, unknown := false, false
	for _, v := range r.values {
		if v == shadowDBEntry {
			entry = true
		} else if v < 0 || v > 255 {
			unknown = true
		} else {
			q.Banks = append(q.Banks, uint16(v))
		}
	}
	if entry {
		q.Status = "depends_on_entry_DB"
	}
	if unknown {
		q.Status = "unknown_DB_value"
	}
	// Mixed entry/unknown states never become a closed bank set.
	if q.Status != "local_constant_set" {
		q.Banks = nil
	}
	return q
}

func attachShadowBankContexts(image romimage.Image, banks []shadowBank, results []shadowDecodeResult, sites map[uint32]*ShadowDispatchSite) {
	type request struct {
		call   *ShadowEntryCallInput
		target decoder.Variant
		entry  decoder.Variant
	}
	var requests []request
	var visit func(*ShadowInitializerRead)
	visit = func(read *ShadowInitializerRead) {
		if local := read.Index.LocalSource; local != nil {
			for i := range local.Calls {
				c := &local.Calls[i]
				if c.SourceEvidence != nil && c.SourceEvidence.Table != nil {
					t := c.SourceEvidence.Table
					requests = append(requests, request{c, decoder.Variant{Address: t.LoadPC, M: t.LoadMX.M, X: t.LoadMX.X}, decoder.Variant{Address: c.CallerEntryPC, M: c.CallerEntryMX.M, X: c.CallerEntryMX.X}})
				}
			}
		}
		if read.PointerSource != nil {
			visit(read.PointerSource)
		}
	}
	for _, site := range sites {
		for _, p := range site.PointerProducers {
			if e := p.IndexEvidence; e != nil {
				for i := range e.Initializers {
					visit(&e.Initializers[i].Read)
				}
			}
		}
	}
	if len(requests) == 0 {
		return
	}
	sort.Slice(requests, func(i, j int) bool {
		return shadowStoredJSONKey(requests[i].call) < shadowStoredJSONKey(requests[j].call)
	})
	configs := make(map[byte]*config.Config)
	hle := make(map[uint32]bool)
	for _, b := range banks {
		configs[b.ID] = b.Config
		for pc := range b.Config.HLEFunctions {
			hle[decoder.Address24(b.ID, pc)] = true
		}
		for pc := range b.Config.HLEFunctionsIf {
			hle[decoder.Address24(b.ID, pc)] = true
		}
		for pc := range b.Config.HLEDispatch {
			hle[decoder.Address24(b.ID, pc)] = true
		}
		for _, pc := range b.Config.HLESPCUpload {
			hle[decoder.Address24(b.ID, pc)] = true
		}
	}
	byEntry := make(map[decoder.Variant]shadowDecodeResult)
	siblings := make(map[byte]map[uint16]struct{})
	parents := make(map[uint32][]ShadowEntryCallInput)
	for _, r := range results {
		byEntry[r.entry] = r
		b := byte(r.entry.Address >> 16)
		if siblings[b] == nil {
			siblings[b] = make(map[uint16]struct{})
		}
		siblings[b][uint16(r.entry.Address)] = struct{}{}
		for _, c := range r.callInputs {
			parents[c.call.TargetPC] = append(parents[c.call.TargetPC], c.call)
		}
	}
	for pc, entries := range parents {
		sort.Slice(entries, func(i, j int) bool { return shadowStoredJSONKey(entries[i]) < shadowStoredJSONKey(entries[j]) })
		parents[pc] = slices.CompactFunc(entries, func(a, b ShadowEntryCallInput) bool { return shadowStoredJSONKey(a) == shadowStoredJSONKey(b) })
	}
	// Re-decode only existing exact variants needed by this report query.
	// This avoids retaining every graph or adding work to unrelated games.
	programs := make(map[decoder.Variant]*shadowDBProgram)
	seen := make(map[decoder.Variant]bool)
	var queue []decoder.Variant
	for _, request := range requests {
		queue = append(queue, request.entry)
		for _, p := range parents[request.entry.Address][:min(len(parents[request.entry.Address]), shadowDBCallerLimit)] {
			queue = append(queue, decoder.Variant{Address: p.CallerEntryPC, M: p.CallerEntryMX.M, X: p.CallerEntryMX.X})
		}
	}
	budgetHit := false
	for len(queue) > 0 {
		v := queue[0]
		queue = queue[1:]
		if seen[v] {
			continue
		}
		seen[v] = true
		r, ok := byEntry[v]
		if !ok || r.issue != nil || r.bankRecipe == nil {
			continue
		}
		if len(programs) >= shadowDBProgramLimit {
			budgetHit = true
			continue
		}
		b := byte(v.Address >> 16)
		cfg := configs[b]
		if cfg == nil {
			continue
		}
		own := make(map[uint16]struct{})
		for pc := range siblings[b] {
			if pc != uint16(v.Address) {
				own[pc] = struct{}{}
			}
		}
		g, err := decoder.DecodeFunction(image, b, uint16(v.Address), v.M, v.X, decoder.Options{End: r.bankRecipe.end, DataRegions: r.bankRecipe.regions, CalleeExitMX: r.bankRecipe.exitMX, HLEDispatch: cfg.HLEDispatch, SiblingEntryPCs: own})
		if err != nil {
			continue
		}
		p := collectShadowDBProgram(image, g)
		programs[v] = p
		if hle[v.Address] {
			continue
		}
		for _, n := range p.nodes {
			if n.call != nil && n.call.closed {
				queue = append(queue, n.call.targets...)
			}
		}
	}
	summaries := buildShadowDBSummaries(programs, hle)
	// A decoded entry omitted by the bounded demand query is unresolved,
	// not a missing generated body. Keep those diagnoses distinct.
	for v := range byEntry {
		if _, ok := summaries[v]; !ok {
			summaries[v] = shadowDBSummary{blockers: []ShadowBankBlocker{{PC: v.Address, Reason: "summary_program_not_loaded_or_decode_failed"}}}
		}
	}
	for _, request := range requests {
		context := &ShadowBankContext{Local: shadowBankQuery(programs[request.entry], summaries, hle, request.target), Scope: "decoded_MX_normal_call_stack_contract_direct_callers_only", Programs: len(programs), ProgramBudgetHit: budgetHit,
			Obligations: []string{"decoded_width_and_code_ownership", "normal_return_frame_and_stack_lifetime", "interrupt_and_external_entry_contracts", "direct_callers_not_all_entry_paths", "no_bank_substitution_samples_roots_or_HLE_changes"}}
		incoming := parents[request.entry.Address]
		context.CallersTruncated = len(incoming) > shadowDBCallerLimit
		for _, c := range incoming[:min(len(incoming), shadowDBCallerLimit)] {
			// No canonical M/X selection: a caller with a different live
			// entry width cannot supply this exact entry interpretation.
			if c.CallMX.M != request.entry.M || c.CallMX.X != request.entry.X {
				continue
			}
			entry := decoder.Variant{Address: c.CallerEntryPC, M: c.CallerEntryMX.M, X: c.CallerEntryMX.X}
			context.Callers = append(context.Callers, shadowBankQuery(programs[entry], summaries, hle, decoder.Variant{Address: c.CallPC, M: c.CallMX.M, X: c.CallMX.X}))
		}
		request.call.SourceEvidence.BankContext = context
	}
}

func writeShadowBankContext(out io.Writer, context *ShadowBankContext, indent string) {
	if context == nil {
		return
	}
	fmt.Fprintf(out, "%sbank-context (report-only): programs=%d budget-hit=%t scope=%s\n", indent, context.Programs, context.ProgramBudgetHit, context.Scope)
	for _, q := range append([]ShadowBankQuery{context.Local}, context.Callers...) {
		fmt.Fprintf(out, "%sentry=%s M%dX%d at=%s status=%s banks=%v\n", indent, shadowAddress(q.EntryPC), q.EntryMX.M, q.EntryMX.X, shadowAddress(q.PC), q.Status, q.Banks)
		for _, c := range q.Calls {
			fmt.Fprintf(out, "%s  call=%s targets=%d closed=%t preserving=%d status=%s blockers=%v truncated=%t\n", indent, shadowAddress(c.PC), c.TargetCount, c.TargetSetClosed, c.PreservingCount, c.Status, c.Blockers, c.BlockersTruncated)
		}
		for _, b := range q.Blockers {
			fmt.Fprintf(out, "%s  blocked=%s reason=%s\n", indent, shadowAddress(b.PC), b.Reason)
		}
	}
	fmt.Fprintf(out, "%sobligations=%v callers-truncated=%t\n", indent, context.Obligations, context.CallersTruncated)
}
