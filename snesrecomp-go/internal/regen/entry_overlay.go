package regen

import (
	"fmt"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// applyProvenEntryFacts withholds exact configured variants from the initial
// decode-root set while retaining their authored metadata templates in place.
// Static variant discovery must independently demand every withheld variant
// before emission is allowed to proceed.
func (repo *repository) applyProvenEntryFacts(facts []analysis.EntryFact) (int, error) {
	for _, original := range facts {
		fact := original
		fact.Normalize()
		if fact.Kind != analysis.EntryRoutine {
			return 0, fmt.Errorf("experimental entry fact $%06X has kind %q; only ordinary routines may be withheld", fact.PC, fact.Kind)
		}
		if len(fact.Evidence) == 0 {
			return 0, fmt.Errorf("experimental entry fact $%06X has no static proof evidence", fact.PC)
		}
		for _, evidence := range fact.Evidence {
			if evidence.Confidence != analysis.ConfidenceProven || !strings.HasPrefix(evidence.Source, "static.direct_js") {
				return 0, fmt.Errorf("experimental entry fact $%06X has non-call proof evidence %q (%s)", fact.PC, evidence.Source, evidence.Confidence)
			}
		}
		bankID := canonicalBank(repo.byBank, byte(fact.PC>>16))
		bank := repo.byBank[bankID]
		if bank == nil {
			return 0, fmt.Errorf("experimental entry fact $%06X has no configured bank", fact.PC)
		}
		variant := decoder.Variant{
			Address: decoder.Address24(bankID, uint16(fact.PC)),
			M:       fact.EntryMX.M & 1,
			X:       fact.EntryMX.X & 1,
		}
		found := false
		for _, entry := range bank.Config.Entries {
			if entryVariant(bankID, entry) == variant {
				if obligations := entryHLEObligations(bank.Config, entry.Start); len(obligations) != 0 {
					return 0, fmt.Errorf("experimental entry fact $%06X M%dX%d has authored HLE obligations (%s); refusing to withhold it", variant.Address, variant.M, variant.X, strings.Join(obligations, ", "))
				}
				found = true
				break
			}
		}
		if !found {
			return 0, fmt.Errorf("experimental entry fact $%06X M%dX%d has no exact authored declaration", variant.Address, variant.M, variant.X)
		}
		if _, duplicate := repo.dormantEntryRoots[variant]; duplicate {
			return 0, fmt.Errorf("duplicate experimental entry fact $%06X M%dX%d", variant.Address, variant.M, variant.X)
		}
		repo.dormantEntryRoots[variant] = struct{}{}
	}
	return len(facts), nil
}

func entryHLEObligations(cfg *config.Config, pc uint16) []string {
	var result []string
	if name := cfg.HLEFunctions[pc]; name != "" {
		result = append(result, "hle_func:"+name)
	}
	if conditional := cfg.HLEFunctionsIf[pc]; conditional.Function != "" {
		result = append(result, "hle_func_if:"+conditional.Function+":"+conditional.Predicate)
	}
	for _, candidate := range cfg.HLESPCUpload {
		if candidate == pc {
			result = append(result, "hle_spc_upload")
			break
		}
	}
	sort.Strings(result)
	return result
}

func entryVariant(bank byte, entry config.Entry) decoder.Variant {
	return decoder.Variant{
		Address: decoder.Address24(bank, entry.Start),
		M:       entry.EntryMX.M & 1,
		X:       entry.EntryMX.X & 1,
	}
}

func (repo *repository) entryRootDormant(bank byte, entry config.Entry) bool {
	_, dormant := repo.dormantEntryRoots[entryVariant(bank, entry)]
	return dormant
}

func (repo *repository) activeSiblingAddress(bank byte, pc uint16) bool {
	state := repo.byBank[bank]
	if state == nil {
		return false
	}
	for _, entry := range state.Config.Entries {
		if entry.Start == pc && !repo.entryRootDormant(bank, entry) {
			return true
		}
	}
	return false
}

func (repo *repository) activateDemandedEntryRoot(variant decoder.Variant) bool {
	if _, dormant := repo.dormantEntryRoots[variant]; !dormant {
		return false
	}
	delete(repo.dormantEntryRoots, variant)
	return true
}

func (repo *repository) unrecoveredProvenEntryFactsError() error {
	variants := make([]decoder.Variant, 0, len(repo.dormantEntryRoots))
	for variant := range repo.dormantEntryRoots {
		variants = append(variants, variant)
	}
	sort.Slice(variants, func(i, j int) bool {
		if variants[i].Address != variants[j].Address {
			return variants[i].Address < variants[j].Address
		}
		if variants[i].M != variants[j].M {
			return variants[i].M < variants[j].M
		}
		return variants[i].X < variants[j].X
	})
	lines := make([]string, 0, len(variants))
	for _, variant := range variants {
		lines = append(lines, fmt.Sprintf("  $%02X:%04X M%dX%d", byte(variant.Address>>16), uint16(variant.Address), variant.M, variant.X))
	}
	return fmt.Errorf("experimental static entry analysis failed closed: %d withheld routine root(s) were not rediscovered:\n%s", len(variants), strings.Join(lines, "\n"))
}
