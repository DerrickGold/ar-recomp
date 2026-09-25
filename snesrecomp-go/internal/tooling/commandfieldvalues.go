package tooling

import (
	"cmp"
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// Both invocations are independently rooted ROM commands. Equal normalized
// field offsets still do not establish object identity or a native return.
type ShadowCommandFieldInput struct {
	StoreInput  *ShadowCommandValueInput `json:"store_input"`
	ReloadInput *ShadowCommandValueInput `json:"reload_input"`
	BaseSlot    uint16                   `json:"object_pointer_dp_slot"`
	Offset      uint16                   `json:"normalized_field_offset"`
}

type coldFieldStore struct {
	selector, fetch uint32
	entryY          uint16
	publication     ShadowCommandCursorStore
	query           coldValueQuery
	stop            string
}
type coldFieldLoad struct {
	selector, fetch uint32
	reload          ShadowCommandCursorReload
	query           coldValueQuery
}

func (q *coldCommandValueQueries) fieldQuery(gi int, pc uint32, ctx int) (coldValueQuery, bool) {
	key := decoder.DecodeKey{PC: pc, M: 0, X: 0}
	query := coldValueQuery{coldValueSite{gi, key}, "X", ctx}
	if q.e.graphs[gi].Instructions[key] == nil {
		return query, false
	}
	f, exists := q.fieldQueries[query]
	if !exists {
		f = q.e.fieldIndex(query)
		q.fieldQueries[query] = f
	}
	return query, f.valid
}

func (q *coldCommandValueQueries) rebasedCursorRoots(wave []ShadowCommandWalk, reloads map[uint32][]ShadowCommandCursorReload) []ShadowCommandRoot {
	if q.e.failed {
		return nil
	}
	if q.fieldQueries == nil {
		q.fieldQueries = map[coldValueQuery]coldFieldIndex{}
		q.fieldStores = map[coldFieldStore]bool{}
		q.fieldLoads = map[coldFieldLoad]bool{}
	}
	for _, w := range wave {
		for _, s := range q.streams {
			if w.SelectorPC != s.SelectorPC {
				continue
			}
			for _, step := range w.Steps {
				y := int(uint16(step.FetchPC)) - s.TagByteOffset + 1 + s.EntryCursorDelta
				if step.Kind == "untagged_data" || y < 0 || y > 65535 {
					continue
				}
				off, ok := q.e.a.offset(step.Handler)
				if !ok {
					continue
				}
				for _, gi := range q.entries[off] {
					for _, ctx := range q.e.entrySeeds[gi] {
						seed := q.e.seeds[q.e.contexts[ctx].seed-1]
						if seed.y != uint16(y) || seed.bank != byte(step.FetchPC>>16) {
							continue
						}
						if p := step.NativePath; p != nil && p.EntryDecimalCondition != "set" {
							for _, store := range p.CursorPublications {
								if store.Mode != "dp,x" || store.Literal {
									continue
								}
								query, ok := q.fieldQuery(gi, store.PC, ctx)
								if ok {
									q.fieldStores[coldFieldStore{s.SelectorPC, step.FetchPC, uint16(y), store, query, p.Status}] = true
								}
							}
						}
						for _, load := range reloads[s.SelectorPC] {
							if load.Mode != "dp,x" || !load.IndexRestored {
								continue
							}
							query, ok := q.fieldQuery(gi, load.PC, ctx)
							if ok {
								q.fieldLoads[coldFieldLoad{s.SelectorPC, step.FetchPC, load, query}] = true
							}
						}
					}
				}
			}
		}
	}
	q.e.solve()
	if q.e.failed {
		return nil
	}
	// Stable order controls the representative evidence when several roots
	// describe the same stream position. Later waves revisit these consumers
	// when a newly rooted reload finally matches an earlier publication.
	var stores []coldFieldStore
	var loads []coldFieldLoad
	for s := range q.fieldStores {
		stores = append(stores, s)
	}
	for l := range q.fieldLoads {
		loads = append(loads, l)
	}
	sort.Slice(stores, func(i, j int) bool {
		a, b := stores[i], stores[j]
		return cmp.Or(cmp.Compare(a.selector, b.selector), cmp.Compare(a.fetch, b.fetch), cmp.Compare(a.publication.PC, b.publication.PC), cmp.Compare(a.publication.CursorDelta, b.publication.CursorDelta), cmp.Compare(a.query.site.graph, b.query.site.graph), cmp.Compare(a.query.context, b.query.context), cmp.Compare(a.stop, b.stop)) < 0
	})
	sort.Slice(loads, func(i, j int) bool {
		a, b := loads[i], loads[j]
		return cmp.Or(cmp.Compare(a.selector, b.selector), cmp.Compare(a.fetch, b.fetch), cmp.Compare(a.reload.PC, b.reload.PC), cmp.Compare(a.query.site.graph, b.query.site.graph), cmp.Compare(a.query.context, b.query.context)) < 0
	})
	var roots []ShadowCommandRoot
	for _, store := range stores {
		sf := q.fieldQueries[store.query]
		so, ok := q.e.fieldOffset(sf)
		if !ok {
			continue
		}
		for _, load := range loads {
			if store.selector != load.selector || store.fetch>>16 != load.fetch>>16 {
				continue
			}
			lf := q.fieldQueries[load.query]
			lo, ok := q.e.fieldOffset(lf)
			if !ok || sf.slot != lf.slot || uint16(uint32(so)+uint32(store.publication.Operand)) != uint16(uint32(lo)+uint32(load.reload.Operand)) {
				continue
			}
			for _, s := range q.streams {
				if s.SelectorPC != store.selector {
					continue
				}
				cursor := int(store.entryY) + store.publication.CursorDelta
				fetch := cursor + load.reload.FetchDelta + s.TagByteOffset - 1
				if cursor < 0 || cursor > 65535 || fetch < 0 || fetch > 65534 {
					continue
				}
				pc := store.fetch&0xff0000 | uint32(fetch)
				if _, ok := shadowStreamROMWord(q.e.image, byte(pc>>16), uint32(fetch)); !ok {
					continue
				}
				pointer := uint16(cursor)
				streamPC := pc&0xff0000 | uint32(pointer)
				field := &ShadowCommandFieldInput{q.evidence(store.query.context), q.evidence(load.query.context), sf.slot, so + store.publication.Operand}
				obligations := []string{"conditional_native_publication_not_a_return", "independently_rooted_store_and_reload_ROM_operands", "binary_arithmetic_and_matching_D_object_pointer_slot", "pointer_slot_object_identity_and_field_lifetime_unproven", "publication_must_reach_reload_without_overwrite", "reload_stream_bank_must_match_parent_stream", "no_frame_skip_or_closed_entry_promotion"}
				evidence := &ShadowCommandPublishedResume{ParentFetchPC: store.fetch, Publication: store.publication, Reload: load.reload, EntryDecimalCondition: "clear", NativeStop: store.stop, Obligations: obligations, FieldInput: field}
				roots = append(roots, ShadowCommandRoot{SelectorPC: s.SelectorPC, FetchPC: s.FetchPC, FetchOperand: uint16(s.TagByteOffset - 1), FetchCursorDelta: load.reload.FetchDelta, References: []ShadowCommandRootReference{{Pointer: &pointer, StreamPC: &streamPC, FirstFetchPC: &pc, Status: "conditional_rebased_cursor_reload", PublishedResume: evidence}}, ProofObligations: slices.Clone(obligations)})
				break
			}
			break // one deterministic witness for this publication is sufficient
		}
	}
	return roots
}
