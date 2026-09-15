package regen

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
)

// Observations seed exact live-width AOT entries, never closed dispatch facts.
// Keep original census files across runs: a newly generated body is no longer
// reported missing in the next trace. Continuations must not become routines.
func (repo *repository) applyObservedDispatchCensus(romPath string, paths []string) (int, error) {
	if len(paths) == 0 {
		return 0, nil
	}
	bytes, err := os.ReadFile(romPath)
	if err != nil {
		return 0, err
	}
	hash := sha256.Sum256(bytes)
	demands := map[codegen.Variant]variantDemandEvidence{}
	for _, path := range paths {
		report, err := tooling.LoadDispatchCensusFile(path)
		if err != nil {
			return 0, err
		}
		if report.ROMHash != hex.EncodeToString(hash[:]) {
			return 0, fmt.Errorf("observed census %s ROM SHA-256 mismatch (or absent hash)", path)
		}
		if decoded, err := hex.DecodeString(report.TraceHash); err != nil || len(decoded) != sha256.Size {
			return 0, fmt.Errorf("observed census %s has no valid trace SHA-256", path)
		}
		if report.Overflow {
			return 0, fmt.Errorf("observed census %s overflowed; incomplete evidence is not importable", path)
		}
		for _, o := range report.Observations {
			if o.M > 1 || o.X > 1 || o.SitePC > 0xffffff || o.TargetPC > 0xffffff || o.ObservationCount == 0 {
				return 0, fmt.Errorf("observed census %s has malformed edge/state evidence", path)
			}
			if o.Found || o.Continuation {
				continue
			}
			if o.Emulation {
				return 0, fmt.Errorf("observed census %s target $%06X requires unsupported emulation entry semantics", path, o.TargetPC)
			}
			bank, pc := canonicalBank(repo.byBank, byte(o.TargetPC>>16)), uint16(o.TargetPC)
			if !repo.image.IsROM(byte(o.TargetPC>>16), pc) || repo.byBank[bank] == nil {
				return 0, fmt.Errorf("observed census %s target $%06X is unmapped or lacks a bank config", path, o.TargetPC)
			}
			if repo.inDataRegion(bank, pc) {
				return 0, fmt.Errorf("observed census %s target $%06X conflicts with an authored data region", path, o.TargetPC)
			}
			v := codegen.Variant{Address: uint32(bank)<<16 | uint32(pc), M: o.M, X: o.X}
			demands[v] = variantDemandEvidence{}
		}
	}
	repo.observedVariants = make(map[codegen.Variant]struct{}, len(demands))
	for v := range demands {
		repo.observedVariants[v] = struct{}{}
	}
	repo.applyDemands(demands)
	return len(demands), nil
}
