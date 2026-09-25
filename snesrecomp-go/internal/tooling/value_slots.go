package tooling

import (
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// Scalar slots are reused: one address can hold unrelated values in separate
// lifetimes. A load's local reaching definitions separate those lifetimes
// before the global publication union is consulted. That union holds the
// values of spelled (unindexed direct, absolute and long) stores only, so both
// answers rest on the stores the analysis can see. A local answer survives a
// call only when no spelled writer of the slot is reachable from the callee
// through decoded direct transfers or resolved dispatch entries, and the
// callee itself is decoded; otherwise the load uses the global union. Code the
// analysis cannot see into (an unresolved indirect transfer, or a transfer
// into undecoded, HLE or overridden code) and aliased (indexed, pointer or
// block-move) writes that could touch the slot do not end a local answer: the
// union would share the same blind spot, and ending it loses the precision
// that separates reused slots. Relying on them is recorded as the explicit
// conditions slot_definition_assumes_no_hidden_write and
// slot_definition_assumes_no_aliased_write. A block move on the load's own
// path still ends the local answer. Indexed writes based above the slot are
// taken not to carry into the next bank; stack writes and interrupt handlers
// are assumed not to alias scalar slots.
const coldSlotWalkLimit = 256
const coldSlotAncestorLimit = 4096

type coldSlotDefinition struct {
	site coldValueSite
	reg  string // stored register; empty for STZ
}

type coldSlotDefinitions struct {
	defs      []coldSlotDefinition
	ok        bool
	crossCall bool
	aliased   bool // an aliased write that could touch the slot was assumed not to
	hidden    bool // a callee reaches code the analysis cannot see into
}

// What a callee graph can reach through direct and resolved transfers.
type coldForwardEffect struct {
	hidden     bool   // unknown memory effects, or the walk ran out of budget
	aliasFloor uint32 // lowest low-WRAM address a reachable aliased write may touch
}

// A direct transfer out of a graph; call distinguishes JSR/JSL from JMP/JML.
type coldTransfer struct {
	target uint32
	call   bool
}

type coldDefinitionBankKey struct {
	graph int
	def   decoder.DecodeKey
}

func coldKeyLess(a, b decoder.DecodeKey) bool {
	if a.PC != b.PC {
		return a.PC < b.PC
	}
	if a.M != b.M {
		return a.M < b.M
	}
	return a.X < b.X
}

// coldSpelledWrite reports an unindexed direct, absolute or long write to
// memory that can mirror low WRAM. Indexed, indirect and block-move writes are
// aliases: they may overlap any slot and follow the caller's alias policy.
func coldSpelledWrite(d *decoder.DecodedInstruction) (address uint32, width int, spelled, alias bool) {
	i := d.Instruction
	width = 2 - int(d.Key.M)
	switch i.Mnemonic {
	case "STA", "STZ", "TSB", "TRB":
	case "STX", "STY":
		width = 2 - int(d.Key.X)
	case "INC", "DEC", "ASL", "LSR", "ROL", "ROR":
		if i.Mode == cpu65816.ACC {
			return 0, 0, false, false
		}
	case "MVN", "MVP":
		return 0, 0, false, true
	default:
		return 0, 0, false, false
	}
	switch i.Mode {
	case cpu65816.DP, cpu65816.ABS:
		return i.Operand & 0xffff, width, true, false
	case cpu65816.LONG:
		if bank := byte(i.Operand >> 16); bank == 0x7e || bank&0x7f < 0x40 {
			return i.Operand & 0xffff, width, true, false
		}
		return 0, 0, false, false
	}
	return 0, 0, false, true
}

// coldAliasFloor bounds an aliased write (indexed, indirect or block move): the
// lowest low-WRAM address it may reach, or 0x10000 when it cannot reach low
// WRAM. Absolute and long indexed writes start at their base; direct-page,
// stack-relative and pointer-based writes may reach any address.
func coldAliasFloor(d *decoder.DecodedInstruction) uint32 {
	i := d.Instruction
	if i.Mnemonic == "MVN" || i.Mnemonic == "MVP" {
		// The destination bank is the first operand byte; Y wraps inside it.
		if bank := byte(i.Operand); bank == 0x7e || bank&0x7f < 0x40 {
			return 0
		}
		return 0x10000
	}
	switch i.Mode {
	case cpu65816.ABSX, cpu65816.ABSY:
		return i.Operand & 0xffff
	case cpu65816.LONGX:
		if bank := byte(i.Operand >> 16); bank == 0x7e || bank&0x7f < 0x40 {
			return i.Operand & 0xffff
		}
		return 0x10000
	}
	return 0
}

func coldDirectCallTarget(d *decoder.DecodedInstruction) (uint32, bool) {
	i := d.Instruction
	switch {
	case i.Opcode == 0x20 && i.Mode == cpu65816.ABS:
		return d.Key.PC&0xff0000 | i.Operand&0xffff, true
	case i.Opcode == 0x22 && i.Mode == cpu65816.LONG:
		return i.Operand & 0xffffff, true
	}
	return 0, false
}

func (e *coldValueEngine) slotDefinitions(s coldValueSite) ([]coldSlotDefinition, bool) {
	r, ok := e.slotDefs[s]
	if !ok {
		r = e.findSlotDefinitions(s)
		e.slotDefs[s] = r
	}
	if r.ok {
		e.conditions["local_slot_reaching_definition"] = true
		if r.crossCall {
			e.conditions["slot_definition_survives_calls"] = true
		}
		if r.aliased {
			e.conditions["slot_definition_assumes_no_aliased_write"] = true
		}
		if r.hidden {
			e.conditions["slot_definition_assumes_no_hidden_write"] = true
		}
	}
	return r.defs, r.ok
}

// A definition must be found on every decoded predecessor path. Reaching the
// entry, a partial or read-modify-write update, a block move, an interrupt
// boundary, an indirect call or a callee that may store to the slot means the
// load has no local answer; callers then use the global publication union.
func (e *coldValueEngine) findSlotDefinitions(s coldValueSite) coldSlotDefinitions {
	w := e.walks[s.graph]
	load := w.graph.Instructions[s.key]
	if load == nil || load.Instruction == nil {
		return coldSlotDefinitions{}
	}
	slot := load.Instruction.Operand & 0xffff
	var r coldSlotDefinitions
	seen := map[decoder.DecodeKey]bool{}
	work := []decoder.DecodeKey{s.key}
	for len(work) != 0 {
		at := work[len(work)-1]
		work = work[:len(work)-1]
		if at == w.graph.Entry || len(w.preds[at]) == 0 {
			return coldSlotDefinitions{}
		}
		for _, p := range w.preds[at] {
			if seen[p] {
				continue
			}
			if len(seen) >= coldSlotWalkLimit {
				return coldSlotDefinitions{}
			}
			seen[p] = true
			d := w.graph.Instructions[p]
			if d == nil || d.Instruction == nil {
				return coldSlotDefinitions{}
			}
			i := d.Instruction
			address, width, spelled, alias := coldSpelledWrite(d)
			if alias {
				if i.Mnemonic == "MVN" || i.Mnemonic == "MVP" {
					return coldSlotDefinitions{}
				}
				r.aliased = r.aliased || coldAliasFloor(d) <= slot+1
			}
			if spelled && address <= slot+1 && slot < address+uint32(width) {
				reg, _ := coldValueStore(d)
				if width != 2 || address != slot || reg == "" && i.Mnemonic != "STZ" {
					return coldSlotDefinitions{}
				}
				r.defs = append(r.defs, coldSlotDefinition{site: coldValueSite{s.graph, p}, reg: reg})
				continue
			}
			switch i.Mnemonic {
			case "JSR", "JSL":
				target, direct := coldDirectCallTarget(d)
				if !direct {
					return coldSlotDefinitions{}
				}
				writes, effect := e.slotCallEffect(target, slot)
				if writes {
					return coldSlotDefinitions{}
				}
				r.crossCall = true
				r.aliased = r.aliased || effect.aliasFloor <= slot+1
				r.hidden = r.hidden || effect.hidden
			case "BRK", "COP", "WAI", "STP", "RTI":
				return coldSlotDefinitions{}
			}
			work = append(work, p)
		}
	}
	sort.Slice(r.defs, func(a, b int) bool { return coldKeyLess(r.defs[a].site.key, r.defs[b].site.key) })
	r.defs = slices.CompactFunc(r.defs, func(a, b coldSlotDefinition) bool { return a.site == b.site })
	r.ok = len(r.defs) != 0
	return r
}

// slotCallEffect classifies a direct call for a slot reloaded after it.
// writes: the callee is undecoded or reaches a spelled writer of the slot, or
// a budget ran out. Otherwise effect reports what else the callee reaches.
func (e *coldValueEngine) slotCallEffect(target, slot uint32) (writes bool, effect coldForwardEffect) {
	effect.aliasFloor = 0x10000
	off, ok := e.a.offset(target)
	if !ok {
		return true, effect
	}
	callees := e.entryGraphs[off]
	if len(callees) == 0 {
		return true, effect
	}
	ancestors, bounded := e.slotWriterAncestors(slot)
	if !bounded {
		return true, effect
	}
	for _, g := range callees {
		if ancestors[g] {
			return true, effect
		}
		reached := e.forwardEffect(g)
		effect.hidden = effect.hidden || reached.hidden
		effect.aliasFloor = min(effect.aliasFloor, reached.aliasFloor)
	}
	return false, effect
}

// Graphs containing a spelled write to either slot byte, plus every graph that
// reaches one of their instructions through a decoded direct transfer or a
// resolved dispatch entry.
func (e *coldValueEngine) slotWriterAncestors(slot uint32) (map[int]bool, bool) {
	if ancestors, ok := e.slotAncestors[slot]; ok {
		return ancestors, ancestors != nil
	}
	result := map[int]bool{}
	var queue []int
	add := func(g int) {
		if !result[g] {
			result[g] = true
			queue = append(queue, g)
		}
	}
	for _, g := range e.slotWrites[slot] {
		add(g)
	}
	for _, g := range e.slotWrites[slot+1] {
		add(g)
	}
	for len(queue) != 0 {
		if len(result) > coldSlotAncestorLimit {
			e.slotAncestors[slot] = nil
			e.boundaries["slot_writer_ancestor_budget"] = true
			return nil, false
		}
		g := queue[0]
		queue = queue[1:]
		for _, key := range e.graphs[g].Order {
			if off, ok := e.a.offset(key.PC); ok {
				for _, caller := range e.callers[off] {
					add(caller.graph)
				}
				for _, dispatcher := range e.dispatchers[off] {
					add(dispatcher.graph)
				}
			}
		}
	}
	e.slotAncestors[slot] = result
	return result, true
}

// forwardEffect summarizes what a graph can reach through decoded direct and
// resolved transfers. It is independent of the slot, so one walk serves every
// slot a call is checked against.
func (e *coldValueEngine) forwardEffect(g int) coldForwardEffect {
	if r, ok := e.forwardEffects[g]; ok {
		return r
	}
	r := coldForwardEffect{aliasFloor: 0x10000}
	seen := map[int]bool{g: true}
	queue := []int{g}
	for len(queue) != 0 {
		if len(seen) > coldSlotAncestorLimit {
			r.hidden = true
			e.boundaries["callee_effect_budget"] = true
			break
		}
		cur := queue[0]
		queue = queue[1:]
		r.hidden = r.hidden || e.opaque[cur]
		r.aliasFloor = min(r.aliasFloor, e.aliasFloor[cur])
		for _, next := range e.successors[cur] {
			if !seen[next] {
				seen[next] = true
				queue = append(queue, next)
			}
		}
	}
	e.forwardEffects[g] = r
	return r
}

// indexDefinition names the value an indexed access uses: the single local
// reaching definition of an untransformed scalar-slot reload, otherwise the
// untransformed load itself. Transformed and entry indices have no identity.
func (e *coldValueEngine) indexDefinition(s coldValueSite, reg string) (decoder.DecodeKey, bool) {
	w := e.walks[s.graph]
	x := w.indexExpression(s.key, reg, true)
	if x.Source.Kind != "load" || len(x.Operations) != 0 {
		return decoder.DecodeKey{}, false
	}
	load := w.graph.Instructions[x.sourceKey].Instruction
	if (load.Mode == cpu65816.DP || load.Mode == cpu65816.ABS) && load.Operand < 0x1fff {
		if defs, ok := e.slotDefinitions(coldValueSite{s.graph, x.sourceKey}); ok && len(defs) == 1 {
			return defs[0].site.key, true
		}
	}
	return x.sourceKey, true
}

// Reads through one index definition address one record. When other reads of
// that definition have an independently known DB, this read may conditionally
// use that ROM bank: record identity, not DB preservation across calls.
// Conflicting known banks provide nothing.
func (e *coldValueEngine) definitionBanks(s coldValueSite, reg string) []byte {
	def, ok := e.indexDefinition(s, reg)
	if !ok {
		return nil
	}
	cacheKey := coldDefinitionBankKey{s.graph, def}
	if banks, ok := e.defBanks[cacheKey]; ok {
		return banks
	}
	e.defBanks[cacheKey] = nil
	g := e.graphs[s.graph]
	var banks []byte
	for _, key := range g.Order {
		d := g.Instructions[key]
		if key == s.key || d == nil || d.Instruction == nil {
			continue
		}
		index := ""
		switch d.Instruction.Mode {
		case cpu65816.ABSX:
			index = "X"
		case cpu65816.ABSY:
			index = "Y"
		default:
			continue
		}
		switch d.Instruction.Mnemonic {
		case "LDA", "LDX", "LDY", "ADC", "SBC", "AND", "ORA", "EOR", "CMP", "CPX", "CPY", "BIT":
		default:
			continue
		}
		other := coldValueSite{s.graph, key}
		if od, ok := e.indexDefinition(other, index); !ok || od != def {
			continue
		}
		if b, ok := e.knownBank(other); ok {
			banks = append(banks, b)
		}
	}
	slices.Sort(banks)
	banks = slices.CompactFunc(banks, e.sameROMBank)
	if len(banks) != 1 {
		banks = nil
	}
	e.defBanks[cacheKey] = banks
	return banks
}

// Mapper mirrors of one ROM bank are the same record storage.
func (e *coldValueEngine) sameROMBank(a, b byte) bool {
	if a == b {
		return true
	}
	x, okA := e.a.offset(uint32(a)<<16 | 0xffff)
	y, okB := e.a.offset(uint32(b)<<16 | 0xffff)
	return okA && okB && x == y
}

// A publication store must address a low-WRAM mirror. Without a known DB, the
// store may borrow the conditional bank of the record read that immediately
// precedes it on the same path: nothing in between can change DB.
func (e *coldValueEngine) slotPublication(s coldValueSite) bool {
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
	if !ok {
		b, ok = e.localReadBank(s)
	}
	return ok && b&0x7f < 0x40
}

// localReadBank finds the nearest preceding indexed read on a unique local
// path with no DB-affecting instruction in between and returns its
// same-definition record bank. Any join, call, pull or block move stops it.
func (e *coldValueEngine) localReadBank(s coldValueSite) (byte, bool) {
	w := e.walks[s.graph]
	at := s.key
	for range shadowPointerWalkLimit {
		prev := w.previous(at)
		if prev == nil || prev.Instruction == nil {
			return 0, false
		}
		at = prev.Key
		i := prev.Instruction
		switch i.Mnemonic {
		case "PLB", "PLP", "MVN", "MVP", "JSR", "JSL", "JMP", "JML", "RTI", "RTS", "RTL", "BRK", "COP", "WAI", "STP":
			return 0, false
		}
		index := ""
		switch i.Mode {
		case cpu65816.ABSX:
			index = "X"
		case cpu65816.ABSY:
			index = "Y"
		default:
			continue
		}
		switch i.Mnemonic {
		case "LDA", "LDX", "LDY", "ADC", "SBC", "AND", "ORA", "EOR", "CMP", "CPX", "CPY", "BIT":
		default:
			return 0, false
		}
		if banks := e.definitionBanks(coldValueSite{s.graph, at}, index); len(banks) == 1 {
			e.conditions["same_index_definition_bank"] = true
			return banks[0], true
		}
		return 0, false
	}
	return 0, false
}
