package tooling

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

const dispatchCensusVersion = 1

// DispatchObservation is runtime evidence for one source/target/M/X tuple.
// Found records whether the sparse generated registry contained a body at the
// time of observation; it is evidence, not authored configuration.
type DispatchObservation struct {
	SitePC           uint32 `json:"site_pc"`
	TargetPC         uint32 `json:"target_pc"`
	M                uint8  `json:"m"`
	X                uint8  `json:"x"`
	Emulation        bool   `json:"emulation"`
	Found            bool   `json:"generated_body"`
	Mirrored         bool   `json:"mirrored"`
	Continuation     bool   `json:"continuation"`
	ObservationCount uint64 `json:"observation_count"`
}

type DispatchCensusReport struct {
	Version       int                   `json:"version"`
	ROMHash       string                `json:"rom_sha256,omitempty"`
	TraceHash     string                `json:"trace_sha256"`
	Provenance    string                `json:"provenance"`
	RawRecords    int                   `json:"raw_records"`
	Overflow      bool                  `json:"overflow"`
	Observations  []DispatchObservation `json:"observations"`
	MissingBodies int                   `json:"missing_generated_bodies"`
}

type dispatchTraceRecord struct {
	Channel  string          `json:"ch"`
	Site     json.RawMessage `json:"site"`
	Target   json.RawMessage `json:"target"`
	M        uint8           `json:"m"`
	X        uint8           `json:"x"`
	E        uint8           `json:"e"`
	Found    uint8           `json:"found"`
	Mirrored uint8           `json:"mirrored"`
	Continue uint8           `json:"continuation"`
	Hits     uint64          `json:"hits"`
	Overflow uint8           `json:"overflow"`
}

type dispatchObservationKey struct {
	SitePC, TargetPC uint32
	M, X             uint8
	Emulation        bool
	Found, Mirrored  bool
	Continuation     bool
}

func LoadDispatchCensus(tracePath, romPath string) (DispatchCensusReport, error) {
	contents, err := os.ReadFile(tracePath)
	if err != nil {
		return DispatchCensusReport{}, fmt.Errorf("read dispatch trace: %w", err)
	}
	report, err := ParseDispatchCensus(bytes.NewReader(contents))
	if err != nil {
		return DispatchCensusReport{}, err
	}
	traceHash := sha256.Sum256(contents)
	report.TraceHash = hex.EncodeToString(traceHash[:])
	if strings.TrimSpace(romPath) != "" {
		rom, err := os.ReadFile(romPath)
		if err != nil {
			return DispatchCensusReport{}, fmt.Errorf("read ROM for dispatch census: %w", err)
		}
		romHash := sha256.Sum256(rom)
		report.ROMHash = hex.EncodeToString(romHash[:])
	}
	return report, nil
}

func ParseDispatchCensus(input io.Reader) (DispatchCensusReport, error) {
	report := DispatchCensusReport{
		Version:    dispatchCensusVersion,
		Provenance: "snesrecomp-runtime-dispatch-census-v1",
	}
	observations := make(map[dispatchObservationKey]uint64)
	scanner := bufio.NewScanner(input)
	line := 0
	for scanner.Scan() {
		line++
		var record dispatchTraceRecord
		if err := json.Unmarshal(scanner.Bytes(), &record); err != nil {
			return DispatchCensusReport{}, fmt.Errorf("parse dispatch trace line %d: %w", line, err)
		}
		if record.Channel != "dispatch" {
			continue
		}
		report.RawRecords++
		if record.Overflow != 0 {
			report.Overflow = true
			continue
		}
		site, err := parseTraceAddress(record.Site)
		if err != nil {
			return DispatchCensusReport{}, fmt.Errorf("parse dispatch trace line %d site: %w", line, err)
		}
		target, err := parseTraceAddress(record.Target)
		if err != nil {
			return DispatchCensusReport{}, fmt.Errorf("parse dispatch trace line %d target: %w", line, err)
		}
		if record.Hits == 0 {
			return DispatchCensusReport{}, fmt.Errorf("parse dispatch trace line %d: zero hit count", line)
		}
		key := dispatchObservationKey{
			SitePC: site & 0xffffff, TargetPC: target & 0xffffff,
			M: record.M & 1, X: record.X & 1, Emulation: record.E != 0,
			Found: record.Found != 0, Mirrored: record.Mirrored != 0,
			Continuation: record.Continue != 0,
		}
		// Runtime milestones are cumulative powers of two followed by a final
		// count. Taking the maximum makes clean and interrupted traces equivalent
		// without double-counting milestone records.
		if record.Hits > observations[key] {
			observations[key] = record.Hits
		}
	}
	if err := scanner.Err(); err != nil {
		return DispatchCensusReport{}, fmt.Errorf("read dispatch trace: %w", err)
	}
	missingBodies := make(map[[3]uint32]struct{})
	for key, hits := range observations {
		report.Observations = append(report.Observations, DispatchObservation{
			SitePC: key.SitePC, TargetPC: key.TargetPC, M: key.M, X: key.X,
			Emulation: key.Emulation, Found: key.Found, Mirrored: key.Mirrored,
			Continuation:     key.Continuation,
			ObservationCount: hits,
		})
		if !key.Found && !key.Continuation {
			missingBodies[[3]uint32{key.TargetPC, uint32(key.M), uint32(key.X)}] = struct{}{}
		}
	}
	report.MissingBodies = len(missingBodies)
	sort.Slice(report.Observations, func(i, j int) bool {
		left, right := report.Observations[i], report.Observations[j]
		if left.SitePC != right.SitePC {
			return left.SitePC < right.SitePC
		}
		if left.TargetPC != right.TargetPC {
			return left.TargetPC < right.TargetPC
		}
		if left.M != right.M {
			return left.M < right.M
		}
		if left.X != right.X {
			return left.X < right.X
		}
		if left.Emulation != right.Emulation {
			return !left.Emulation
		}
		if left.Found != right.Found {
			return !left.Found
		}
		if left.Continuation != right.Continuation {
			return !left.Continuation
		}
		return !left.Mirrored && right.Mirrored
	})
	return report, nil
}

func parseTraceAddress(raw json.RawMessage) (uint32, error) {
	if len(raw) == 0 || bytes.Equal(raw, []byte("null")) {
		return 0, fmt.Errorf("missing address")
	}
	var text string
	if raw[0] == '"' {
		if err := json.Unmarshal(raw, &text); err != nil {
			return 0, err
		}
		text = strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(text), "$"), "0x")
		value, err := strconv.ParseUint(text, 16, 24)
		return uint32(value), err
	}
	var value uint32
	if err := json.Unmarshal(raw, &value); err != nil {
		return 0, err
	}
	if value > 0xffffff {
		return 0, fmt.Errorf("address %#x is wider than 24 bits", value)
	}
	return value, nil
}

func WriteDispatchCensus(output io.Writer, report DispatchCensusReport, format string, suggest bool) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(output)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		fmt.Fprintf(output, "dispatch census: %d unique edge/state observation(s), %d missing generated body/bodies\n",
			len(report.Observations), report.MissingBodies)
		if report.Overflow {
			fmt.Fprintln(output, "warning: runtime census capacity overflowed; report is incomplete")
		}
		for _, observation := range report.Observations {
			status := "generated"
			if !observation.Found {
				status = "MISSING"
			}
			if observation.Mirrored {
				status += ", mirrored"
			}
			if observation.Continuation {
				status = "continuation/return guard"
			}
			fmt.Fprintf(output, "  $%02X:%04X -> $%02X:%04X M%dX%d x%d %s\n",
				byte(observation.SitePC>>16), uint16(observation.SitePC),
				byte(observation.TargetPC>>16), uint16(observation.TargetPC),
				observation.M, observation.X, observation.ObservationCount, status)
		}
		if suggest {
			writeDispatchSuggestions(output, report.Observations)
		}
		return nil
	default:
		return fmt.Errorf("unknown dispatch census format %q (want text or json)", format)
	}
}

func writeDispatchSuggestions(output io.Writer, observations []DispatchObservation) {
	type suggestionKey struct {
		Target uint32
		M, X   uint8
	}
	suggestions := make(map[suggestionKey]uint64)
	for _, observation := range observations {
		if observation.Found || observation.Continuation {
			continue
		}
		key := suggestionKey{observation.TargetPC, observation.M, observation.X}
		suggestions[key] += observation.ObservationCount
	}
	if len(suggestions) == 0 {
		return
	}
	keys := make([]suggestionKey, 0, len(suggestions))
	for key := range suggestions {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].Target != keys[j].Target {
			return keys[i].Target < keys[j].Target
		}
		if keys[i].M != keys[j].M {
			return keys[i].M < keys[j].M
		}
		return keys[i].X < keys[j].X
	})
	fmt.Fprintln(output, "\n# Observed candidates only: verify routine/handler/continuation semantics before authoring.")
	for _, key := range keys {
		fmt.Fprintf(output, "# observed x%d\nfunc Observed_%02X_%04X_M%dX%d %04X entry_mx:%d,%d\n",
			suggestions[key], byte(key.Target>>16), uint16(key.Target), key.M, key.X,
			uint16(key.Target), key.M, key.X)
	}
}

func WriteDispatchCensusFile(path string, report DispatchCensusReport) error {
	var contents bytes.Buffer
	if err := WriteDispatchCensus(&contents, report, "json", false); err != nil {
		return err
	}
	if directory := filepath.Dir(path); directory != "." {
		if err := os.MkdirAll(directory, 0o755); err != nil {
			return fmt.Errorf("create dispatch census directory: %w", err)
		}
	}
	if err := os.WriteFile(path, contents.Bytes(), 0o644); err != nil {
		return fmt.Errorf("write dispatch census: %w", err)
	}
	return nil
}
