package tooling

import (
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// A rooted command supplies Y/DB to a native value query. No stack bytes,
// flags, branch outcomes or execution counts are supplied or inferred.
type ShadowCommandValueInput struct {
	ParentFetchPC uint32   `json:"parent_fetch_pc"`
	NativeEntryPC uint32   `json:"native_entry_pc"`
	EntryY        uint16   `json:"entry_y"`
	EntryDB       byte     `json:"entry_db"`
	CallSites     []uint32 `json:"direct_transfer_sites,omitempty"`
	Obligations   []string `json:"proof_obligations"`
}

type ShadowCommandValueSummary struct {
	EntryInputs int      `json:"entry_inputs"`
	Queries     int      `json:"queries"`
	Evaluations int      `json:"evaluations"`
	Converged   bool     `json:"converged"`
	Boundaries  []string `json:"boundaries,omitempty"`
	Conditions  []string `json:"conditions,omitempty"`
}

type coldCommandValueQueries struct {
	e            *coldValueEngine
	streams      []ShadowCommandStream
	roots        []ShadowCommandRoot
	entries      map[int][]int // physical native entry -> graph IDs
	contexts     map[decoder.Variant]int
	fieldQueries map[coldValueQuery]coldFieldIndex
	fieldStores  map[coldFieldStore]bool
	fieldLoads   map[coldFieldLoad]bool
}

func newColdCommandValueQueries(image rom.Image, configs map[byte]*config.Config, graphs, eligible []*decoder.Graph, inventory *DecodedCommandInventory) *coldCommandValueQueries {
	e := newColdValueEngine(image, configs, graphs, eligible)
	q := &coldCommandValueQueries{e: e, streams: inventory.Streams, roots: inventory.Roots, entries: map[int][]int{}, contexts: map[decoder.Variant]int{}}
	for gi, g := range e.graphs {
		if g.Entry.M != 0 || g.Entry.X != 0 {
			continue
		}
		if off, ok := e.a.offset(g.Entry.PC); ok {
			q.entries[off] = append(q.entries[off], gi)
		}
		q.contexts[decoder.Variant{Address: g.Entry.PC, M: 0, X: 0}] = gi
	}
	return q
}

func (q *coldCommandValueQueries) seed(wave []ShadowCommandWalk) bool {
	changed := false
	// Stable insertion also determines which evidence represents equivalent
	// entry inputs and which bounded contexts are retained first.
	type input struct {
		handler, fetch uint32
		y              uint16
		bank           byte
	}
	var inputs []input
	for _, w := range wave {
		for _, s := range q.streams {
			if s.SelectorPC != w.SelectorPC {
				continue
			}
			for _, step := range w.Steps {
				if step.Kind == "untagged_data" || step.Index < 0 {
					continue
				}
				for _, c := range s.Commands {
					if c.Index != step.Index || c.EntryPC != step.Handler || c.Callback != nil {
						continue
					}
					y := int(uint16(step.FetchPC)) - s.TagByteOffset + 1 + s.EntryCursorDelta
					if y >= 0 && y <= 65535 {
						inputs = append(inputs, input{c.EntryPC, step.FetchPC, uint16(y), byte(step.FetchPC >> 16)})
					}
				}
			}
		}
	}
	sort.Slice(inputs, func(i, j int) bool {
		a, b := inputs[i], inputs[j]
		if a.handler != b.handler {
			return a.handler < b.handler
		}
		if a.fetch != b.fetch {
			return a.fetch < b.fetch
		}
		if a.y != b.y {
			return a.y < b.y
		}
		return a.bank < b.bank
	})
	for _, in := range slices.Compact(inputs) {
		off, ok := q.e.a.offset(in.handler)
		if !ok {
			continue
		}
		for _, gi := range q.entries[off] {
			changed = q.e.addEntrySeed(coldValueEntrySeed{gi, in.y, in.bank, in.fetch}) || changed
		}
	}
	return changed
}

func (q *coldCommandValueQueries) evidence(context int) *ShadowCommandValueInput {
	var calls []uint32
	for depth := 0; context != 0 && depth <= coldValueContextDepth; depth++ {
		ctx := q.e.contexts[context]
		if ctx.seed != 0 {
			s := q.e.seeds[ctx.seed-1]
			slices.Reverse(calls)
			return &ShadowCommandValueInput{s.parentFetch, q.e.graphs[s.graph].Entry.PC, s.y, s.bank, calls, []string{
				"independently_rooted_ROM_command_inputs_not_runtime_observations",
				"native_entry_M0X0_and_direct_transfer_paths_are_conditional",
				"value_preservation_does_not_prove_stack_return_or_branch_feasibility",
				"no_closed_target_or_global_D_DB_MX_fact_promotion"}}
		}
		calls = append(calls, ctx.caller.key.PC)
		context = ctx.parent
	}
	return nil
}

// Existing stream-root shapes are consumers of the shared word queries. A
// newly learned script input can therefore reach an ordinary table initializer
// through direct tails, without interpreting opaque stack bytes or inventing a
// complete native command effect. Returned words remain DATA stream pointers.
func (q *coldCommandValueQueries) discover(wave []ShadowCommandWalk) []ShadowCommandRoot {
	if q.e.failed || !q.seed(wave) {
		return nil
	}
	type query struct {
		root  ShadowCommandRoot
		node  int
		input *ShadowCommandValueInput
		bank  byte
	}
	var queries []query
	for _, r := range q.roots {
		gi, ok := q.contexts[decoder.Variant{Address: r.Context.PC, M: r.Context.EntryMX.M, X: r.Context.EntryMX.X}]
		if !ok || r.TableRead.LoadPC == 0 {
			continue
		}
		bank, ok := shadowStreamConstantBank(r.StreamBank)
		if !ok {
			continue
		}
		g := q.e.graphs[gi]
		key := decoder.DecodeKey{PC: r.TableRead.LoadPC, M: 0, X: 0}
		if g.Instructions[key] == nil {
			continue
		}
		for _, ctx := range q.e.invocationContexts(gi, coldValueContextDepth, map[int]bool{}) {
			input := q.evidence(ctx)
			if input == nil {
				continue
			}
			node := q.e.node(coldValueQuery{coldValueSite{gi, key}, "read", ctx})
			if node >= 0 {
				queries = append(queries, query{r, node, input, bank})
			}
		}
	}
	q.e.solve()
	if q.e.failed {
		return nil
	}
	var out []ShadowCommandRoot
	for _, query := range queries {
		for _, v := range q.e.nodes[query.node].values {
			r := query.root
			if !v.origin.valid || v.origin.base != uint16(r.TableRead.Operand) {
				continue
			}
			cursor := int(v.word) + r.FetchCursorDelta
			fetch := cursor + int(r.FetchOperand)
			if v.word == 0 || v.word == 0xffff || cursor < 0 || cursor > 65535 || fetch < 0 || fetch > 65534 {
				continue
			}
			if _, ok := shadowStreamROMWord(q.e.image, query.bank, uint32(fetch)); !ok {
				continue
			}
			pc := uint32(query.bank)<<16 | uint32(fetch)
			streamPC := uint32(query.bank)<<16 | uint32(v.word)
			pointer := v.word
			r.References = []ShadowCommandRootReference{{ValueInput: query.input, TableIndex: v.origin.cell - v.origin.base, TableReadPC: uint32(v.origin.bank)<<16 | uint32(v.origin.cell), Pointer: &pointer, StreamPC: &streamPC, FirstFetchPC: &pc, Status: "conditional_command_value_stream_reference"}}
			r.ProofObligations = append(slices.Clone(r.ProofObligations), query.input.Obligations...)
			out = append(out, r)
		}
	}
	return out
}

func (q *coldCommandValueQueries) summary() *ShadowCommandValueSummary {
	r := &ShadowCommandValueSummary{EntryInputs: len(q.e.seeds), Queries: len(q.e.nodes), Evaluations: q.e.work, Converged: !q.e.failed}
	for s := range q.e.boundaries {
		r.Boundaries = append(r.Boundaries, s)
	}
	for s := range q.e.conditions {
		r.Conditions = append(r.Conditions, s)
	}
	slices.Sort(r.Boundaries)
	slices.Sort(r.Conditions)
	return r
}
