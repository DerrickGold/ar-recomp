package tooling

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const (
	staticAnalysisDatabaseVersion    = 1
	staticAnalysisDatabaseProvenance = "snesrecomp-static-shadow-v1"
)

// StaticAnalysisDatabase is the deterministic, fact-only output of shadow
// analysis. It intentionally excludes timestamps, runtime observations, and
// heuristic findings so it can be checked in as a hermetic AOT build input.
type StaticAnalysisDatabase struct {
	Version             int                               `json:"version"`
	Provenance          string                            `json:"provenance"`
	ShadowReportVersion int                               `json:"shadow_report_version"`
	ROM                 ShadowROM                         `json:"rom"`
	DispatchFacts       []analysis.DispatchFact           `json:"dispatch_facts,omitempty"`
	EntryFacts          []analysis.EntryFact              `json:"entry_facts,omitempty"`
	EntryTemplates      []analysis.EntryTemplatePlacement `json:"entry_templates,omitempty"`
}

// BuildStaticAnalysisDatabase selects only closed facts backed exclusively by
// static proof. Authored conflicts fail closed instead of being persisted.
func BuildStaticAnalysisDatabase(report ShadowReport) (StaticAnalysisDatabase, error) {
	if report.Version != shadowReportVersion {
		return StaticAnalysisDatabase{}, fmt.Errorf("shadow report version %d is not supported (want %d)", report.Version, shadowReportVersion)
	}
	if report.Summary.Conflicts != 0 {
		return StaticAnalysisDatabase{}, fmt.Errorf("shadow analysis has %d authored conflict(s):\n%s", report.Summary.Conflicts, FormatShadowConflicts(report))
	}
	dispatchFacts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
	entryFacts := SelectStaticProvenRoutineEntryFacts(report)
	entryFacts = append(entryFacts, SelectStaticProvenContinuationEntryFacts(report)...)
	database := StaticAnalysisDatabase{
		Version: staticAnalysisDatabaseVersion, Provenance: staticAnalysisDatabaseProvenance,
		ShadowReportVersion: report.Version, ROM: report.ROM,
		DispatchFacts: dispatchFacts, EntryFacts: entryFacts,
	}
	entryFactSet := make(map[analysis.EntryVariant]struct{}, len(entryFacts))
	for _, fact := range entryFacts {
		if fact.TemplateFree {
			entryFactSet[analysis.EntryVariant{PC: fact.PC, EntryMX: fact.EntryMX}] = struct{}{}
		}
	}
	for _, record := range report.EntryAblation.Entries {
		variant := analysis.EntryVariant{PC: record.PC, EntryMX: record.AuthoredMX}
		if _, selected := entryFactSet[variant]; selected {
			database.EntryTemplates = append(database.EntryTemplates, analysis.EntryTemplatePlacement{
				EntryVariant: variant, BankOrdinal: record.AuthoredOrdinal,
			})
		}
	}
	if err := normalizeAndValidateStaticAnalysisDatabase(&database); err != nil {
		return StaticAnalysisDatabase{}, err
	}
	return database, nil
}

// WriteStaticAnalysisDatabaseFile writes stable indented JSON. The database
// has no capture time or host path, so equal analysis produces equal bytes.
func WriteStaticAnalysisDatabaseFile(path string, database StaticAnalysisDatabase) error {
	if strings.TrimSpace(path) == "" {
		return errors.New("analysis database output path is empty")
	}
	if err := normalizeAndValidateStaticAnalysisDatabase(&database); err != nil {
		return err
	}
	var output bytes.Buffer
	encoder := json.NewEncoder(&output)
	encoder.SetEscapeHTML(false)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(database); err != nil {
		return fmt.Errorf("encode analysis database: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return fmt.Errorf("create analysis database directory: %w", err)
	}
	if err := os.WriteFile(path, output.Bytes(), 0o644); err != nil {
		return fmt.Errorf("write analysis database: %w", err)
	}
	return nil
}

// LoadStaticAnalysisDatabaseFile validates the schema and binds the facts to
// the exact headerless ROM bytes used by generation.
func LoadStaticAnalysisDatabaseFile(path, romPath string) (StaticAnalysisDatabase, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return StaticAnalysisDatabase{}, fmt.Errorf("read analysis database: %w", err)
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	var database StaticAnalysisDatabase
	if err := decoder.Decode(&database); err != nil {
		return StaticAnalysisDatabase{}, fmt.Errorf("decode analysis database: %w", err)
	}
	if err := requireJSONEOF(decoder); err != nil {
		return StaticAnalysisDatabase{}, fmt.Errorf("decode analysis database: %w", err)
	}
	if err := normalizeAndValidateStaticAnalysisDatabase(&database); err != nil {
		return StaticAnalysisDatabase{}, err
	}
	rom, err := shadowROMIdentity(romPath)
	if err != nil {
		return StaticAnalysisDatabase{}, err
	}
	if database.ROM != rom {
		return StaticAnalysisDatabase{}, fmt.Errorf(
			"analysis database ROM mismatch: database sha256=%s size=%d mapper=%s; input sha256=%s size=%d mapper=%s",
			database.ROM.SHA256, database.ROM.Size, database.ROM.Mapper,
			rom.SHA256, rom.Size, rom.Mapper)
	}
	return database, nil
}

func normalizeAndValidateStaticAnalysisDatabase(database *StaticAnalysisDatabase) error {
	if database.Version != staticAnalysisDatabaseVersion {
		return fmt.Errorf("analysis database version %d is not supported (want %d)", database.Version, staticAnalysisDatabaseVersion)
	}
	if database.Provenance != staticAnalysisDatabaseProvenance {
		return fmt.Errorf("analysis database provenance %q is not supported", database.Provenance)
	}
	if database.ShadowReportVersion != shadowReportVersion {
		return fmt.Errorf("analysis database shadow report version %d is not supported (want %d)", database.ShadowReportVersion, shadowReportVersion)
	}
	if len(database.ROM.SHA256) != sha256.Size*2 {
		return fmt.Errorf("analysis database ROM SHA-256 has %d hexadecimal characters, want %d", len(database.ROM.SHA256), sha256.Size*2)
	}
	if _, err := hex.DecodeString(database.ROM.SHA256); err != nil {
		return fmt.Errorf("analysis database ROM SHA-256: %w", err)
	}
	if database.ROM.Size <= 0 {
		return fmt.Errorf("analysis database ROM size %d is invalid", database.ROM.Size)
	}
	if database.ROM.Mapper != "lorom" {
		return fmt.Errorf("analysis database mapper %q is not supported", database.ROM.Mapper)
	}
	seenDispatch := make(map[uint32]struct{})
	for index := range database.DispatchFacts {
		fact := &database.DispatchFacts[index]
		if fact.SitePC > 0xffffff {
			return fmt.Errorf("analysis database dispatch fact %d has out-of-range site PC %#x", index, fact.SitePC)
		}
		if !hasOnlyStaticProof(fact.Evidence) || !fact.TargetSetClosed || len(fact.Targets) == 0 || len(fact.UnknownFields) != 0 {
			return fmt.Errorf("analysis database dispatch fact $%06X is not a closed static proof", fact.SitePC)
		}
		fact.Normalize()
		if _, duplicate := seenDispatch[fact.SitePC]; duplicate {
			return fmt.Errorf("analysis database has duplicate dispatch site $%06X", fact.SitePC)
		}
		seenDispatch[fact.SitePC] = struct{}{}
	}
	sort.Slice(database.DispatchFacts, func(i, j int) bool {
		return database.DispatchFacts[i].SitePC < database.DispatchFacts[j].SitePC
	})
	seenEntry := make(map[analysis.EntryVariant]struct{})
	for index := range database.EntryFacts {
		fact := &database.EntryFacts[index]
		if fact.PC > 0xffffff || fact.EntryMX.M > 1 || fact.EntryMX.X > 1 {
			return fmt.Errorf("analysis database entry fact %d has invalid variant $%X M%dX%d", index, fact.PC, fact.EntryMX.M, fact.EntryMX.X)
		}
		if !hasOnlyStaticProof(fact.Evidence) {
			return fmt.Errorf("analysis database entry fact $%06X M%dX%d is not a static proof", fact.PC, fact.EntryMX.M, fact.EntryMX.X)
		}
		if fact.Kind != analysis.EntryRoutine && fact.Kind != analysis.EntryContinuation {
			return fmt.Errorf("analysis database entry fact $%06X has unsupported kind %q", fact.PC, fact.Kind)
		}
		fact.Normalize()
		variant := analysis.EntryVariant{PC: fact.PC, EntryMX: fact.EntryMX}
		if _, duplicate := seenEntry[variant]; duplicate {
			return fmt.Errorf("analysis database has duplicate entry variant $%06X M%dX%d", fact.PC, fact.EntryMX.M, fact.EntryMX.X)
		}
		seenEntry[variant] = struct{}{}
	}
	sort.Slice(database.EntryFacts, func(i, j int) bool {
		left, right := database.EntryFacts[i], database.EntryFacts[j]
		if left.PC != right.PC {
			return left.PC < right.PC
		}
		if left.EntryMX.M != right.EntryMX.M {
			return left.EntryMX.M < right.EntryMX.M
		}
		return left.EntryMX.X < right.EntryMX.X
	})
	seenTemplates := make(map[analysis.EntryVariant]struct{})
	for index := range database.EntryTemplates {
		template := &database.EntryTemplates[index]
		if template.PC > 0xffffff || template.EntryMX.M > 1 || template.EntryMX.X > 1 || template.BankOrdinal < 0 {
			return fmt.Errorf("analysis database entry template %d is invalid", index)
		}
		template.PC &= 0xffffff
		variant := template.EntryVariant
		if _, duplicate := seenTemplates[variant]; duplicate {
			return fmt.Errorf("analysis database has duplicate entry template $%06X M%dX%d", template.PC, template.EntryMX.M, template.EntryMX.X)
		}
		seenTemplates[variant] = struct{}{}
		if _, selected := seenEntry[variant]; !selected {
			return fmt.Errorf("analysis database entry template $%06X M%dX%d has no matching entry fact", template.PC, template.EntryMX.M, template.EntryMX.X)
		}
	}
	for _, fact := range database.EntryFacts {
		if !fact.TemplateFree {
			continue
		}
		variant := analysis.EntryVariant{PC: fact.PC, EntryMX: fact.EntryMX}
		if _, found := seenTemplates[variant]; !found {
			return fmt.Errorf("analysis database template-free entry fact $%06X M%dX%d has no layout placement", fact.PC, fact.EntryMX.M, fact.EntryMX.X)
		}
	}
	sort.Slice(database.EntryTemplates, func(i, j int) bool {
		left, right := database.EntryTemplates[i], database.EntryTemplates[j]
		if left.PC != right.PC {
			return left.PC < right.PC
		}
		if left.EntryMX.M != right.EntryMX.M {
			return left.EntryMX.M < right.EntryMX.M
		}
		return left.EntryMX.X < right.EntryMX.X
	})
	return nil
}

func shadowROMIdentity(path string) (ShadowROM, error) {
	image, err := romimage.Load(path)
	if err != nil {
		return ShadowROM{}, err
	}
	hash := sha256.Sum256(image)
	return ShadowROM{SHA256: hex.EncodeToString(hash[:]), Size: len(image), Mapper: "lorom"}, nil
}

func requireJSONEOF(decoder *json.Decoder) error {
	var trailing any
	if err := decoder.Decode(&trailing); err == io.EOF {
		return nil
	} else if err != nil {
		return err
	}
	return errors.New("unexpected trailing JSON value")
}
