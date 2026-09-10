package tooling

import (
	"fmt"
	"io"

	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// One additional source hop for a direct-call argument. A caller's table
// index is distinct from the loaded argument and the callee's own table index.
// None of these expressions or writer candidates is substituted into another.
type ShadowCallerSource struct {
	Field       *ShadowStoredField         `json:"field,omitempty"`
	Writers     []ShadowStoredTargetWriter `json:"writer_candidates,omitempty"`
	Table       *ShadowInitializerRead     `json:"table_read,omitempty"`
	LocalSlot   *ShadowLocalSlotSource     `json:"local_slot_source,omitempty"`
	WriterMatch string                     `json:"writer_match"`
	Scope       string                     `json:"expansion_scope"`
	Obligations []string                   `json:"proof_obligations"`
	BankContext *ShadowBankContext         `json:"bank_context,omitempty"`
}

func (walk shadowPointerWalk) callerSource(value ShadowInitializerIndex) *ShadowCallerSource {
	if value.Source.Kind != "load" {
		return nil
	}
	// This hop never recursively follows another table pointer or direct
	// caller. Its local index/bank/slot queries have their existing budgets.
	table := walk.initializerRead(value.sourceKey, false)
	if table == nil && value.Field == nil {
		return nil
	}
	r := &ShadowCallerSource{Field: value.Field, Table: table,
		WriterMatch: "same_address_expression_across_program_banks_not_aliases",
		Scope:       "one_caller_source_hop_no_recursive_expansion",
		Obligations: []string{"decoded_source_not_reachability", "D_DB_index_wrap_alias_and_lifetime", "writer_inventory_not_complete_domain", "caller_input_not_callee_index", "table_extent_and_data_ownership", "no_sample_or_writer_value_substitution", "no_root_or_HLE_policy_change"}}
	if value.Field != nil && table == nil {
		r.LocalSlot = walk.localSlotSource(value.sourceKey, *value.Field)
	}
	return r
}

func cloneShadowCallerSource(source *ShadowCallerSource) *ShadowCallerSource {
	if source == nil {
		return nil
	}
	copy := *source
	if source.Table != nil {
		table := *source.Table
		copy.Table = &table
		// Raw per-graph records have no samples/writer annotations yet. These
		// slices must be owned by the particular published caller record.
		table.Index.Writers = nil
		table.Samples = nil
		table.SamplesTruncated = false
	}
	return &copy
}

func attachShadowCallerSources(image romimage.Image, results []shadowDecodeResult, sites map[uint32]*ShadowDispatchSite) {
	var sources []*ShadowCallerSource
	var reads []*ShadowInitializerRead
	wanted := make(map[ShadowStoredField]bool)
	var visit func(*ShadowInitializerRead)
	visit = func(read *ShadowInitializerRead) {
		if local := read.Index.LocalSource; local != nil {
			for i := range local.Calls {
				if source := local.Calls[i].SourceEvidence; source != nil {
					sources = append(sources, source)
					if source.Field != nil {
						wanted[*source.Field] = true
					}
					if source.Table != nil {
						reads = append(reads, source.Table)
					}
				}
			}
		}
		if read.PointerSource != nil {
			visit(read.PointerSource)
		}
	}
	for _, site := range sites {
		for _, producer := range site.PointerProducers {
			if e := producer.IndexEvidence; e != nil {
				for i := range e.Initializers {
					visit(&e.Initializers[i].Read)
				}
			}
		}
	}
	writers := shadowFieldWriterIndex(results, wanted)
	for _, source := range sources {
		if source.Field != nil {
			source.Writers = writers[*source.Field]
		}
	}
	attachInitializerReadSamples(image, results, reads)
}

func writeShadowCallerSource(out io.Writer, source *ShadowCallerSource, indent string) {
	if source == nil {
		return
	}
	fmt.Fprintf(out, "%scaller-source (report-only): scope=%s writer-match=%s writers=%d\n", indent, source.Scope, source.WriterMatch, len(source.Writers))
	for _, writer := range source.Writers {
		writeShadowStoredWriter(out, writer)
	}
	writeShadowLocalSlot(out, source.LocalSlot, indent+"  ")
	if source.Table != nil {
		fmt.Fprintf(out, "%scaller table index (distinct from argument/callee index):\n", indent)
		writeShadowInitializer(out, source.Table, indent+"  ")
	}
	writeShadowBankContext(out, source.BankContext, indent)
	fmt.Fprintf(out, "%sobligations=%v\n", indent, source.Obligations)
}
