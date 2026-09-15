package regen

import (
	"fmt"
	"reflect"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/emitter"
)

// exposeInternalTails runs AFTER the ordinary discovery/pruning fixed point.
// It exposes only existing local unconditional jump destinations, not bytes
// guessed to be code, and does not claim any dynamic edge is reachable/closed.
// New wrappers are output-only entries: they must never split the old CFG or
// become ordinary-call discovery roots. Existing direct edges remain gotos.
func (repo *repository) exposeInternalTails(results map[byte][]*emitter.FunctionResult) (int, error) {
	type owner struct {
		bank   *bankState
		index  int
		entry  config.Entry
		result *emitter.FunctionResult
	}
	type candidate struct {
		owner    *owner
		key      decoder.DecodeKey
		conflict bool
	}
	candidates := make(map[decoder.Variant]*candidate)
	var owners []*owner
	for _, bank := range repo.banks {
		for index, r := range results[bank.ID] {
			e := bank.Config.Entries[index]
			if r == nil || r.Graph == nil || r.CFG == nil || r.GarbageBRK != nil || r.DecodeBudgetStub ||
				e.End != nil || e.ExitMX != nil || e.TailCallPC != nil || e.EntrySOffset != 0 ||
				len(entryHLEObligations(bank.Config, e.Start)) != 0 ||
				len(r.Graph.ConstantZFolds) != 0 || len(r.Graph.SuppressedIndirectCalls) != 0 ||
				len(r.Graph.DispatchTargetsSuppressed) != 0 ||
				strings.Contains(r.Source, "resumable-region") || strings.Contains(r.Source, "sr_continuation_") {
				continue
			}
			o := &owner{bank, index, e, r}
			owners = append(owners, o)
			for _, key := range r.Graph.Order {
				d := r.Graph.Instructions[key]
				if d == nil || d.Instruction == nil || len(d.Successors) != 1 {
					continue
				}
				switch d.Instruction.Opcode {
				case 0x4c, 0x80, 0x82: // JMP abs / BRA / BRL: native same-frame edges.
				default:
					continue
				}
				target := d.Successors[0]
				v := decoder.Variant{Address: target.PC, M: target.M, X: target.X}
				if target.PDepth != 0 || target.PStack != 0 || byte(target.PC>>16) != bank.ID ||
					r.CFG.Blocks[target] == nil || repo.names[target.PC] != "" ||
					len(entryHLEObligations(bank.Config, uint16(target.PC))) != 0 || repo.inDataRegion(bank.ID, uint16(target.PC)) {
					continue
				}
				c := candidates[v]
				if c == nil {
					candidates[v] = &candidate{owner: o, key: target}
					continue
				}
				if !sameInternalTailClosure(c.owner.result.Graph, r.Graph, target) {
					c.conflict = true
				}
				if variantLess(entryVariant(bank.ID, e), entryVariant(c.owner.bank.ID, c.owner.entry)) {
					c.owner = o
				}
			}
		}
	}
	keys := make([]decoder.Variant, 0, len(candidates))
	for v := range candidates {
		keys = append(keys, v)
	}
	sort.Slice(keys, func(i, j int) bool { return variantLess(keys[i], keys[j]) })
	plans := make(map[*owner]*emitter.RegionBodyOptions)
	for _, v := range keys {
		c := candidates[v]
		if c.conflict {
			continue
		}
		o := c.owner
		// A fresh entry must yield exactly the same native instruction/edge
		// closure. Reject inherited PHP history, entry-dependent pruning, HLE
		// and return-table specialization rather than borrowing those facts.
		options := repo.decodeOptions(o.bank, o.entry)
		g, err := decoder.DecodeFunction(repo.image, o.bank.ID, uint16(v.Address), v.M, v.X, options)
		if err != nil || !sameInternalTailClosure(o.result.Graph, g, c.key) ||
			len(g.ConstantZFolds) != 0 || len(g.SuppressedIndirectCalls) != 0 || len(g.DispatchTargetsSuppressed) != 0 {
			continue
		}
		plan := plans[o]
		if plan == nil {
			plan = &emitter.RegionBodyOptions{OwnerPC: o.entry.Start,
				HelperName: fmt.Sprintf("sr_region_cold_%02X_%04X_M%dX%d", o.bank.ID, o.entry.Start, o.entry.EntryMX.M, o.entry.EntryMX.X)}
			plans[o] = plan
		}
		if len(plan.Entries) == 65535 {
			continue
		}
		plan.Entries = append(plan.Entries, emitter.RegionEntry{PC: uint16(v.Address), M: v.M, X: v.X, Selector: uint16(len(plan.Entries) + 1)})
	}
	// Re-emit every selected owner before adding any output-only entries.
	// Sibling boundaries and direct-call routing are therefore unchanged.
	sort.Slice(owners, func(i, j int) bool {
		return variantLess(entryVariant(owners[i].bank.ID, owners[i].entry), entryVariant(owners[j].bank.ID, owners[j].entry))
	})
	for _, o := range owners {
		plan := plans[o]
		if plan == nil {
			continue
		}
		options := repo.functionOptions(o.bank, o.entry)
		options.RegionBody = plan
		r, err := emitter.EmitFunction(repo.image, o.bank.ID, o.entry.Start, o.entry.EntryMX.M, o.entry.EntryMX.X, options)
		if err != nil {
			return 0, fmt.Errorf("expose internal tails for $%02X:%04X: %w", o.bank.ID, o.entry.Start, err)
		}
		if !reflect.DeepEqual(r.Graph, o.result.Graph) {
			return 0, fmt.Errorf("internal tail exposure changed owner graph $%02X:%04X", o.bank.ID, o.entry.Start)
		}
		results[o.bank.ID][o.index] = r
	}
	count := 0
	for _, o := range owners {
		plan := plans[o]
		if plan == nil {
			continue
		}
		for _, e := range plan.Entries {
			// Empty Name avoids an ordinary void routine alias. The variant
			// symbol exists solely for the sparse exact-M/X runtime registry.
			o.bank.Config.Entries = append(o.bank.Config.Entries, config.Entry{Start: e.PC, EntryMX: config.MX{M: e.M, X: e.X}})
			results[o.bank.ID] = append(results[o.bank.ID], &emitter.FunctionResult{
				Source: emitter.EmitColdRegionEntry(o.bank.ID, e, plan.HelperName, o.entry.Start),
			})
			count++
		}
	}
	return count, nil
}

func sameInternalTailClosure(a, b *decoder.Graph, start decoder.DecodeKey) bool {
	left, right := graphInstructionClosure(a, start), graphInstructionClosure(b, start)
	if len(left) == 0 || len(left) != len(right) {
		return false
	}
	for key := range left {
		if _, found := right[key]; !found || !reflect.DeepEqual(a.Instructions[key], b.Instructions[key]) {
			return false
		}
	}
	return true
}
