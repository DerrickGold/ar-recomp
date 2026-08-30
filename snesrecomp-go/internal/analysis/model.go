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
	PC                uint32     `json:"pc"`
	EntryMX           MXState    `json:"entry_mx"`
	Kind              EntryKind  `json:"kind"`
	TemplateFree      bool       `json:"template_free,omitempty"`
	CanonicalPromoted bool       `json:"canonical_promoted,omitempty"`
	Evidence          []Evidence `json:"evidence,omitempty"`
}

func (fact *EntryFact) Normalize() {
	fact.PC &= 0xffffff
	fact.EntryMX.M &= 1
	fact.EntryMX.X &= 1
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
