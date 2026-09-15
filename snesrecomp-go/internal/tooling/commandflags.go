package tooling

import (
	"slices"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// These are native decoded-path contracts, not global entry status summaries.
// Entry-preserved flags may only be substituted from the SAME value call chain.
type ShadowCommandInputFlags struct {
	Carry   ShadowStoredFlag `json:"carry"`
	Decimal ShadowStoredFlag `json:"decimal"`
}

type ShadowCommandArithmetic struct {
	Context   analysis.EntryVariant `json:"context"`
	Operation ShadowStoredOperation `json:"resolved_operation"`
}

type ShadowCommandStatusDependency struct {
	Context    analysis.EntryVariant  `json:"context"`
	Call       ShadowNativeStatusCall `json:"call"`
	ReturnKind string                 `json:"return_kind"`
	Successors []decoder.DecodeKey    `json:"decoded_successors"`
	Before     *ShadowStoredFlag      `json:"flag_before_call"`
}

func shadowCommandFlag(w shadowPointerWalk, at decoder.DecodeKey, mask byte) ShadowStoredFlag {
	budget := shadowPointerWalkLimit
	return shadowCommandFlagBefore(w, at, mask, 0, &budget)
}

func shadowCommandFlagBefore(w shadowPointerWalk, at decoder.DecodeKey, mask byte, depth int, budget *int) ShadowStoredFlag {
	seen := make(map[decoder.DecodeKey]bool)
	for *budget > 0 {
		*budget--
		if at == w.graph.Entry {
			return ShadowStoredFlag{Reason: "preserved_entry_flag"}
		}
		if seen[at] {
			return ShadowStoredFlag{Reason: "input_flag_predecessor_cycle"}
		}
		seen[at] = true
		prev := w.previous(at)
		if prev == nil || prev.Instruction == nil {
			return ShadowStoredFlag{Reason: "input_flag_ambiguous_predecessor"}
		}
		if flag, stop := shadowStoredFlagEffect(prev, mask); stop {
			if depth < shadowStatusCallDepth && prev.Instruction.DispatchKind == "" && prev.Instruction.DispatchEntries == nil {
				if target, kind, ok := shadowStatusDirectCall(prev); ok {
					before := shadowCommandFlagBefore(w, prev.Key, mask, depth+1, budget)
					flag.NativeCall = &ShadowCommandStatusDependency{Context: analysis.EntryVariant{PC: w.graph.Entry.PC, EntryMX: analysis.MXState{M: w.graph.Entry.M, X: w.graph.Entry.X}}, Call: ShadowNativeStatusCall{SitePC: prev.Key.PC, TargetPC: target, MX: analysis.MXState{M: prev.Key.M, X: prev.Key.X}}, ReturnKind: kind, Successors: slices.Clone(prev.Successors), Before: &before}
				}
			}
			return flag
		}
		at = prev.Key
	}
	return ShadowStoredFlag{Reason: "input_flag_predecessor_budget"}
}

func shadowCommandFlags(w shadowPointerWalk, at decoder.DecodeKey) *ShadowCommandInputFlags {
	return &ShadowCommandInputFlags{Carry: shadowCommandFlag(w, at, 1), Decimal: shadowCommandFlag(w, at, 8)}
}

// No cross-product of flag sources and argument sources. Only a call chain
// whose innermost target matches this context can establish an entry flag.
func resolveShadowCommandFlag(ctx analysis.EntryVariant, flag *ShadowStoredFlag, mask byte, calls []ShadowCommandRootCall, sameEntry func(uint32, analysis.MXState, analysis.EntryVariant) bool) *ShadowStoredFlag {
	if flag == nil || flag.Value != nil || flag.Reason != "preserved_entry_flag" {
		return flag
	}
	result := *flag
	finish := func() *ShadowStoredFlag {
		if flag.NativeCall == nil {
			return &result
		}
		combined := result
		combined.NativeCall, combined.EntrySource = flag.NativeCall, &result
		return &combined
	}
	for _, call := range slices.Backward(calls) {
		if call.Flags == nil || !sameEntry(call.TargetPC, call.MX, ctx) {
			break
		}
		result = call.Flags.Carry
		if mask == 8 {
			result = call.Flags.Decimal
		}
		if result.Value != nil {
			result.Reason = "correlated_call_path_constant"
			return finish()
		}
		if result.Reason != "preserved_entry_flag" {
			return finish()
		}
		ctx = call.Caller
	}
	result.Value = nil
	result.Reason = "input_entry_flag_unproven"
	return finish()
}

func (a *shadowStatusAnalyzer) resolveFlag(flag ShadowStoredFlag, mask byte) ShadowStoredFlag {
	dep := flag.NativeCall
	if dep == nil {
		return flag
	}
	copyDep := *dep
	summary := a.summarize(dep.Call.TargetPC, dep.Call.MX, dep.ReturnKind)
	copyDep.Call.Summary = summary
	flag.NativeCall = &copyDep
	if summary.Reason != "" {
		flag.Reason = summary.Reason
		return flag
	}
	if !shadowStatusReturnMXCovered(summary, dep.Successors) {
		flag.Reason = "native_status_return_MX_mismatch"
		return flag
	}
	effect := summary.Carry
	if mask == 8 {
		effect = summary.Decimal
	}
	if effect == "clear" || effect == "set" {
		v := uint8(0)
		if effect == "set" {
			v = 1
		}
		flag.Value, flag.DefinitionPC, flag.Reason = &v, dep.Call.SitePC, "native_callee_constant"
		return flag
	}
	if effect != "preserve" || dep.Before == nil {
		flag.Reason = "native_callee_flag_unknown"
		return flag
	}
	before := a.resolveFlag(*dep.Before, mask)
	copyDep.Before = &before
	flag.Value, flag.DefinitionPC, flag.Reason = before.Value, before.DefinitionPC, before.Reason
	return flag
}

func (a *shadowStatusAnalyzer) resolveFlags(flags *ShadowCommandInputFlags) *ShadowCommandInputFlags {
	if flags == nil {
		return nil
	}
	return &ShadowCommandInputFlags{Carry: a.resolveFlag(flags.Carry, 1), Decimal: a.resolveFlag(flags.Decimal, 8)}
}
