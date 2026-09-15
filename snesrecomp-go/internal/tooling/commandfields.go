package tooling

import (
	"encoding/json"
	"fmt"
	"io"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// Field spellings establish candidate relationships, not object identity,
// alias equality, lifetime, control-flow reachability or observed targets.
type ShadowDeferredField struct {
	Native           decoder.ForwardedIndirectField `json:"native_flow"`
	Contexts         []analysis.EntryVariant        `json:"decoded_in"`
	ProofObligations []string                       `json:"proof_obligations"`
}

type ShadowDeferredCommandOperand struct {
	LoadPC           uint32                `json:"load_pc"`
	OperandOffset    int                   `json:"offset_from_command_entry_y"`
	Width            int                   `json:"width_bytes"`
	StreamBankSource string                `json:"stream_bank_source"`
	StreamBank       *uint8                `json:"stream_bank,omitempty"`
	StorePC          uint32                `json:"store_pc"`
	StoreMode        string                `json:"store_mode"`
	StoreOperand     uint16                `json:"store_operand"`
	Consumers        []ShadowDeferredField `json:"conditional_consumers,omitempty"`
	ProofObligations []string              `json:"proof_obligations"`
}

func deferredFieldObligations() []string {
	return []string{"matching_field_spelling_not_must_alias", "D_DB_and_object_index_equality", "field_lifetime_and_intervening_writes", "scratch_stack_alias_and_native_path_feasibility", "independently_rooted_stream_and_cursor", "native_HLE_and_live_MX_return_contracts", "no_closed_target_or_reachability_claim"}
}

func mergeShadowDeferredFields(results []shadowDecodeResult) []ShadowDeferredField {
	byShape := make(map[string]*ShadowDeferredField)
	for _, r := range results {
		if r.issue != nil {
			continue
		}
		for _, f := range r.forwardedFields {
			key, _ := json.Marshal(f)
			item := byShape[string(key)]
			if item == nil {
				item = &ShadowDeferredField{Native: f, ProofObligations: deferredFieldObligations()}
				byShape[string(key)] = item
			}
			item.Contexts = append(item.Contexts, analysis.EntryVariant{PC: r.entry.Address, EntryMX: analysis.MXState{M: r.entry.M, X: r.entry.X}})
		}
	}
	var keys []string
	for key := range byShape {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	var out []ShadowDeferredField
	for _, key := range keys {
		item := byShape[key]
		item.Contexts = normalizeShadowCommandContexts(item.Contexts)
		out = append(out, *item)
	}
	return out
}

func linkDeferredCommandOperands(streams []ShadowCommandStream, fields []ShadowDeferredField) []ShadowCommandStream {
	for i := range streams {
		for j := range streams[i].Commands {
			p := streams[i].Commands[j].Deferred
			if p == nil {
				continue
			}
			// Do not mutate input summaries: different selector shapes can share
			// a command prefix but not necessarily the same correlation evidence.
			copy := *p
			copy.Consumers = nil
			for _, f := range fields {
				if f.Native.Mode == p.StoreMode && f.Native.Operand == p.StoreOperand {
					copy.Consumers = append(copy.Consumers, f)
				}
			}
			streams[i].Commands[j].Deferred = &copy
		}
	}
	return streams
}

func writeShadowDeferredFields(out io.Writer, fields []ShadowDeferredField) {
	for _, f := range fields {
		n := f.Native
		fmt.Fprintf(out, "[DEFERRED-FIELD] load=%s %s $%04X -> scratch=%s $%04X -> dispatch=%s ($%04X) required-D=$%04X M%dX%d target-bank=live-PB (decoded=$%02X); conditional, not a closed target set\n", shadowAddress(n.LoadPC), n.Mode, n.Operand, shadowAddress(n.StorePC), n.ScratchOperand, shadowAddress(n.DispatchPC), n.PointerAddress, n.RequiredD, n.M, n.X, n.ProgramBank)
	}
}
