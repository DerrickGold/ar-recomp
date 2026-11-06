package regen

import (
	"fmt"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// Autoroutes recognize fixed compiler idioms in ROM bytes and add the cfg
// facts an author would otherwise write by hand. Each one is skipped wherever
// the authored cfg already speaks for the site, so a hand-written directive
// always wins. Sibling fall-through needs no autoroute here: gotoOrTail
// already turns a fall into any named entry into a tail call.

// WrapperRoute renames a cross-bank alias that bypassed a DB-setting wrapper.
type WrapperRoute struct {
	Wrapper, Body uint32
	Name, Routed  string
}

// PHARTSRoute inventories candidates for a guarded native RTS. Count is the
// number of unique nonzero targets, not the number of table slots.
type PHARTSRoute struct {
	Site, Table uint32
	Count       int
	IndexReg    string
	Owner       string
}

// wrapperSignatureLength covers PHB PHK PLB JSR lo hi PLB RTL: a far-callable
// stub that makes its own bank the data bank around a near call to the body.
const wrapperSignatureLength = 8

func matchesDBWrapper(code []byte) (uint16, bool) {
	if len(code) < wrapperSignatureLength || code[0] != 0x8b || code[1] != 0x4b ||
		code[2] != 0xab || code[3] != 0x20 || code[6] != 0xab || code[7] != 0x6b {
		return 0, false
	}
	return uint16(code[4]) | uint16(code[5])<<8, true
}

// routeDBWrappers runs before cross-bank promotion. A `name <wrapper> F` alias
// whose F is a func declared at another address makes every far caller of the
// wrapper call F's body directly, so the body runs with the caller's DB. Give
// the alias a name of its own; promotion then emits the wrapper as a callable
// entry and the body keeps F for its near callers.
func (repo *repository) routeDBWrappers() []WrapperRoute {
	declared := make(map[string]uint32)
	ambiguous := make(map[string]bool)
	for _, bank := range repo.banks {
		for _, entry := range bank.Config.Entries {
			if entry.Name != "" {
				address := decoder.Address24(bank.ID, entry.Start)
				if old, seen := declared[entry.Name]; seen && old != address {
					ambiguous[entry.Name] = true
				}
				declared[entry.Name] = address
			}
		}
	}
	aliases := make(map[uint32][]*config.NameDecl)
	for _, bank := range repo.banks {
		for index := range bank.Config.Names {
			declaration := &bank.Config.Names[index]
			address := declaration.Address & 0xffffff
			aliases[address] = append(aliases[address], declaration)
		}
	}
	var routes []WrapperRoute
	for _, bank := range repo.banks {
		barriers := nativeReturnConfigBarriers(bank.ID, bank.Config)
		entries := make(map[uint16]bool)
		for _, entry := range bank.Config.Entries {
			entries[entry.Start] = true
		}
		for pc := 0; pc+wrapperSignatureLength <= 0x10000; pc++ {
			wrapperPC := uint16(pc)
			if entries[wrapperPC] {
				continue // an authored wrapper already owns its name and widths
			}
			code, err := repo.image.Slice(bank.ID, wrapperPC, wrapperSignatureLength)
			if err != nil || !repo.image.IsROM(bank.ID, wrapperPC) ||
				!repo.image.IsROM(bank.ID, wrapperPC+wrapperSignatureLength-1) {
				continue
			}
			bodyPC, found := matchesDBWrapper(code)
			if !found || bodyPC == wrapperPC || !repo.image.IsROM(bank.ID, bodyPC) || repo.autorouteTargetBlocked(bank, bodyPC) {
				continue
			}
			if repo.autorouteWindowBlocked(bank.ID, wrapperPC, wrapperSignatureLength, barriers) {
				continue
			}
			wrapper := decoder.Address24(bank.ID, wrapperPC)
			eligible := len(aliases[wrapper]) != 0
			for _, declaration := range aliases[wrapper] {
				target, known := declared[declaration.Name]
				if !known || ambiguous[declaration.Name] || target != decoder.Address24(bank.ID, bodyPC) {
					eligible = false
					break // another authored alias already names this wrapper
				}
			}
			if !eligible {
				continue
			}
			routed := fmt.Sprintf("_AutoWrap_%s__%02X_%04X", aliases[wrapper][0].Name, bank.ID, wrapperPC)
			if _, used := declared[routed]; used {
				continue
			}
			seen := make(map[string]bool)
			for _, declaration := range aliases[wrapper] {
				original := declaration.Name
				declaration.Name = routed
				if seen[original] {
					continue
				}
				seen[original] = true
				route := WrapperRoute{
					Wrapper: wrapper, Body: decoder.Address24(bank.ID, bodyPC), Name: original, Routed: routed,
				}
				routes = append(routes, route)
			}
			pc += wrapperSignatureLength - 1
		}
	}
	return routes
}

// phaRTSWindow is LDA table,X|Y / DEC A / PHA / SEP #$30 / RTS. DEC and
// the native RTS increment cancel for a word push: table words are exact
// targets. Other widths/banks retain native behavior; this is only an inventory.
const phaRTSWindow = 8

func matchesPHARTSDispatch(code []byte) (table uint16, indexReg string, ok bool) {
	if len(code) < phaRTSWindow || code[3] != 0x3a || code[4] != 0x48 ||
		code[5] != 0xe2 || code[6] != 0x30 || code[7] != 0x60 {
		return 0, "", false
	}
	switch code[0] {
	case 0xb9:
		indexReg = "Y"
	case 0xbd:
		indexReg = "X"
	default:
		return 0, "", false
	}
	return uint16(code[1]) | uint16(code[2])<<8, indexReg, true
}

// phaRTSTableLimit bounds one inferred table so a misread operand cannot run
// across the bank.
const phaRTSTableLimit = 256

// autorouteWindowBlocked protects every byte, not just the first instruction.
// The existing native-helper barriers include explicit dispatch/HLE/width and
// exclusion contracts. Data remains data even if it resembles an instruction.
func (repo *repository) autorouteWindowBlocked(bank byte, start uint16, length int, barriers [][2]uint32) bool {
	first, last := decoder.Address24(bank, start), decoder.Address24(bank, start)+uint32(length)-1
	for _, barrier := range barriers {
		if first <= barrier[1] && last >= barrier[0] {
			return true
		}
	}
	for offset := 0; offset < length; offset++ {
		if repo.inDataRegion(bank, start+uint16(offset)) {
			return true
		}
	}
	return false
}

func (repo *repository) autorouteTargetBlocked(bank *bankState, target uint16) bool {
	if repo.inDataRegion(bank.ID, target) {
		return true
	}
	for _, excluded := range bank.Config.ExcludeRanges {
		if target >= excluded.Start && target < excluded.End {
			return true
		}
	}
	return false
}

// Guess only a bounded prefix. It is not a closed dispatch proof: the native
// RTS guard checks the actual stack address and otherwise executes normal RTS.
// Table data may have an explicit data-region bound; targets must not be data.
func (repo *repository) phaRTSTargets(bank *bankState, table uint16, starts []uint16) []uint16 {
	limit := 0x10000
	for _, start := range starts {
		if int(start) > int(table) && int(start) < limit {
			limit = int(start)
		}
	}
	for _, region := range repo.allDataRegions {
		if region.Bank == bank.ID && table >= region.Start && table < region.End && int(region.End) < limit {
			limit = int(region.End)
		}
	}
	var candidates []uint16
	end := int(table)
	for slot, count := int(table), 0; count < phaRTSTableLimit && slot+2 <= limit; slot, count = slot+2, count+1 {
		excluded := false
		for _, region := range bank.Config.ExcludeRanges {
			if slot < int(region.End) && slot+2 > int(region.Start) {
				excluded = true
				break
			}
		}
		if excluded {
			break
		}
		bytes, err := repo.image.Slice(bank.ID, uint16(slot), 2)
		if err != nil || !repo.image.IsROM(bank.ID, uint16(slot)) || !repo.image.IsROM(bank.ID, uint16(slot+1)) {
			break
		}
		pointer := uint16(bytes[0]) | uint16(bytes[1])<<8
		if pointer != 0 {
			if !repo.image.IsROM(bank.ID, pointer) || repo.autorouteTargetBlocked(bank, pointer) {
				break
			}
			if int(pointer) >= int(table) && int(pointer) < slot+2 {
				break
			}
			candidates = append(candidates, pointer)
		}
		end = slot + 2
	}
	var targets []uint16
	seen := make(map[uint16]bool)
	for _, target := range candidates {
		// Reject forward references into the table too, after its span is known.
		if seen[target] || (int(target) >= int(table) && int(target) < end) {
			continue
		}
		seen[target] = true
		targets = append(targets, target)
	}
	return targets
}

// routePHARTSDispatch adds guarded RTS target candidates inside declared
// function ranges. LDA/DEC/PHA/SEP are never collapsed: live DB, accumulator
// width and stack values determine the real destination, including targets
// outside the guessed same-bank table prefix.
func (repo *repository) routePHARTSDispatch() []PHARTSRoute {
	var routes []PHARTSRoute
	for _, bank := range repo.banks {
		barriers := nativeReturnConfigBarriers(bank.ID, bank.Config)
		starts := make([]uint16, 0, len(bank.Config.Entries))
		for _, entry := range bank.Config.Entries {
			starts = append(starts, entry.Start)
		}
		sort.Slice(starts, func(i, j int) bool { return starts[i] < starts[j] })
		scanned := make(map[uint16]struct{})
		for _, entry := range bank.Config.Entries {
			if len(entryHLEObligations(bank.Config, entry.Start)) != 0 || entry.TailCallPC != nil || entry.EntrySOffset != 0 || entry.ExitMX != nil {
				continue
			}
			end := 0x10000
			if entry.End != nil {
				end = int(*entry.End)
			} else if next := sort.Search(len(starts), func(i int) bool { return starts[i] > entry.Start }); next < len(starts) {
				end = int(starts[next])
			}
			for pc := int(entry.Start); pc+phaRTSWindow <= end; pc++ {
				lda := uint16(pc)
				if _, done := scanned[lda]; done {
					continue
				}
				code, err := repo.image.Slice(bank.ID, lda, phaRTSWindow)
				if err != nil || !repo.image.IsROM(bank.ID, lda) || !repo.image.IsROM(bank.ID, lda+phaRTSWindow-1) {
					continue
				}
				table, indexReg, found := matchesPHARTSDispatch(code)
				if !found {
					continue
				}
				scanned[lda] = struct{}{}
				site := lda + 7
				if repo.autorouteWindowBlocked(bank.ID, lda, phaRTSWindow, barriers) {
					continue
				}
				targets := repo.phaRTSTargets(bank, table, starts)
				if len(targets) == 0 {
					continue
				}
				bank.Config.RTSDispatch = append(bank.Config.RTSDispatch, config.RTSDispatch{
					SitePC: site, Targets: targets,
				})
				address := decoder.Address24(bank.ID, site)
				barriers = append(barriers, [2]uint32{address, address})
				routes = append(routes, PHARTSRoute{
					Site: decoder.Address24(bank.ID, site), Table: decoder.Address24(bank.ID, table),
					Count: len(targets), IndexReg: indexReg, Owner: entry.Name,
				})
			}
		}
	}
	return routes
}
