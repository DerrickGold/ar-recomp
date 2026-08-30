package regen

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"hash"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/emitter"
)

type semanticFunctionRecord struct {
	address uint32
	m, x    uint8
	name    string
	source  string
}

type semanticAliasRecord struct {
	name    string
	address uint32
	m, x    uint8
}

// semanticSourceSHA256 fingerprints emitted behavior-bearing source while
// intentionally ignoring cfg order, translation-unit splitting, and forward
// declaration order. It includes every function body, the effective void
// alias targets, the runtime dispatch registry, and unresolved trap bodies.
func (repo *repository) semanticSourceSHA256(results map[byte][]*emitter.FunctionResult) (string, error) {
	var functions []semanticFunctionRecord
	var aliases []semanticAliasRecord
	for _, bank := range repo.banks {
		bankResults, selected := results[bank.ID]
		if !selected {
			continue
		}
		if len(bankResults) != len(bank.Config.Entries) {
			return "", fmt.Errorf("semantic hash bank $%02X has %d entries but %d results", bank.ID, len(bank.Config.Entries), len(bankResults))
		}
		seenAliases := make(map[string]struct{})
		for index, entry := range bank.Config.Entries {
			result := bankResults[index]
			if result == nil {
				return "", fmt.Errorf("semantic hash bank $%02X entry %d has no result", bank.ID, index)
			}
			name := entry.Name
			if name == "" {
				name = fmt.Sprintf("bank_%02X_%04X", bank.ID, entry.Start)
			}
			functions = append(functions, semanticFunctionRecord{
				address: uint32(bank.ID)<<16 | uint32(entry.Start),
				m:       entry.EntryMX.M & 1, x: entry.EntryMX.X & 1,
				name: name, source: result.Source,
			})
			if entry.Name != "" {
				if _, found := seenAliases[entry.Name]; !found {
					seenAliases[entry.Name] = struct{}{}
					aliases = append(aliases, semanticAliasRecord{
						name: entry.Name, address: uint32(bank.ID)<<16 | uint32(entry.Start),
						m: entry.EntryMX.M & 1, x: entry.EntryMX.X & 1,
					})
				}
			}
		}
	}
	sort.Slice(functions, func(i, j int) bool {
		if functions[i].address != functions[j].address {
			return functions[i].address < functions[j].address
		}
		if functions[i].m != functions[j].m {
			return functions[i].m < functions[j].m
		}
		if functions[i].x != functions[j].x {
			return functions[i].x < functions[j].x
		}
		if functions[i].name != functions[j].name {
			return functions[i].name < functions[j].name
		}
		return functions[i].source < functions[j].source
	})
	sort.Slice(aliases, func(i, j int) bool {
		if aliases[i].name != aliases[j].name {
			return aliases[i].name < aliases[j].name
		}
		if aliases[i].address != aliases[j].address {
			return aliases[i].address < aliases[j].address
		}
		if aliases[i].m != aliases[j].m {
			return aliases[i].m < aliases[j].m
		}
		return aliases[i].x < aliases[j].x
	})

	digest := sha256.New()
	writeSemanticHashField(digest, "snesrecomp-generated-semantic-source-v1")
	for _, function := range functions {
		writeSemanticHashField(digest, fmt.Sprintf("function:%06X:M%dX%d:%s", function.address&0xffffff, function.m, function.x, function.name))
		writeSemanticHashField(digest, function.source)
	}
	for _, alias := range aliases {
		writeSemanticHashField(digest, fmt.Sprintf("alias:%s:%06X:M%dX%d", alias.name, alias.address&0xffffff, alias.m, alias.x))
	}
	writeSemanticHashField(digest, repo.dispatchSource())
	writeSemanticHashField(digest, repo.unresolvedStubsSource())
	return hex.EncodeToString(digest.Sum(nil)), nil
}

func writeSemanticHashField(digest hash.Hash, value string) {
	var length [8]byte
	binary.LittleEndian.PutUint64(length[:], uint64(len(value)))
	_, _ = digest.Write(length[:])
	_, _ = digest.Write([]byte(value))
}
