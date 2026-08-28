package regen

import (
	"fmt"
	"slices"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// applyProvenDispatchFacts adds an ephemeral dispatch overlay to the loaded
// configs. It never serializes the overlay, and validates every fact against
// the exact ROM bytes before allowing it to affect generated code.
func (repo *repository) applyProvenDispatchFacts(facts []analysis.DispatchFact) (int, error) {
	ordered := append([]analysis.DispatchFact(nil), facts...)
	for index := range ordered {
		ordered[index].Normalize()
	}
	sort.Slice(ordered, func(i, j int) bool { return ordered[i].SitePC < ordered[j].SitePC })

	existing := make(map[uint32]struct{})
	for _, bank := range repo.banks {
		for _, dispatch := range bank.Config.IndirectDispatch {
			existing[decoder.Address24(bank.ID, dispatch.SitePC)] = struct{}{}
		}
		for _, dispatch := range bank.Config.RTSDispatch {
			existing[decoder.Address24(bank.ID, dispatch.SitePC)] = struct{}{}
		}
	}

	applied := 0
	for _, fact := range ordered {
		if err := validateProvenFact(fact); err != nil {
			return applied, fmt.Errorf("analysis fact $%02X:%04X: %w", byte(fact.SitePC>>16), uint16(fact.SitePC), err)
		}
		if _, found := existing[fact.SitePC&0xffffff]; found {
			return applied, fmt.Errorf("analysis fact $%02X:%04X collides with an authored dispatch declaration", byte(fact.SitePC>>16), uint16(fact.SitePC))
		}
		bankID := byte(fact.SitePC >> 16)
		bank := repo.byBank[bankID]
		if bank == nil {
			return applied, fmt.Errorf("analysis fact $%02X:%04X has no matching bank config", bankID, uint16(fact.SitePC))
		}
		switch fact.Mnemonic {
		case "RTS":
			if err := repo.validateFactInstruction(bankID, fact); err != nil {
				return applied, fmt.Errorf("analysis fact $%02X:%04X: %w", bankID, uint16(fact.SitePC), err)
			}
			targets, err := sameBankPCs(bankID, fact.Targets, "target")
			if err != nil {
				return applied, fmt.Errorf("analysis fact $%02X:%04X: %w", bankID, uint16(fact.SitePC), err)
			}
			bank.Config.RTSDispatch = append(bank.Config.RTSDispatch, config.RTSDispatch{
				SitePC: uint16(fact.SitePC), Targets: targets,
			})
		case "PHA", "JMP", "JSR":
			directive, err := repo.provenIndirectDirective(bankID, fact)
			if err != nil {
				return applied, fmt.Errorf("analysis fact $%02X:%04X: %w", bankID, uint16(fact.SitePC), err)
			}
			bank.Config.IndirectDispatch = append(bank.Config.IndirectDispatch, directive)
			if repo.provenDispatchMX == nil {
				repo.provenDispatchMX = make(map[uint32]struct{})
			}
			repo.provenDispatchMX[fact.SitePC&0xffffff] = struct{}{}
			if fact.Mnemonic == "PHA" && fact.ReturnPC != nil {
				if repo.provenResumePCs == nil {
					repo.provenResumePCs = make(map[uint32]struct{})
				}
				repo.provenResumePCs[*fact.ReturnPC&0xffffff] = struct{}{}
			}
		default:
			return applied, fmt.Errorf("analysis fact $%02X:%04X uses unsupported instruction %s", bankID, uint16(fact.SitePC), fact.Mnemonic)
		}
		existing[fact.SitePC&0xffffff] = struct{}{}
		applied++
	}
	return applied, nil
}

func validateProvenFact(fact analysis.DispatchFact) error {
	if !fact.TargetSetClosed || len(fact.Targets) == 0 || len(fact.UnknownFields) != 0 {
		return fmt.Errorf("target set is not closed and fully known")
	}
	if len(fact.Evidence) == 0 {
		return fmt.Errorf("has no static proof evidence")
	}
	for _, evidence := range fact.Evidence {
		if evidence.Confidence != analysis.ConfidenceProven || !strings.HasPrefix(evidence.Source, "static.") {
			return fmt.Errorf("evidence %q has confidence %q, want static proven", evidence.Source, evidence.Confidence)
		}
	}
	switch fact.Mnemonic {
	case "RTS":
		if fact.Transfer != analysis.TransferResume || fact.TargetEntryKind != analysis.EntryContinuation {
			return fmt.Errorf("return dispatch has incompatible %s/%s semantics", fact.Transfer, fact.TargetEntryKind)
		}
	case "PHA":
		if fact.Transfer != analysis.TransferCall || fact.TargetEntryKind != analysis.EntryComputed || fact.IndexRegister != "A" {
			return fmt.Errorf("PHA dispatch has incompatible %s/%s index=%q semantics", fact.Transfer, fact.TargetEntryKind, fact.IndexRegister)
		}
		if fact.ReturnPC == nil {
			return fmt.Errorf("terminal PHA dispatch continuation semantics are not modeled yet")
		}
	case "JMP":
		if fact.Transfer != analysis.TransferTail || fact.TargetEntryKind != analysis.EntryComputed {
			return fmt.Errorf("JMP dispatch has incompatible %s/%s semantics", fact.Transfer, fact.TargetEntryKind)
		}
	case "JSR":
		if fact.Transfer != analysis.TransferCall || fact.TargetEntryKind != analysis.EntryComputed {
			return fmt.Errorf("JSR dispatch has incompatible %s/%s semantics", fact.Transfer, fact.TargetEntryKind)
		}
	default:
		return fmt.Errorf("instruction %s is not representable by the dispatch overlay", fact.Mnemonic)
	}
	return nil
}

func (repo *repository) validateFactInstruction(bank byte, fact analysis.DispatchFact) error {
	m, x := uint8(0), uint8(0)
	if len(fact.LiveMX) > 0 {
		m, x = fact.LiveMX[0].M&1, fact.LiveMX[0].X&1
	}
	offset, err := rom.LoROMOffset(bank, uint16(fact.SitePC))
	if err != nil || offset < 0 || offset >= len(repo.image) {
		return fmt.Errorf("site is outside the ROM mapping")
	}
	instruction, err := cpu65816.Decode(repo.image, offset, uint16(fact.SitePC), bank, m, x)
	if err != nil {
		return fmt.Errorf("decode site: %w", err)
	}
	if instruction.Mnemonic != fact.Mnemonic {
		return fmt.Errorf("ROM instruction is %s, analysis says %s", instruction.Mnemonic, fact.Mnemonic)
	}
	return nil
}

func (repo *repository) provenIndirectDirective(bank byte, fact analysis.DispatchFact) (config.IndirectDispatch, error) {
	tables, err := sameBankPCs(bank, fact.TableBases, "table base")
	if err != nil {
		return config.IndirectDispatch{}, err
	}
	if fact.Mnemonic == "PHA" && len(tables) == 0 {
		return config.IndirectDispatch{}, fmt.Errorf("PHA dispatch has no proven table base")
	}
	var returnPC *uint16
	if fact.ReturnPC != nil {
		values, convertErr := sameBankPCs(bank, []uint32{*fact.ReturnPC}, "return PC")
		if convertErr != nil {
			return config.IndirectDispatch{}, convertErr
		}
		value := values[0]
		returnPC = &value
	}
	directive := config.IndirectDispatch{
		SitePC: uint16(fact.SitePC), Count: len(fact.Targets), IndexReg: fact.IndexRegister,
		TableBases: tables, ReturnPC: returnPC, SEPMask: fact.SEPMask,
	}
	switch fact.Transfer {
	case analysis.TransferCall:
		directive.Transfer = config.IndirectTransferCall
	case analysis.TransferTail:
		directive.Transfer = config.IndirectTransferTail
	default:
		return config.IndirectDispatch{}, fmt.Errorf("unsupported transfer %q", fact.Transfer)
	}
	if directive.IndexReg != "A" && directive.IndexReg != "X" && directive.IndexReg != "Y" {
		return config.IndirectDispatch{}, fmt.Errorf("unsupported index register %q", directive.IndexReg)
	}

	if err := repo.validateFactInstruction(bank, fact); err != nil {
		return config.IndirectDispatch{}, err
	}
	m, x := uint8(0), uint8(0)
	if len(fact.LiveMX) > 0 {
		m, x = fact.LiveMX[0].M&1, fact.LiveMX[0].X&1
	}
	offset, _ := rom.LoROMOffset(bank, uint16(fact.SitePC))
	instruction, err := cpu65816.Decode(repo.image, offset, uint16(fact.SitePC), bank, m, x)
	if err != nil {
		return config.IndirectDispatch{}, fmt.Errorf("decode site: %w", err)
	}
	auth := decoder.DispatchAuth{
		Count: directive.Count, IndexReg: directive.IndexReg,
		TableBases: append([]uint16(nil), directive.TableBases...),
		ReturnPC:   directive.ReturnPC, SEPMask: directive.SEPMask, Transfer: string(directive.Transfer),
	}
	resolved, ok := decoder.ResolveDispatchTargets(repo.image, bank, instruction, auth)
	if !ok {
		return config.IndirectDispatch{}, fmt.Errorf("proven table cannot be resolved from the current ROM")
	}
	long := instruction.Length == 4 || len(directive.TableBases) == 3
	for index, target := range resolved {
		if fact.Mnemonic == "PHA" {
			pc := uint16(target + 1)
			if target == 0 || pc < 0x8000 {
				resolved[index] = 0
				continue
			}
			target = target&0xff0000 | uint32(pc)
		}
		if !long || target <= 0xffff {
			target = decoder.Address24(bank, uint16(target))
		}
		resolved[index] = target & 0xffffff
	}
	if !slices.Equal(resolved, fact.Targets) {
		return config.IndirectDispatch{}, fmt.Errorf("current ROM resolves targets %v, analysis proved %v", resolved, fact.Targets)
	}
	return directive, nil
}

func sameBankPCs(bank byte, addresses []uint32, field string) ([]uint16, error) {
	result := make([]uint16, len(addresses))
	for index, address := range addresses {
		if byte(address>>16) != bank {
			return nil, fmt.Errorf("%s $%02X:%04X is outside site bank $%02X", field, byte(address>>16), uint16(address), bank)
		}
		result[index] = uint16(address)
	}
	return result, nil
}
