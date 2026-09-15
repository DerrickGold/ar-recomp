package tooling

// Cold value queries are deliberately separate from dispatch proofs. A ROM
// word is reusable data evidence, not a function just because it is decodable.
// Consumers select fields/entry kinds; they never close an edge or override MX.

import (
	"fmt"
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const coldValueNodeLimit = 65536
const coldValueWorkLimit = 524288
const coldValueContextLimit = 1024
const coldValueContextDepth = 4

// ColdValueInventory is conditional inventory, never a completeness claim.
// Boundaries include unsupported origins and budgets, not runtime failures.
type ColdValueInventory struct {
	Targets            []uint32
	IndexedTargets     []uint32
	IndirectTargets    []uint32
	LongPointerTargets []uint32
	Nodes, Evaluations int
	Boundaries         []string
	Conditions         []string
	Converged          bool
}

type coldValueSite struct {
	graph int
	key   decoder.DecodeKey
}
type coldValueQuery struct {
	site    coldValueSite
	reg     string
	context int
}
type coldValueOrigin struct {
	bank   byte
	base   uint16
	cell   uint16
	valid  bool
	record bool // selected through a self-delimiting record-pointer prefix
}
type coldWordValue struct {
	word   uint16
	origin coldValueOrigin
}
type coldValueContext struct {
	caller coldValueSite
	parent int
	seed   int // one independently rooted native-entry input, not a caller frame
}
type coldValueNode struct {
	query     coldValueQuery
	values    []coldWordValue
	users     map[int]bool
	saturated bool // unknown/top after cardinality overflow, not an empty seed
}
type coldValueRead struct {
	site coldValueSite
	base uint16
}
type coldValueEngine struct {
	image                  rom.Image
	a                      *shadowCallbackAnalyzer
	graphs                 []*decoder.Graph
	walks                  []shadowPointerWalk
	banks                  coldBankQueries
	stores                 map[uint16][]coldValueSite
	callers                map[int][]coldValueSite // physical destination, not guessed bank spelling
	reads                  map[[2]uint32][]coldValueSite
	bankCache              map[coldValueSite][]byte
	contexts               []coldValueContext // zero is an unbound entry
	contextIDs             map[coldValueContext]int
	seeds                  []coldValueEntrySeed
	entrySeeds             map[int][]int
	ids                    map[coldValueQuery]int
	nodes                  []coldValueNode
	queue                  []int
	queued                 map[int]bool
	current                int
	work                   int
	failed                 bool
	boundaries, conditions map[string]bool
}

// AnalyzeColdValueProvenance shares demand-driven register, slot, table and
// caller-argument queries between indexed transfers and saved long pointers.
// A dependency worklist reruns consumers when values change, even without a
// newly decoded function. Cycles have no self-proving seed. If the bounded
// worklist fails to converge, none of its candidate targets are admitted.
func AnalyzeColdValueProvenance(image rom.Image, configs map[byte]*config.Config, graphs, eligible []*decoder.Graph) ColdValueInventory {
	e := newColdValueEngine(image, configs, graphs, eligible)
	indexed, pointers := e.consumers()
	indirect := e.indirectConsumers()
	e.solve()
	out := ColdValueInventory{Nodes: len(e.nodes), Evaluations: e.work, Converged: !e.failed}
	if !e.failed {
		for _, c := range indirect {
			for _, v := range e.nodes[c.node].values {
				pc := uint32(c.bank)<<16 | uint32(v.word)
				if v.word != 0 && v.word != 0xffff && coldMappedTarget(e.a, pc) {
					out.IndirectTargets = append(out.IndirectTargets, pc)
				}
			}
		}
		for _, c := range indexed {
			for _, v := range e.nodes[c.node].values {
				if pc := uint32(c.bank)<<16 | uint32(v.word); v.word != 0 && v.word != 0xffff && coldMappedTarget(e.a, pc) {
					out.IndexedTargets = append(out.IndexedTargets, pc)
				}
			}
		}
		for _, c := range pointers {
			words, banks := e.nodes[c.word].values, e.nodes[c.bank].values
			// Correlation is preserved by one invocation context. Do not take
			// a Cartesian product of unrelated callers' word/bank arguments.
			var bank *byte
			ambiguous := false
			for _, v := range banks {
				// The new consumer handles explicit bank-byte arguments, not
				// arbitrary word data that happens to have a ROM-looking low
				// byte. Split/table-derived bank lanes need correlated support.
				if v.word > 0xff || v.origin.valid {
					ambiguous = true
					break
				}
				b := byte(v.word)
				if bank != nil && *bank != b {
					ambiguous = true
					break
				}
				bank = &b
			}
			if ambiguous {
				e.boundaries["non_singleton_publication_bank"] = true
				continue
			}
			if bank == nil {
				continue
			}
			for _, v := range words {
				// Raw masked ROM reads are values, not a proved table extent.
				// Admit literal pointers and bounded record fields here; other
				// table forms need their own consumer ownership contract.
				if v.origin.valid && !v.origin.record || !v.origin.valid && len(words) != 1 {
					e.boundaries["unbounded_saved_pointer_word"] = true
					continue
				}
				pc := uint32(*bank)<<16 | uint32(v.word)
				if v.word != 0 && v.word != 0xffff && coldMappedTarget(e.a, pc) {
					out.LongPointerTargets = append(out.LongPointerTargets, pc)
				}
			}
		}
	}
	out.IndexedTargets = coldSortedTargets(out.IndexedTargets)
	out.IndirectTargets = coldSortedTargets(out.IndirectTargets)
	out.LongPointerTargets = coldSortedTargets(out.LongPointerTargets)
	out.Targets = coldSortedTargets(append(slices.Clone(out.IndexedTargets), out.LongPointerTargets...))
	out.Targets = coldSortedTargets(append(out.Targets, out.IndirectTargets...))
	for s := range e.boundaries {
		out.Boundaries = append(out.Boundaries, s)
	}
	for s := range e.conditions {
		out.Conditions = append(out.Conditions, s)
	}
	slices.Sort(out.Boundaries)
	slices.Sort(out.Conditions)
	return out
}

func coldSortedTargets(v []uint32) []uint32 { slices.Sort(v); return slices.Compact(v) }

func newColdValueEngine(image rom.Image, configs map[byte]*config.Config, graphs, eligible []*decoder.Graph) *coldValueEngine {
	native, a := coldNativeGraphs(image, configs, graphs, eligible)
	native = slices.Clone(native)
	sort.Slice(native, func(i, j int) bool {
		a, b := native[i].Entry, native[j].Entry
		if a.PC != b.PC {
			return a.PC < b.PC
		}
		if a.M != b.M {
			return a.M < b.M
		}
		return a.X < b.X
	})
	e := &coldValueEngine{image: image, a: a, graphs: native, banks: coldBankQueries{a: a}, stores: map[uint16][]coldValueSite{}, callers: map[int][]coldValueSite{}, reads: map[[2]uint32][]coldValueSite{}, bankCache: map[coldValueSite][]byte{}, contexts: []coldValueContext{{}}, contextIDs: map[coldValueContext]int{}, ids: map[coldValueQuery]int{}, queued: map[int]bool{}, current: -1, boundaries: map[string]bool{}, conditions: map[string]bool{}}
	for gi, g := range native {
		e.walks = append(e.walks, shadowPointerWalk{graph: g, preds: shadowPredecessors(g)})
		for _, key := range g.Order {
			d := g.Instructions[key]
			if d == nil {
				continue
			}
			i := d.Instruction
			s := coldValueSite{gi, key}
			if reg, width := coldValueStore(d); reg != "" && width == 2 && (i.Mode == cpu65816.DP || i.Mode == cpu65816.ABS) && i.Operand < 0x1fff {
				e.stores[uint16(i.Operand)] = append(e.stores[uint16(i.Operand)], s)
			}
			var target uint32
			switch i.Opcode {
			case 0x20, 0x4c:
				target = key.PC&0xff0000 | i.Operand
			case 0x22, 0x5c:
				target = i.Operand
			}
			if target != 0 {
				if off, ok := a.offset(target); ok {
					e.callers[off] = append(e.callers[off], s)
				}
			}
			if i.Mode == cpu65816.ABSX || i.Mode == cpu65816.ABSY {
				e.reads[[2]uint32{key.PC >> 16, i.Operand}] = append(e.reads[[2]uint32{key.PC >> 16, i.Operand}], s)
			}
		}
	}
	return e
}

func (e *coldValueEngine) enqueue(id int) {
	if !e.queued[id] {
		e.queued[id] = true
		e.queue = append(e.queue, id)
	}
}
func (e *coldValueEngine) node(q coldValueQuery) int {
	id, ok := e.ids[q]
	if !ok {
		if len(e.nodes) >= coldValueNodeLimit {
			e.failed = true
			e.boundaries["value_node_budget"] = true
			return -1
		}
		id = len(e.nodes)
		e.ids[q] = id
		e.nodes = append(e.nodes, coldValueNode{query: q, users: map[int]bool{}})
		e.enqueue(id)
	}
	if e.current >= 0 {
		e.nodes[id].users[e.current] = true
	}
	return id
}
func (e *coldValueEngine) values(q coldValueQuery) []coldWordValue {
	id := e.node(q)
	if id < 0 {
		return nil
	}
	return e.nodes[id].values
}
func (e *coldValueEngine) solve() {
	for len(e.queue) != 0 && !e.failed {
		if e.work >= coldValueWorkLimit {
			e.failed = true
			e.boundaries["value_worklist_budget"] = true
			break
		}
		id := e.queue[0]
		e.queue = e.queue[1:]
		delete(e.queued, id)
		if e.nodes[id].saturated {
			continue
		}
		e.current = id
		e.work++
		v := e.evaluate(e.nodes[id].query)
		e.current = -1
		v = coldWordSet(v)
		if len(v) > shadowInitializerDomainLimit {
			e.boundaries["value_cardinality_budget"] = true
			// A cycle which increments a finite set must not alternate
			// between overflow->empty and reseeding the same finite set.
			// This query has reached unknown/top for the rest of this solve.
			e.nodes[id].saturated = true
			v = nil
		}
		if !slices.Equal(v, e.nodes[id].values) {
			e.nodes[id].values = v
			var users []int
			for u := range e.nodes[id].users {
				users = append(users, u)
			}
			slices.Sort(users)
			for _, u := range users {
				e.enqueue(u)
			}
		}
	}
}
func coldWordSet(v []coldWordValue) []coldWordValue {
	sort.Slice(v, func(i, j int) bool {
		a, b := v[i], v[j]
		if a.word != b.word {
			return a.word < b.word
		}
		if a.origin.valid != b.origin.valid {
			return !a.origin.valid
		}
		if a.origin.record != b.origin.record {
			return !a.origin.record
		}
		if a.origin.bank != b.origin.bank {
			return a.origin.bank < b.origin.bank
		}
		if a.origin.base != b.origin.base {
			return a.origin.base < b.origin.base
		}
		return a.origin.cell < b.origin.cell
	})
	return slices.Compact(v)
}

func (e *coldValueEngine) evaluate(q coldValueQuery) []coldWordValue {
	w := e.walks[q.site.graph]
	// A read query is independent of the register eventually consuming it.
	if q.reg == "read" {
		return e.read(q)
	}
	if q.reg == "carry" {
		return e.carry(q)
	}
	if q.reg == "pointer" {
		return e.localPointer(q)
	}
	if q.reg == "indexed" {
		i := w.graph.Instructions[q.site.key].Instruction
		indices := e.values(coldValueQuery{q.site, "X", q.context})
		return e.readWords(byte(q.site.key.PC>>16), uint16(i.Operand), indices)
	}
	x := w.indexExpression(q.site.key, q.reg, true)
	// A backedge does not erase the separately supplied external entry input.
	// Query before the entry instruction; never borrow the previous iteration.
	if x.Source.Kind == "unknown" && x.Source.Reason == "entry_or_ambiguous_predecessor" && x.sourceKey == w.graph.Entry && shadowPointerWord(x.sourceKey, x.Source.Register) {
		x.Source.Kind = "entry_register"
	}
	// Prefer a resolved operand over the mask's finite superset. A mask is
	// useful when the origin is unknown, but must not erase a rooted input.
	if len(x.DomainValues) != 0 && x.Source.Kind == "load" && x.Source.Mode == "imm" {
		var v []coldWordValue
		for _, word := range x.DomainValues {
			v = append(v, coldWordValue{word: word})
		}
		return v
	}
	var v []coldWordValue
	switch x.Source.Kind {
	case "load":
		v = e.values(coldValueQuery{coldValueSite{q.site.graph, x.sourceKey}, "read", q.context})
	case "entry_register":
		if q.context != 0 {
			ctx := e.contexts[q.context]
			if ctx.seed != 0 {
				seed := e.seeds[ctx.seed-1]
				if seed.graph == q.site.graph && x.Source.Register == "Y" {
					v = []coldWordValue{{word: seed.y}}
				}
			} else {
				v = e.values(coldValueQuery{ctx.caller, x.Source.Register, ctx.parent})
			}
		} else {
			e.boundaries["unbound_entry_register"] = true
		}
	default:
		if x.Source.Register == "A" && x.Source.Reason == "unsupported_value_effect_ADC" {
			v = e.add(coldValueQuery{coldValueSite{q.site.graph, x.sourceKey}, "A", q.context}, false)
		} else {
			e.boundaries[x.Source.Kind+":"+x.Source.Reason] = true
		}
	}
	deferDomain := false
	if x.Source.Kind == "load" {
		i := w.graph.Instructions[x.sourceKey].Instruction
		// An indirect ROM read is a dependency, not an independent mask
		// seed. Seeding its result can make a cyclic state table prove itself.
		deferDomain = i.Mode == cpu65816.DPINDIR || i.Mode == cpu65816.INDIRY
	}
	if len(v) == 0 && len(x.DomainValues) != 0 && !deferDomain {
		for _, word := range x.DomainValues {
			v = append(v, coldWordValue{word: word})
		}
		return v // DomainValues already includes the operations.
	}
	if len(x.Operations) == 0 {
		return v
	}
	out := slices.Clone(v)
	for n := range out {
		for _, op := range x.Operations {
			switch op.Mnemonic {
			case "AND":
				out[n].word &= op.Operand
			case "ORA":
				out[n].word |= op.Operand
			case "EOR":
				out[n].word ^= op.Operand
			case "ASL":
				out[n].word <<= 1
			case "LSR":
				out[n].word >>= 1
			default:
				e.boundaries["unsupported_value_operation"] = true
				return nil
			}
		}
		out[n].origin = coldValueOrigin{} // transformed values no longer denote raw table cells
	}
	return out
}

func (e *coldValueEngine) knownBank(s coldValueSite) (byte, bool) {
	w := e.walks[s.graph]
	if b, ok := shadowStreamConstantBank(w.initializerBank(s.key)); ok {
		return b, true
	}
	b, ok := shadowStreamConstantBank(e.banks.bank(w.graph, s.key))
	if ok {
		e.conditions["saved_stack_non_aliasing"] = true
	}
	return b, ok
}
func (e *coldValueEngine) scalarSlot(s coldValueSite) bool {
	i := e.graphs[s.graph].Instructions[s.key].Instruction
	if i.Operand >= 0x1fff {
		return false
	}
	if i.Mode == cpu65816.DP {
		return true
	}
	if i.Mode != cpu65816.ABS {
		return false
	}
	b, ok := e.knownBank(s)
	return ok && b&0x7f < 0x40
}
func (e *coldValueEngine) read(q coldValueQuery) []coldWordValue {
	w := e.walks[q.site.graph]
	d := w.graph.Instructions[q.site.key]
	if d == nil {
		return nil
	}
	i := d.Instruction
	reg := ""
	switch i.Mnemonic {
	case "LDA", "ADC":
		reg = "A"
	case "LDX":
		reg = "X"
	case "LDY":
		reg = "Y"
	}
	if reg == "" || !shadowPointerWord(q.site.key, reg) {
		e.boundaries["non_word_read"] = true
		return nil
	}
	if i.Mode == cpu65816.IMM {
		return []coldWordValue{{word: uint16(i.Operand)}}
	}
	if i.Mode == cpu65816.DPINDIR || i.Mode == cpu65816.INDIRY {
		return e.indirectRead(q)
	}
	conditionalSlotRead := false
	if i.Mode == cpu65816.ABS && i.Operand < 0x1fff {
		bank, bankKnown := e.knownBank(q.site)
		conditionalSlotRead = !bankKnown || bank&0x7f < 0x40
	}
	if (i.Mode == cpu65816.DP && i.Operand < 0x1fff) || conditionalSlotRead {
		e.conditions["scalar_slot_alias_and_lifetime"] = true
		var values []coldWordValue
		for _, s := range e.stores[uint16(i.Operand)] {
			if !e.scalarSlot(s) {
				continue
			}
			r, _ := coldValueStore(e.graphs[s.graph].Instructions[s.key])
			values = append(values, e.values(coldValueQuery{s, r, 0})...)
		}
		return values
	}
	r := w.initializerRead(q.site.key, false)
	if r == nil || r.Mode == "(dp),y" {
		e.boundaries["unsupported_read_address"] = true
		return nil
	}
	indices := e.values(coldValueQuery{q.site, r.IndexRegister, q.context})
	var banks []byte
	if i.Mode == cpu65816.LONGX {
		banks = []byte{byte(i.Operand >> 16)}
	} else if b, ok := e.contextBank(q.site, q.context, 0); ok {
		banks = []byte{b}
	} else {
		// Reuse an independently banked spelling of the same table in this
		// native bank. This is a table-identity hypothesis, NOT DB preservation
		// through an unknown call. Never override a known contrary DB value.
		banks = e.tableBanks(q.site)
		if len(banks) != 0 {
			e.conditions["same_table_bank_identity"] = true
		}
		if len(banks) == 0 {
			for _, v := range indices {
				if v.origin.valid {
					banks = append(banks, v.origin.bank)
				}
			}
			slices.Sort(banks)
			banks = slices.Compact(banks)
			if len(banks) != 0 {
				e.conditions["record_in_pointer_table_bank"] = true
			}
		}
	}
	if len(banks) == 0 {
		e.boundaries["unknown_rom_read_bank"] = true
	}
	var values []coldWordValue
	for _, bank := range banks {
		values = append(values, e.readWords(bank, uint16(i.Operand), indices)...)
	}
	return values
}
func (e *coldValueEngine) tableBanks(s coldValueSite) []byte {
	if banks, ok := e.bankCache[s]; ok {
		return banks
	}
	i := e.graphs[s.graph].Instructions[s.key].Instruction
	var banks []byte
	if i.Operand >= 0x2000 {
		for _, other := range e.reads[[2]uint32{s.key.PC >> 16, i.Operand}] {
			if b, ok := e.knownBank(other); ok {
				banks = append(banks, b)
			}
		}
	}
	slices.Sort(banks)
	banks = slices.Compact(banks)
	e.bankCache[s] = banks
	return banks
}

func (e *coldValueEngine) readWords(bank byte, base uint16, indices []coldWordValue) []coldWordValue {
	// Raw ROM words reused as indices are record pointers. Keep their source
	// cells before the earliest forward record, including nullable tables.
	ends := map[[2]uint32]int{}
	for _, v := range indices {
		if !v.origin.valid {
			continue
		}
		o := v.origin
		group := [2]uint32{uint32(o.bank), uint32(o.base)}
		start, ok := e.a.offset(uint32(o.bank)<<16 | uint32(o.base))
		if !ok {
			continue
		}
		end, exists := ends[group]
		if !exists {
			end = start + 0x10000 - int(o.base)
		}
		if off, ok := e.a.offset(uint32(bank)<<16 | uint32(v.word)); ok && v.word != 0 && v.word != 0xffff && off >= start && off < end {
			end = off
		}
		ends[group] = end
	}
	var result []coldWordValue
	for _, v := range indices {
		boundedRecord := false
		if v.origin.valid {
			if v.word == 0 || v.word == 0xffff {
				continue
			}
			o := v.origin
			cell, ok := e.a.offset(uint32(o.bank)<<16 | uint32(o.cell))
			if !ok || cell+2 > ends[[2]uint32{uint32(o.bank), uint32(o.base)}] {
				continue
			}
			start, valid := e.a.offset(uint32(o.bank)<<16 | uint32(o.base))
			boundedRecord = valid && ends[[2]uint32{uint32(o.bank), uint32(o.base)}] < start+0x10000-int(o.base)
		}
		address := uint32(base) + uint32(v.word)
		if address > 0xfffe {
			e.boundaries["rom_word_bank_wrap"] = true
			continue
		}
		word, ok := shadowStreamROMWord(e.image, bank, address)
		if !ok {
			continue
		}
		result = append(result, coldWordValue{word: word, origin: coldValueOrigin{bank: bank, base: base, cell: uint16(address), valid: true, record: boundedRecord}})
	}
	return result
}

func coldValueStore(d *decoder.DecodedInstruction) (string, int) {
	switch d.Instruction.Mnemonic {
	case "STA":
		return "A", 2 - int(d.Key.M)
	case "STX":
		return "X", 2 - int(d.Key.X)
	case "STY":
		return "Y", 2 - int(d.Key.X)
	}
	return "", 0
}

func (e *coldValueEngine) invocationContexts(gi, depth int, active map[int]bool) []int {
	result := []int{0}
	result = append(result, e.entrySeeds[gi]...)
	if depth == 0 {
		e.boundaries["caller_context_depth"] = true
		return result
	}
	if active[gi] {
		e.boundaries["recursive_caller_context"] = true
		return result
	}
	active[gi] = true
	defer delete(active, gi)
	g := e.graphs[gi]
	off, ok := e.a.offset(g.Entry.PC)
	if !ok {
		return result
	}
	for _, caller := range e.callers[off] {
		if caller.key.M != g.Entry.M || caller.key.X != g.Entry.X {
			continue
		}
		for _, parent := range e.invocationContexts(caller.graph, depth-1, active) {
			ctx := coldValueContext{caller: caller, parent: parent}
			id, ok := e.contextIDs[ctx]
			if !ok {
				id = len(e.contexts)
				e.contexts = append(e.contexts, ctx)
				e.contextIDs[ctx] = id
			}
			result = append(result, id)
			if len(result) > coldValueContextLimit {
				e.boundaries["caller_context_count"] = true
				return nil
			}
		}
	}
	return result
}

type coldIndexedConsumer struct {
	node int
	bank byte
}
type coldPointerConsumer struct{ word, bank int }

func (e *coldValueEngine) consumers() ([]coldIndexedConsumer, []coldPointerConsumer) {
	var indexed []coldIndexedConsumer
	var pointers []coldPointerConsumer
	slots := map[uint16]bool{}
	for gi, g := range e.graphs {
		for _, key := range g.Order {
			d := g.Instructions[key]
			if d == nil {
				continue
			}
			i := d.Instruction
			if key.X == 0 && (i.Opcode == 0xfc || i.Opcode == 0x7c) && len(i.DispatchEntries) == 0 {
				id := e.node(coldValueQuery{coldValueSite{gi, key}, "indexed", 0})
				if id >= 0 {
					indexed = append(indexed, coldIndexedConsumer{id, byte(key.PC >> 16)})
				}
			}
			if i.Opcode == 0xdc && i.Operand < 0x1ffe {
				slots[uint16(i.Operand)] = true
			}
		}
	}
	// First consumer contract: adjacent word + low-bank publications on one
	// unique native path. Entry arguments retain one shared caller context.
	// Overlapping/byte-split stores remain with the existing literal lane pass.
	for gi, g := range e.graphs {
		for _, key := range g.Order {
			d := g.Instructions[key]
			if d == nil {
				continue
			}
			i := d.Instruction
			r, width := coldValueStore(d)
			if r == "" || width != 2 || i.Operand < 2 || !slots[uint16(i.Operand)-2] || !e.scalarSlot(coldValueSite{gi, key}) {
				continue
			}
			previous := e.walks[gi].previous(key)
			for steps := 0; steps < 16 && previous != nil; steps++ {
				p := previous.Instruction
				pr, pw := coldValueStore(previous)
				if pr != "" {
					if pw == 2 && p.Mode == i.Mode && p.Operand+2 == i.Operand {
						for _, ctx := range e.invocationContexts(gi, coldValueContextDepth, map[int]bool{}) {
							word := e.node(coldValueQuery{coldValueSite{gi, previous.Key}, pr, ctx})
							bank := e.node(coldValueQuery{coldValueSite{gi, key}, r, ctx})
							if word >= 0 && bank >= 0 {
								pointers = append(pointers, coldPointerConsumer{word, bank})
								e.conditions["saved_long_pointer_alias_and_lifetime"] = true
							}
						}
					}
					break // any intervening store might replace an unfinished lane
				}
				switch p.Mnemonic {
				case "LDA", "LDX", "LDY", "NOP":
				default:
					previous = nil
					continue
				}
				previous = e.walks[gi].previous(previous.Key)
			}
		}
	}
	return indexed, pointers
}

func (v ColdValueInventory) Summary() string {
	return fmt.Sprintf("cold values: %d nodes, %d evaluations, %d indexed / %d saved-pointer / %d indirect targets, converged=%t; boundaries=%v; conditions=%v", v.Nodes, v.Evaluations, len(v.IndexedTargets), len(v.LongPointerTargets), len(v.IndirectTargets), v.Converged, v.Boundaries, v.Conditions)
}
