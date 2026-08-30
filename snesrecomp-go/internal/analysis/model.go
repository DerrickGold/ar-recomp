// Package analysis defines game-agnostic evidence and comparison records used
// by the recompiler's read-only analysis tools.  These types deliberately do
// not depend on generated C or on a runtime interpreter.
package analysis

import "sort"

// EntryKind describes why an address may be entered.  It is separate from the
// transfer that reaches the address: a computed handler can be called, tail
// entered, or resumed depending on the dispatch construct that owns it.
type EntryKind string

const (
	EntryResetInterrupt EntryKind = "reset_interrupt"
	EntryRoutine        EntryKind = "routine"
	EntryTailTarget     EntryKind = "tail_target"
	EntryComputed       EntryKind = "computed_handler"
	EntryContinuation   EntryKind = "continuation"
)

// TransferKind describes host-stack and emulated-stack intent at an edge.
type TransferKind string

const (
	TransferCall      TransferKind = "call"
	TransferTail      TransferKind = "tail"
	TransferResume    TransferKind = "resume"
	TransferInterrupt TransferKind = "interrupt"
)

type Confidence string

const (
	ConfidenceObserved    Confidence = "observed"
	ConfidenceProven      Confidence = "proven"
	ConfidenceProbable    Confidence = "probable"
	ConfidenceSpeculative Confidence = "speculative"
	ConfidenceAuthored    Confidence = "authored_override"
)

// CodeOwnership classifies the decode path that produced a fact. An empty
// value means ownership has not yet been classified.
type CodeOwnership string

const (
	OwnershipGarbageOnly CodeOwnership = "garbage_only"
)

type MXState struct {
	M uint8 `json:"m"`
	X uint8 `json:"x"`
}

// EntryVariant identifies one exact generated entry. Continuation facts use
// RegionOwners to state which active generated regions may resume at the
// continuation without creating another host activation frame.
type EntryVariant struct {
	PC      uint32  `json:"pc"`
	EntryMX MXState `json:"entry_mx"`
}

// EntryTemplatePlacement preserves the bank-local authored ordering of a
// removable canonical template. It affects deterministic C layout only; it is
// not control-flow evidence and cannot create an entry without an EntryFact.
type EntryTemplatePlacement struct {
	EntryVariant
	BankOrdinal int `json:"bank_ordinal"`
}

// EntryEdge identifies one exact live-width control-flow edge. ResumeEdges
// preserve the boundary crossing independently of whether generation lowers
// it through the registry, an in-region goto, or a shared no-activation body.
// RegionOwner is required when one continuation has multiple owners.
type EntryEdge struct {
	Source      EntryVariant  `json:"source"`
	Target      EntryVariant  `json:"target"`
	RegionOwner *EntryVariant `json:"region_owner,omitempty"`
}

type Evidence struct {
	Source     string     `json:"source"`
	Confidence Confidence `json:"confidence"`
	Detail     string     `json:"detail,omitempty"`
}

// EntryFact states that one exact address/width variant has a statically
// established entry role. Unlike a configured func declaration, this fact
// does not by itself make the variant a decode root: generation may withhold
// the root and require an independently decoded edge to demand it.
type EntryFact struct {
	PC                uint32         `json:"pc"`
	EntryMX           MXState        `json:"entry_mx"`
	Kind              EntryKind      `json:"kind"`
	TemplateFree      bool           `json:"template_free,omitempty"`
	CanonicalPromoted bool           `json:"canonical_promoted,omitempty"`
	RegionOwners      []EntryVariant `json:"region_owners,omitempty"`
	ResumeEdges       []EntryEdge    `json:"resume_edges,omitempty"`
	Evidence          []Evidence     `json:"evidence,omitempty"`
}

func (fact *EntryFact) Normalize() {
	fact.PC &= 0xffffff
	fact.EntryMX.M &= 1
	fact.EntryMX.X &= 1
	for index := range fact.RegionOwners {
		fact.RegionOwners[index].PC &= 0xffffff
		fact.RegionOwners[index].EntryMX.M &= 1
		fact.RegionOwners[index].EntryMX.X &= 1
	}
	sort.Slice(fact.RegionOwners, func(i, j int) bool {
		if fact.RegionOwners[i].PC != fact.RegionOwners[j].PC {
			return fact.RegionOwners[i].PC < fact.RegionOwners[j].PC
		}
		if fact.RegionOwners[i].EntryMX.M != fact.RegionOwners[j].EntryMX.M {
			return fact.RegionOwners[i].EntryMX.M < fact.RegionOwners[j].EntryMX.M
		}
		return fact.RegionOwners[i].EntryMX.X < fact.RegionOwners[j].EntryMX.X
	})
	fact.RegionOwners = dedupeOrdered(fact.RegionOwners)
	for index := range fact.ResumeEdges {
		if fact.ResumeEdges[index].RegionOwner != nil {
			fact.ResumeEdges[index].RegionOwner.PC &= 0xffffff
			fact.ResumeEdges[index].RegionOwner.EntryMX.M &= 1
			fact.ResumeEdges[index].RegionOwner.EntryMX.X &= 1
		}
		fact.ResumeEdges[index].Source.PC &= 0xffffff
		fact.ResumeEdges[index].Source.EntryMX.M &= 1
		fact.ResumeEdges[index].Source.EntryMX.X &= 1
		fact.ResumeEdges[index].Target.PC &= 0xffffff
		fact.ResumeEdges[index].Target.EntryMX.M &= 1
		fact.ResumeEdges[index].Target.EntryMX.X &= 1
	}
	sort.Slice(fact.ResumeEdges, func(i, j int) bool {
		left, right := fact.ResumeEdges[i], fact.ResumeEdges[j]
		if compared := compareOptionalEntryVariant(left.RegionOwner, right.RegionOwner); compared != 0 {
			return compared < 0
		}
		if left.Source.PC != right.Source.PC {
			return left.Source.PC < right.Source.PC
		}
		if left.Source.EntryMX.M != right.Source.EntryMX.M {
			return left.Source.EntryMX.M < right.Source.EntryMX.M
		}
		if left.Source.EntryMX.X != right.Source.EntryMX.X {
			return left.Source.EntryMX.X < right.Source.EntryMX.X
		}
		if left.Target.PC != right.Target.PC {
			return left.Target.PC < right.Target.PC
		}
		if left.Target.EntryMX.M != right.Target.EntryMX.M {
			return left.Target.EntryMX.M < right.Target.EntryMX.M
		}
		return left.Target.EntryMX.X < right.Target.EntryMX.X
	})
	dedupedEdges := fact.ResumeEdges[:0]
	for _, edge := range fact.ResumeEdges {
		if len(dedupedEdges) == 0 || !entryEdgesEqual(dedupedEdges[len(dedupedEdges)-1], edge) {
			dedupedEdges = append(dedupedEdges, edge)
		}
	}
	fact.ResumeEdges = dedupedEdges
	sort.Slice(fact.Evidence, func(i, j int) bool {
		if fact.Evidence[i].Source != fact.Evidence[j].Source {
			return fact.Evidence[i].Source < fact.Evidence[j].Source
		}
		if fact.Evidence[i].Confidence != fact.Evidence[j].Confidence {
			return fact.Evidence[i].Confidence < fact.Evidence[j].Confidence
		}
		return fact.Evidence[i].Detail < fact.Evidence[j].Detail
	})
	fact.Evidence = dedupeOrdered(fact.Evidence)
}

func compareOptionalEntryVariant(left, right *EntryVariant) int {
	if left == nil {
		if right == nil {
			return 0
		}
		return -1
	}
	if right == nil {
		return 1
	}
	if left.PC != right.PC {
		if left.PC < right.PC {
			return -1
		}
		return 1
	}
	if left.EntryMX.M != right.EntryMX.M {
		if left.EntryMX.M < right.EntryMX.M {
			return -1
		}
		return 1
	}
	if left.EntryMX.X != right.EntryMX.X {
		if left.EntryMX.X < right.EntryMX.X {
			return -1
		}
		return 1
	}
	return 0
}

func entryEdgesEqual(left, right EntryEdge) bool {
	return compareOptionalEntryVariant(left.RegionOwner, right.RegionOwner) == 0 &&
		left.Source == right.Source && left.Target == right.Target
}

// DispatchFact is a normalized statement about one runtime-target site.
// Targets contains a closed, proven set only when TargetSetClosed is true.
// TargetCandidates may hold an independently recovered prefix when its bound
// has not yet been proven.
type DispatchFact struct {
	SitePC           uint32        `json:"site_pc"`
	FunctionEntries  []uint32      `json:"function_entries,omitempty"`
	InstructionBytes string        `json:"instruction_bytes,omitempty"`
	Mnemonic         string        `json:"mnemonic,omitempty"`
	AddressingMode   string        `json:"addressing_mode,omitempty"`
	LiveMX           []MXState     `json:"live_mx,omitempty"`
	Transfer         TransferKind  `json:"transfer"`
	TargetEntryKind  EntryKind     `json:"target_entry_kind"`
	Targets          []uint32      `json:"targets,omitempty"`
	TargetCandidates []uint32      `json:"target_candidates,omitempty"`
	TargetSetClosed  bool          `json:"target_set_closed"`
	IndexRegister    string        `json:"index_register,omitempty"`
	TableBases       []uint32      `json:"table_bases,omitempty"`
	TableEntryBytes  uint8         `json:"table_entry_bytes,omitempty"`
	ReturnPC         *uint32       `json:"return_pc,omitempty"`
	SEPMask          byte          `json:"sep_mask,omitempty"`
	UnknownFields    []string      `json:"unknown_fields,omitempty"`
	Evidence         []Evidence    `json:"evidence,omitempty"`
	CodeOwnership    CodeOwnership `json:"code_ownership,omitempty"`
}

func (fact *DispatchFact) Normalize() {
	fact.SitePC &= 0xffffff
	for index := range fact.FunctionEntries {
		fact.FunctionEntries[index] &= 0xffffff
	}
	for index := range fact.Targets {
		fact.Targets[index] &= 0xffffff
	}
	for index := range fact.TargetCandidates {
		fact.TargetCandidates[index] &= 0xffffff
	}
	for index := range fact.TableBases {
		fact.TableBases[index] &= 0xffffff
	}
	if fact.ReturnPC != nil {
		value := *fact.ReturnPC & 0xffffff
		fact.ReturnPC = &value
	}
	sort.Slice(fact.FunctionEntries, func(i, j int) bool { return fact.FunctionEntries[i] < fact.FunctionEntries[j] })
	sort.Slice(fact.LiveMX, func(i, j int) bool {
		if fact.LiveMX[i].M != fact.LiveMX[j].M {
			return fact.LiveMX[i].M < fact.LiveMX[j].M
		}
		return fact.LiveMX[i].X < fact.LiveMX[j].X
	})
	if fact.TargetEntryKind == EntryContinuation {
		sort.Slice(fact.Targets, func(i, j int) bool { return fact.Targets[i] < fact.Targets[j] })
		sort.Slice(fact.TargetCandidates, func(i, j int) bool { return fact.TargetCandidates[i] < fact.TargetCandidates[j] })
		fact.Targets = dedupeOrdered(fact.Targets)
		fact.TargetCandidates = dedupeOrdered(fact.TargetCandidates)
	}
	sort.Slice(fact.Evidence, func(i, j int) bool {
		if fact.Evidence[i].Source != fact.Evidence[j].Source {
			return fact.Evidence[i].Source < fact.Evidence[j].Source
		}
		if fact.Evidence[i].Confidence != fact.Evidence[j].Confidence {
			return fact.Evidence[i].Confidence < fact.Evidence[j].Confidence
		}
		return fact.Evidence[i].Detail < fact.Evidence[j].Detail
	})
	sort.Strings(fact.UnknownFields)
	fact.FunctionEntries = dedupeOrdered(fact.FunctionEntries)
	fact.LiveMX = dedupeMX(fact.LiveMX)
	fact.Evidence = dedupeOrdered(fact.Evidence)
	fact.UnknownFields = dedupeOrdered(fact.UnknownFields)
}

func (fact DispatchFact) FieldUnknown(field string) bool {
	index := sort.SearchStrings(fact.UnknownFields, field)
	return index < len(fact.UnknownFields) && fact.UnknownFields[index] == field
}

func dedupeOrdered[T comparable](values []T) []T {
	if len(values) < 2 {
		return values
	}
	result := values[:1]
	for _, value := range values[1:] {
		if value != result[len(result)-1] {
			result = append(result, value)
		}
	}
	return result
}

func dedupeMX(values []MXState) []MXState {
	if len(values) < 2 {
		return values
	}
	result := values[:1]
	for _, value := range values[1:] {
		if value != result[len(result)-1] {
			result = append(result, value)
		}
	}
	return result
}
