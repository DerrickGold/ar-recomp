package tooling

// Inputs supplied by other static consumers are per-invocation evidence.
// They are not globals, observed roots, or a guessed native stack frame.
type coldValueEntrySeed struct {
	graph       int
	y           uint16
	bank        byte
	parentFetch uint32
}

func (e *coldValueEngine) addEntrySeed(s coldValueEntrySeed) bool {
	if e.entrySeeds == nil {
		e.entrySeeds = map[int][]int{}
	}
	for _, ctx := range e.entrySeeds[s.graph] {
		old := e.seeds[e.contexts[ctx].seed-1]
		if old.y == s.y && old.bank == s.bank {
			return false
		}
	}
	if len(e.entrySeeds[s.graph]) >= coldValueContextLimit {
		e.boundaries["entry_seed_context_budget"] = true
		return false
	}
	e.seeds = append(e.seeds, s)
	ctx := coldValueContext{seed: len(e.seeds)}
	id := len(e.contexts)
	e.contexts = append(e.contexts, ctx)
	e.contextIDs[ctx] = id
	e.entrySeeds[s.graph] = append(e.entrySeeds[s.graph], id)
	return true
}

func (e *coldValueEngine) contextBank(s coldValueSite, context, depth int) (byte, bool) {
	if bank, ok := e.knownBank(s); ok {
		return bank, true
	}
	if context == 0 || depth >= coldValueContextDepth || !e.banks.preservesEntryDB(e.graphs[s.graph], s.key) {
		return 0, false
	}
	ctx := e.contexts[context]
	if ctx.seed != 0 {
		seed := e.seeds[ctx.seed-1]
		if seed.graph != s.graph {
			return 0, false
		}
		e.conditions["rooted_native_entry_inputs"] = true
		return seed.bank, true
	}
	return e.contextBank(ctx.caller, ctx.parent, depth+1)
}
