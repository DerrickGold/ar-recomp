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
	"sort"
	"strconv"
	"strings"
)

const traceDiffVersion = 1

type TraceDiffOptions struct {
	Mode                   string
	OraclePath, RecompPath string
	Top                    int
	SkipZeroPage           bool
	Low, High              uint32
	MinPrefix              int
	FromGameFrame          uint64
	ToGameFrame            uint64
	ClockLow, ClockHigh    uint32
}

type TraceDiffSummary struct {
	OracleAddresses     int     `json:"oracle_addresses"`
	RecompAddresses     int     `json:"recomp_addresses"`
	CommonAddresses     int     `json:"common_addresses"`
	DivergentAddresses  int     `json:"divergent_addresses"`
	OracleOnlyAddresses int     `json:"oracle_only_addresses"`
	RecompOnlyAddresses int     `json:"recomp_only_addresses"`
	ConsistentAddresses int     `json:"consistent_addresses"`
	ConsistencyPercent  float64 `json:"consistency_percent"`
	OracleMaxFrame      uint64  `json:"oracle_max_frame"`
	RecompMaxFrame      uint64  `json:"recomp_max_frame"`
	FirstDivergenceGF   *uint64 `json:"first_divergence_game_frame,omitempty"`
}

type TraceDiffRow struct {
	Address       uint32 `json:"address"`
	OracleValue   byte   `json:"oracle_value"`
	RecompValue   byte   `json:"recomp_value"`
	OracleFrame   uint64 `json:"oracle_frame,omitempty"`
	RecompFrame   uint64 `json:"recomp_frame,omitempty"`
	GameFrame     uint64 `json:"game_frame,omitempty"`
	SequenceIndex int    `json:"sequence_index,omitempty"`
	RecompWrites  int    `json:"recomp_writes,omitempty"`
	Kind          string `json:"kind"`
}

type TraceDiffOnset struct {
	GameFrame uint64 `json:"game_frame"`
	Count     int    `json:"count"`
}

type TraceDiffReport struct {
	Version       int              `json:"version"`
	Mode          string           `json:"mode"`
	NoWrite       bool             `json:"no_write"`
	OracleSHA256  string           `json:"oracle_sha256"`
	RecompSHA256  string           `json:"recomp_sha256"`
	Summary       TraceDiffSummary `json:"summary"`
	Rows          []TraceDiffRow   `json:"rows"`
	Onsets        []TraceDiffOnset `json:"onsets,omitempty"`
	RowsTruncated int              `json:"rows_truncated,omitempty"`
}

type wramTraceRecord struct {
	Frame   uint64
	Address uint32
	Value   byte
}

func BuildTraceDiff(options TraceDiffOptions) (TraceDiffReport, error) {
	mode := strings.ToLower(strings.TrimSpace(options.Mode))
	if mode != "final" && mode != "sequence" && mode != "aligned" {
		return TraceDiffReport{}, fmt.Errorf("unknown trace-diff mode %q (want final, sequence, or aligned)", options.Mode)
	}
	if options.Top <= 0 {
		options.Top = 40
	}
	if options.High == 0 {
		options.High = 0x1ffff
	}
	if options.ToGameFrame == 0 {
		options.ToGameFrame = ^uint64(0)
	}
	if options.ClockLow == 0 && options.ClockHigh == 0 {
		options.ClockLow, options.ClockHigh = 0x88, 0x89
	}
	oracleContents, oracleRecords, err := loadWRAMTrace(options.OraclePath)
	if err != nil {
		return TraceDiffReport{}, err
	}
	recompContents, recompRecords, err := loadWRAMTrace(options.RecompPath)
	if err != nil {
		return TraceDiffReport{}, err
	}
	oracleHash, recompHash := sha256.Sum256(oracleContents), sha256.Sum256(recompContents)
	report := TraceDiffReport{
		Version: traceDiffVersion, Mode: mode, NoWrite: true,
		OracleSHA256: hex.EncodeToString(oracleHash[:]), RecompSHA256: hex.EncodeToString(recompHash[:]),
	}
	switch mode {
	case "final":
		buildFinalTraceDiff(&report, oracleRecords, recompRecords, options)
	case "sequence":
		buildSequenceTraceDiff(&report, oracleRecords, recompRecords, options)
	case "aligned":
		buildAlignedTraceDiff(&report, oracleRecords, recompRecords, options)
	}
	if len(report.Rows) > options.Top {
		report.RowsTruncated = len(report.Rows) - options.Top
		report.Rows = report.Rows[:options.Top]
	}
	if len(report.Onsets) > options.Top {
		report.Onsets = report.Onsets[:options.Top]
	}
	return report, nil
}

func loadWRAMTrace(path string) ([]byte, []wramTraceRecord, error) {
	contents, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, fmt.Errorf("read WRAM trace %s: %w", path, err)
	}
	var records []wramTraceRecord
	scanner := bufio.NewScanner(bytes.NewReader(contents))
	scanner.Buffer(make([]byte, 64*1024), 4*1024*1024)
	line := 0
	for scanner.Scan() {
		line++
		if len(bytes.TrimSpace(scanner.Bytes())) == 0 {
			continue
		}
		var raw struct {
			Frame   json.RawMessage `json:"f"`
			Address json.RawMessage `json:"adr"`
			Value   json.RawMessage `json:"val"`
		}
		if err := json.Unmarshal(scanner.Bytes(), &raw); err != nil {
			return nil, nil, fmt.Errorf("parse WRAM trace %s line %d: %w", path, line, err)
		}
		frame, err := parseFlexibleUint(raw.Frame, 10, 64)
		if err != nil {
			return nil, nil, fmt.Errorf("parse WRAM trace %s line %d frame: %w", path, line, err)
		}
		address, err := parseFlexibleUint(raw.Address, 16, 32)
		if err != nil {
			return nil, nil, fmt.Errorf("parse WRAM trace %s line %d address: %w", path, line, err)
		}
		value, err := parseFlexibleUint(raw.Value, 16, 8)
		if err != nil {
			return nil, nil, fmt.Errorf("parse WRAM trace %s line %d value: %w", path, line, err)
		}
		records = append(records, wramTraceRecord{Frame: frame, Address: uint32(address), Value: byte(value)})
	}
	if err := scanner.Err(); err != nil {
		return nil, nil, fmt.Errorf("scan WRAM trace %s: %w", path, err)
	}
	return contents, records, nil
}

func parseFlexibleUint(raw json.RawMessage, stringBase, bits int) (uint64, error) {
	if len(raw) == 0 {
		return 0, fmt.Errorf("missing value")
	}
	if raw[0] == '"' {
		var text string
		if err := json.Unmarshal(raw, &text); err != nil {
			return 0, err
		}
		text = strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(text), "0x"), "0X")
		text = strings.TrimPrefix(text, "$")
		return strconv.ParseUint(text, stringBase, bits)
	}
	var value uint64
	if err := json.Unmarshal(raw, &value); err != nil {
		return 0, err
	}
	if bits < 64 && value >= uint64(1)<<bits {
		return 0, fmt.Errorf("value %#x exceeds %d bits", value, bits)
	}
	return value, nil
}

type finalAddressState struct {
	Value       byte
	First, Last uint64
	Writes      int
}

func finalTraceState(records []wramTraceRecord) (map[uint32]finalAddressState, uint64) {
	states := make(map[uint32]finalAddressState)
	var maxFrame uint64
	for _, record := range records {
		state, found := states[record.Address]
		if !found {
			state.First = record.Frame
		}
		state.Value, state.Last, state.Writes = record.Value, record.Frame, state.Writes+1
		states[record.Address] = state
		if record.Frame > maxFrame {
			maxFrame = record.Frame
		}
	}
	return states, maxFrame
}

func buildFinalTraceDiff(report *TraceDiffReport, oracleRecords, recompRecords []wramTraceRecord, options TraceDiffOptions) {
	oracle, oracleMax := finalTraceState(oracleRecords)
	recomp, recompMax := finalTraceState(recompRecords)
	report.Summary.OracleAddresses, report.Summary.RecompAddresses = len(oracle), len(recomp)
	report.Summary.OracleMaxFrame, report.Summary.RecompMaxFrame = oracleMax, recompMax
	for address, oracleState := range oracle {
		recompState, common := recomp[address]
		if !common {
			report.Summary.OracleOnlyAddresses++
			continue
		}
		report.Summary.CommonAddresses++
		if oracleState.Value != recompState.Value && keepTraceAddress(address, options, false) {
			report.Rows = append(report.Rows, TraceDiffRow{
				Address: address, OracleValue: oracleState.Value, RecompValue: recompState.Value,
				OracleFrame: oracleState.First, RecompFrame: recompState.First, Kind: "divergent_final_value",
			})
		}
	}
	for address, state := range recomp {
		if _, found := oracle[address]; found {
			continue
		}
		report.Summary.RecompOnlyAddresses++
		if keepTraceAddress(address, options, false) {
			report.Rows = append(report.Rows, TraceDiffRow{Address: address, RecompValue: state.Value, RecompFrame: state.First, RecompWrites: state.Writes, Kind: "recomp_only_write"})
		}
	}
	report.Summary.DivergentAddresses = countTraceRows(report.Rows, "divergent_final_value")
	sort.Slice(report.Rows, func(i, j int) bool {
		if report.Rows[i].Kind != report.Rows[j].Kind {
			return report.Rows[i].Kind < report.Rows[j].Kind
		}
		if report.Rows[i].RecompFrame != report.Rows[j].RecompFrame {
			return report.Rows[i].RecompFrame < report.Rows[j].RecompFrame
		}
		return report.Rows[i].Address < report.Rows[j].Address
	})
}

type sequenceValue struct {
	Frame uint64
	Value byte
}

func traceSequences(records []wramTraceRecord) map[uint32][]sequenceValue {
	sequences := make(map[uint32][]sequenceValue)
	for _, record := range records {
		values := sequences[record.Address]
		if len(values) == 0 || values[len(values)-1].Value != record.Value {
			sequences[record.Address] = append(values, sequenceValue{record.Frame, record.Value})
		}
	}
	return sequences
}

func buildSequenceTraceDiff(report *TraceDiffReport, oracleRecords, recompRecords []wramTraceRecord, options TraceDiffOptions) {
	oracle, recomp := traceSequences(oracleRecords), traceSequences(recompRecords)
	report.Summary.OracleAddresses, report.Summary.RecompAddresses = len(oracle), len(recomp)
	for address, oracleValues := range oracle {
		recompValues, common := recomp[address]
		if !common || !keepTraceAddress(address, options, true) {
			continue
		}
		report.Summary.CommonAddresses++
		limit := len(oracleValues)
		if len(recompValues) < limit {
			limit = len(recompValues)
		}
		divergence := -1
		for index := 0; index < limit; index++ {
			if oracleValues[index].Value != recompValues[index].Value {
				divergence = index
				break
			}
		}
		if divergence < 0 {
			report.Summary.ConsistentAddresses++
			continue
		}
		if divergence >= options.MinPrefix {
			report.Rows = append(report.Rows, TraceDiffRow{
				Address: address, OracleValue: oracleValues[divergence].Value, RecompValue: recompValues[divergence].Value,
				OracleFrame: oracleValues[divergence].Frame, RecompFrame: recompValues[divergence].Frame,
				SequenceIndex: divergence, Kind: "sequence_divergence",
			})
		}
	}
	report.Summary.DivergentAddresses = len(report.Rows)
	if report.Summary.CommonAddresses != 0 {
		report.Summary.ConsistencyPercent = 100 * float64(report.Summary.ConsistentAddresses) / float64(report.Summary.CommonAddresses)
	}
	sort.Slice(report.Rows, func(i, j int) bool {
		if report.Rows[i].RecompFrame != report.Rows[j].RecompFrame {
			return report.Rows[i].RecompFrame < report.Rows[j].RecompFrame
		}
		return report.Rows[i].Address < report.Rows[j].Address
	})
}

func buildAlignedTraceDiff(report *TraceDiffReport, oracleRecords, recompRecords []wramTraceRecord, options TraceDiffOptions) {
	oracleDeltas, oracleMax := alignedTraceDeltas(oracleRecords, options.ClockLow, options.ClockHigh)
	recompDeltas, recompMax := alignedTraceDeltas(recompRecords, options.ClockLow, options.ClockHigh)
	oracleAddresses := traceAddressSets(oracleRecords, options)
	recompAddresses := traceAddressSets(recompRecords, options)
	report.Summary.OracleAddresses = len(oracleAddresses)
	report.Summary.RecompAddresses = len(recompAddresses)
	for address := range oracleAddresses {
		if _, found := recompAddresses[address]; found {
			report.Summary.CommonAddresses++
		} else {
			report.Summary.OracleOnlyAddresses++
		}
	}
	for address := range recompAddresses {
		if _, found := oracleAddresses[address]; !found {
			report.Summary.RecompOnlyAddresses++
		}
	}
	report.Summary.OracleMaxFrame, report.Summary.RecompMaxFrame = oracleMax, recompMax
	maxFrame := oracleMax
	if recompMax > maxFrame {
		maxFrame = recompMax
	}
	if options.ToGameFrame < maxFrame {
		maxFrame = options.ToGameFrame
	}
	oracleState, recompState := make(map[uint32]byte), make(map[uint32]byte)
	first := make(map[uint32]TraceDiffRow)
	onsets := make(map[uint64]int)
	for gameFrame := uint64(0); gameFrame <= maxFrame; gameFrame++ {
		for address, value := range oracleDeltas[gameFrame] {
			oracleState[address] = value
		}
		for address, value := range recompDeltas[gameFrame] {
			recompState[address] = value
		}
		if gameFrame < options.FromGameFrame {
			continue
		}
		touched := make(map[uint32]struct{})
		for address := range oracleDeltas[gameFrame] {
			touched[address] = struct{}{}
		}
		for address := range recompDeltas[gameFrame] {
			touched[address] = struct{}{}
		}
		for address := range touched {
			if _, found := first[address]; found || !keepTraceAddress(address, options, true) {
				continue
			}
			oracleValue, oracleKnown := oracleState[address]
			recompValue, recompKnown := recompState[address]
			if oracleKnown && recompKnown && oracleValue != recompValue {
				first[address] = TraceDiffRow{Address: address, OracleValue: oracleValue, RecompValue: recompValue, GameFrame: gameFrame, Kind: "aligned_divergence"}
				onsets[gameFrame]++
			}
		}
		if gameFrame == ^uint64(0) {
			break
		}
	}
	for _, row := range first {
		report.Rows = append(report.Rows, row)
	}
	sort.Slice(report.Rows, func(i, j int) bool {
		if report.Rows[i].GameFrame != report.Rows[j].GameFrame {
			return report.Rows[i].GameFrame < report.Rows[j].GameFrame
		}
		return report.Rows[i].Address < report.Rows[j].Address
	})
	for gameFrame, count := range onsets {
		report.Onsets = append(report.Onsets, TraceDiffOnset{gameFrame, count})
	}
	sort.Slice(report.Onsets, func(i, j int) bool { return report.Onsets[i].GameFrame < report.Onsets[j].GameFrame })
	report.Summary.DivergentAddresses = len(report.Rows)
	if len(report.Rows) != 0 {
		value := report.Rows[0].GameFrame
		report.Summary.FirstDivergenceGF = &value
	}
}

func traceAddressSets(records []wramTraceRecord, options TraceDiffOptions) map[uint32]struct{} {
	addresses := make(map[uint32]struct{})
	for _, record := range records {
		if keepTraceAddress(record.Address, options, true) {
			addresses[record.Address] = struct{}{}
		}
	}
	return addresses
}

func alignedTraceDeltas(records []wramTraceRecord, clockLow, clockHigh uint32) (map[uint64]map[uint32]byte, uint64) {
	deltas := make(map[uint64]map[uint32]byte)
	var low, high byte
	var gameFrame, maxFrame uint64
	for _, record := range records {
		if record.Address == clockLow {
			low = record.Value
			gameFrame = uint64(low) | uint64(high)<<8
		} else if record.Address == clockHigh {
			high = record.Value
			gameFrame = uint64(low) | uint64(high)<<8
		} else {
			if deltas[gameFrame] == nil {
				deltas[gameFrame] = make(map[uint32]byte)
			}
			deltas[gameFrame][record.Address] = record.Value
		}
		if gameFrame > maxFrame {
			maxFrame = gameFrame
		}
	}
	return deltas, maxFrame
}

func keepTraceAddress(address uint32, options TraceDiffOptions, skipClock bool) bool {
	if skipClock && (address == options.ClockLow || address == options.ClockHigh) {
		return false
	}
	if options.SkipZeroPage && address <= 0x1ff {
		return false
	}
	return address >= options.Low && address <= options.High
}

func countTraceRows(rows []TraceDiffRow, kind string) int {
	count := 0
	for _, row := range rows {
		if row.Kind == kind {
			count++
		}
	}
	return count
}

func formatWRAMAddress(address uint32) string {
	if address >= 0x10000 {
		return fmt.Sprintf("$7F:%04X", address-0x10000)
	}
	return fmt.Sprintf("$7E:%04X", address)
}

func WriteTraceDiff(output io.Writer, report TraceDiffReport, format string) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(output)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		fmt.Fprintf(output, "trace diff %s: oracle=%d recomp=%d common=%d divergent=%d\n",
			report.Mode, report.Summary.OracleAddresses, report.Summary.RecompAddresses,
			report.Summary.CommonAddresses, report.Summary.DivergentAddresses)
		if report.Mode == "sequence" {
			fmt.Fprintf(output, "  prefix-consistent=%d (%.1f%%)\n", report.Summary.ConsistentAddresses, report.Summary.ConsistencyPercent)
		}
		if report.Summary.FirstDivergenceGF != nil {
			fmt.Fprintf(output, "  first divergence at game-frame %d\n", *report.Summary.FirstDivergenceGF)
		}
		for _, row := range report.Rows {
			extra := ""
			switch row.Kind {
			case "sequence_divergence":
				extra = fmt.Sprintf(" seq=%d recomp_frame=%d", row.SequenceIndex, row.RecompFrame)
			case "aligned_divergence":
				extra = fmt.Sprintf(" game_frame=%d", row.GameFrame)
			case "recomp_only_write":
				extra = fmt.Sprintf(" recomp_only first_frame=%d writes=%d", row.RecompFrame, row.RecompWrites)
			default:
				extra = fmt.Sprintf(" oracle_first=%d recomp_first=%d", row.OracleFrame, row.RecompFrame)
			}
			fmt.Fprintf(output, "  %-8s oracle=$%02X recomp=$%02X %s%s\n", formatWRAMAddress(row.Address), row.OracleValue, row.RecompValue, row.Kind, extra)
		}
		if report.RowsTruncated != 0 {
			fmt.Fprintf(output, "... (%d more; raise --top)\n", report.RowsTruncated)
		}
		if len(report.Onsets) != 0 {
			fmt.Fprintln(output, "  divergence onset by game-frame:")
			for _, onset := range report.Onsets {
				fmt.Fprintf(output, "    gf %d: %d new address(es)\n", onset.GameFrame, onset.Count)
			}
		}
		return nil
	default:
		return fmt.Errorf("unknown trace-diff format %q (want text or json)", format)
	}
}
