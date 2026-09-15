package tooling

import "github.com/DerrickGold/snesrecomp-go/internal/decoder"

// Demand-driven reuse of bank/abstract-stack summaries. Re-decode a requested
// native region without compiler body boundaries; these are analysis-only
// graphs, not additional emitted roots. Every budget or authored barrier stays
// unknown. In particular, recursion never proves itself preserving. Results
// are conditional on native writes not aliasing local saved stack bytes; this
// cold-only hypothesis MUST NOT become a published DB preservation fact.
type coldBankQueries struct {
	a         *shadowCallbackAnalyzer
	entry     decoder.Variant
	active    bool
	programs  map[decoder.Variant]*shadowDBProgram
	summaries map[decoder.Variant]shadowDBSummary
	queries   map[[2]decoder.Variant]ShadowRegisterEvidence
	entryDB   map[[2]decoder.Variant]bool
}

func (q *coldBankQueries) load(v decoder.Variant) {
	if q.programs == nil {
		q.programs = map[decoder.Variant]*shadowDBProgram{}
	}
	if _, ok := q.programs[v]; ok || len(q.programs) >= shadowDBProgramLimit {
		return
	}
	q.programs[v] = nil
	off, ok := q.a.offset(v.Address)
	if !ok || q.a.blocked[off] != "" {
		return
	}
	g, err := decoder.DecodeFunction(q.a.image, byte(v.Address>>16), uint16(v.Address), v.M, v.X, decoder.Options{MaxInstructions: shadowDBWorkLimit})
	if err != nil {
		return
	}
	p := collectShadowDBProgram(q.a.image, g)
	q.programs[v] = p
	q.summaries = nil
	for i := range p.nodes {
		n := &p.nodes[i]
		blocked := false
		for b := 0; b < int(g.Instructions[n.key].Instruction.Length); b++ {
			if off, ok := q.a.offset(n.key.PC + uint32(b)); !ok || q.a.blocked[off] != "" {
				blocked = true
			}
		}
		if blocked {
			n.blocked = "authored_cold_bank_barrier"
			n.call = nil
			continue
		}
		if n.call != nil {
			for _, target := range n.call.targets {
				q.load(target)
			}
		}
	}
}

func (q *coldBankQueries) bank(g *decoder.Graph, at decoder.DecodeKey) ShadowRegisterEvidence {
	entry := decoder.Variant{Address: g.Entry.PC, M: g.Entry.M, X: g.Entry.X}
	target := decoder.Variant{Address: at.PC, M: at.M, X: at.X}
	key := [2]decoder.Variant{entry, target}
	if r, ok := q.queries[key]; ok {
		return r
	}
	// Budgets belong to each requested entry, not the iteration order of
	// unrelated cold candidates. Cached answers remain independent of jobs.
	if !q.active || q.entry != entry {
		q.entry, q.active = entry, true
		q.programs, q.summaries = nil, nil
	}
	q.load(entry)
	if q.summaries == nil {
		q.summaries = buildShadowDBSummariesWithStackPolicy(q.programs, nil, true)
	}
	query := evaluateShadowDBWithStackPolicy(q.programs[entry], q.summaries, nil, &target, true)
	if q.entryDB == nil {
		q.entryDB = map[[2]decoder.Variant]bool{}
	}
	q.entryDB[key] = query.summary.valid && len(query.values) == 1 && query.values[0] == shadowDBEntry
	r := ShadowRegisterEvidence{UnknownPaths: true}
	if query.summary.valid && len(query.values) == 1 && query.values[0] >= 0 && query.values[0] <= 255 {
		r = ShadowRegisterEvidence{Constants: []ShadowRegisterConstant{{Value: uint16(query.values[0]), DefinitionPC: at.PC}}}
	}
	if q.queries == nil {
		q.queries = map[[2]decoder.Variant]ShadowRegisterEvidence{}
	}
	q.queries[key] = r
	return r
}

// A symbolic entry DB is reusable only when every modeled path to this read
// preserves it. Unknown pulls/callees are not a license to borrow the seed DB.
func (q *coldBankQueries) preservesEntryDB(g *decoder.Graph, at decoder.DecodeKey) bool {
	q.bank(g, at)
	return q.entryDB[[2]decoder.Variant{{Address: g.Entry.PC, M: g.Entry.M, X: g.Entry.X}, {Address: at.PC, M: at.M, X: at.X}}]
}
