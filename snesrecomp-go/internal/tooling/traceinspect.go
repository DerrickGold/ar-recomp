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

	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const traceInspectionVersion = 1

type TraceRange struct {
	Low  uint32 `json:"low"`
	High uint32 `json:"high"`
}

type TraceInspectOptions struct {
	TracePath, MetadataPath, ROMPath string
	Summary, Diagnose                bool
	Channels                         []string
	Function                         string
	Misdecodes, Leaks, VMADD         bool
	VRAM, WRAM                       *TraceRange
	Around                           *uint64
	Window                           uint64
	Limit                            int
}

type TraceSummary struct {
	Events       int            `json:"events"`
	InvalidLines int            `json:"invalid_lines"`
	Channels     map[string]int `json:"channels"`
	FirstSeq     uint64         `json:"first_seq"`
	LastSeq      uint64         `json:"last_seq"`
	FirstHF      uint64         `json:"first_host_frame"`
	LastHF       uint64         `json:"last_host_frame"`
	FirstGF      uint64         `json:"first_game_frame"`
	LastGF       uint64         `json:"last_game_frame"`
	Misdecodes   int            `json:"entry_misdecodes"`
	Leaks        int            `json:"call_mx_leaks"`
	DispatchMiss int            `json:"dispatch_misses"`
	Garbage      int            `json:"garbage_variants"`
}

type TraceFinding struct {
	Priority   int      `json:"priority"`
	Kind       string   `json:"kind"`
	SitePC     *uint32  `json:"site_pc,omitempty"`
	TargetPC   *uint32  `json:"target_pc,omitempty"`
	Count      uint64   `json:"count"`
	Message    string   `json:"message"`
	Suggestion string   `json:"suggestion,omitempty"`
	Evidence   []string `json:"evidence,omitempty"`
}

type TraceInspectionReport struct {
	Version     int              `json:"version"`
	Mode        string           `json:"mode"`
	NoWrite     bool             `json:"no_write"`
	TraceSHA256 string           `json:"trace_sha256"`
	Summary     TraceSummary     `json:"summary"`
	Findings    []TraceFinding   `json:"findings,omitempty"`
	Events      []map[string]any `json:"events,omitempty"`
	Truncated   int              `json:"truncated_events,omitempty"`
}

func ParseTraceRange(value string) (TraceRange, error) {
	parts := strings.Split(strings.TrimSpace(value), "-")
	if len(parts) > 2 || len(parts) == 0 {
		return TraceRange{}, fmt.Errorf("bad hexadecimal range %q", value)
	}
	parse := func(text string) (uint32, error) {
		text = strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(text), "0x"), "0X")
		text = strings.TrimPrefix(text, "$")
		parsed, err := strconv.ParseUint(text, 16, 32)
		return uint32(parsed), err
	}
	low, err := parse(parts[0])
	if err != nil {
		return TraceRange{}, fmt.Errorf("parse range %q: %w", value, err)
	}
	high := low
	if len(parts) == 2 {
		high, err = parse(parts[1])
		if err != nil {
			return TraceRange{}, fmt.Errorf("parse range %q: %w", value, err)
		}
	}
	if high < low {
		return TraceRange{}, fmt.Errorf("range %q has high below low", value)
	}
	return TraceRange{Low: low, High: high}, nil
}

func BuildTraceInspection(options TraceInspectOptions) (TraceInspectionReport, error) {
	contents, err := os.ReadFile(options.TracePath)
	if err != nil {
		return TraceInspectionReport{}, fmt.Errorf("read runtime trace: %w", err)
	}
	hash := sha256.Sum256(contents)
	report := TraceInspectionReport{
		Version: traceInspectionVersion, Mode: "runtime_trace_inspection", NoWrite: true,
		TraceSHA256: hex.EncodeToString(hash[:]), Summary: TraceSummary{Channels: make(map[string]int)},
	}
	events, legacy := parseLegacyDispatchDump(contents)
	if !legacy {
		scanner := bufio.NewScanner(bytes.NewReader(contents))
		scanner.Buffer(make([]byte, 64*1024), 8*1024*1024)
		for scanner.Scan() {
			line := bytes.TrimSpace(scanner.Bytes())
			if len(line) == 0 {
				continue
			}
			decoder := json.NewDecoder(bytes.NewReader(line))
			decoder.UseNumber()
			var event map[string]any
			if err := decoder.Decode(&event); err != nil {
				report.Summary.InvalidLines++
				continue
			}
			events = append(events, event)
		}
		if err := scanner.Err(); err != nil {
			return TraceInspectionReport{}, fmt.Errorf("scan runtime trace: %w", err)
		}
	}
	for _, event := range events {
		updateTraceSummary(&report.Summary, event)
	}
	if options.Diagnose {
		metadata, metadataErr := loadOptionalGeneratedMetadata(options.MetadataPath)
		if metadataErr != nil {
			return TraceInspectionReport{}, metadataErr
		}
		var image romimage.Image
		if strings.TrimSpace(options.ROMPath) != "" {
			image, err = romimage.Load(options.ROMPath)
			if err != nil {
				return TraceInspectionReport{}, err
			}
		}
		report.Findings = diagnoseTrace(events, metadata, image)
	}
	selected := filterTraceEvents(events, options)
	if options.Limit <= 0 {
		options.Limit = 200
	}
	if len(selected) > options.Limit {
		report.Truncated = len(selected) - options.Limit
		selected = selected[:options.Limit]
	}
	if !options.Summary && !options.Diagnose || traceHasExplicitFilter(options) {
		report.Events = selected
	}
	return report, nil
}

func parseLegacyDispatchDump(contents []byte) ([]map[string]any, bool) {
	decoder := json.NewDecoder(bytes.NewReader(contents))
	decoder.UseNumber()
	var root map[string]any
	if err := decoder.Decode(&root); err != nil {
		return nil, false
	}
	dispatchLog, ok := root["dispatch_log"].(map[string]any)
	if !ok {
		return nil, false
	}
	rawEvents, ok := dispatchLog["events"].([]any)
	if !ok {
		return nil, true
	}
	events := make([]map[string]any, 0, len(rawEvents))
	for index, raw := range rawEvents {
		entry, ok := raw.(map[string]any)
		if !ok {
			continue
		}
		mx, _ := traceUint(entry, "mx")
		events = append(events, map[string]any{
			"seq": json.Number(strconv.Itoa(index + 1)), "hf": json.Number("0"), "gf": json.Number("0"),
			"ch": "dispatch", "fn": "legacy_dispatch_log", "site": entry["source_pc24"], "target": entry["pc24"],
			"m": json.Number(strconv.Itoa(int((mx >> 1) & 1))), "x": json.Number(strconv.Itoa(int(mx & 1))),
			"found": entry["found"], "hits": json.Number("1"), "continuation": false, "trapped": false,
		})
	}
	return events, true
}

func updateTraceSummary(summary *TraceSummary, event map[string]any) {
	seq, _ := traceUint(event, "seq")
	hf, _ := traceUint(event, "hf")
	gf, _ := traceUint(event, "gf")
	if summary.Events == 0 {
		summary.FirstSeq, summary.FirstHF, summary.FirstGF = seq, hf, gf
	}
	summary.Events++
	summary.LastSeq, summary.LastHF, summary.LastGF = seq, hf, gf
	channel := traceString(event, "ch")
	summary.Channels[channel]++
	if channel == "func" && traceBool(event, "misdecode") {
		summary.Misdecodes++
	}
	if channel == "call" && traceBool(event, "leak") {
		summary.Leaks++
	}
	if channel == "dispmiss" || channel == "dispatch" && (traceBool(event, "trapped") || !traceBoolDefault(event, "found", true)) {
		summary.DispatchMiss++
	}
	if channel == "garbage" {
		summary.Garbage++
	}
}

func traceHasExplicitFilter(options TraceInspectOptions) bool {
	return len(options.Channels) != 0 || options.Function != "" || options.Misdecodes || options.Leaks || options.VMADD || options.VRAM != nil || options.WRAM != nil || options.Around != nil
}

func filterTraceEvents(events []map[string]any, options TraceInspectOptions) []map[string]any {
	channels := make(map[string]struct{})
	for _, channel := range options.Channels {
		channels[strings.TrimSpace(channel)] = struct{}{}
	}
	var selected []map[string]any
	for _, event := range events {
		channel := traceString(event, "ch")
		if len(channels) != 0 {
			if _, ok := channels[channel]; !ok {
				continue
			}
		}
		if options.Function != "" && !strings.Contains(traceString(event, "fn"), options.Function) {
			continue
		}
		if options.Misdecodes && (channel != "func" || !traceBool(event, "misdecode")) {
			continue
		}
		if options.Leaks && (channel != "call" || !traceBool(event, "leak")) {
			continue
		}
		if options.VMADD && channel != "vmadd" {
			continue
		}
		if options.VRAM != nil {
			value, ok := traceHex(event, "va")
			if channel != "vram" || !ok || value < options.VRAM.Low || value > options.VRAM.High {
				continue
			}
		}
		if options.WRAM != nil {
			value, ok := traceHex(event, "off")
			if (channel != "wram" && channel != "stack") || !ok || value < options.WRAM.Low || value > options.WRAM.High {
				continue
			}
		}
		if options.Around != nil {
			seq, ok := traceUint(event, "seq")
			if !ok || absoluteUintDifference(seq, *options.Around) > options.Window {
				continue
			}
		}
		selected = append(selected, event)
	}
	return selected
}

type traceMissKey struct {
	Target uint32
	M, X   uint8
}

type traceMissAggregate struct {
	Count   uint64
	Sources map[uint32]struct{}
	Hidden  bool
	Site    uint32
}

func diagnoseTrace(events []map[string]any, metadata *GeneratedMetadata, image romimage.Image) []TraceFinding {
	misses := make(map[traceMissKey]*traceMissAggregate)
	var findings []TraceFinding
	for _, event := range events {
		channel := traceString(event, "ch")
		if channel == "call" && traceBool(event, "leak") {
			site, _ := traceAddressField(event, "site")
			findings = append(findings, TraceFinding{
				Priority: 70, Kind: "mx_leak", SitePC: optionalTraceAddress(site), Count: 1,
				Message:  fmt.Sprintf("runtime M/X differs from the decoder expectation at %s", traceString(event, "fn")),
				Evidence: []string{fmt.Sprintf("runtime M%sX%s expected M%sX%s", traceString(event, "m"), traceString(event, "x"), traceString(event, "em"), traceString(event, "ex"))},
			})
		}
		if channel == "garbage" {
			pc, _ := traceAddressField(event, "pc")
			findings = append(findings, TraceFinding{Priority: 60, Kind: "garbage_variant", SitePC: optionalTraceAddress(pc), Count: 1, Message: "runtime entered a variant classified as garbage", Evidence: []string{traceString(event, "variant")}})
		}
		isMiss := channel == "dispmiss" || channel == "dispatch" && (traceBool(event, "trapped") || !traceBoolDefault(event, "found", true))
		if !isMiss {
			continue
		}
		targetField, siteField := "to", "from"
		if channel == "dispatch" {
			targetField, siteField = "target", "site"
		}
		target, targetOK := traceAddressField(event, targetField)
		site, _ := traceAddressField(event, siteField)
		if !targetOK {
			continue
		}
		m, _ := traceUint(event, "m")
		x, _ := traceUint(event, "x")
		if channel == "dispmiss" {
			if value, ok := traceUint(event, "mnow"); ok {
				m = value
			}
			if value, ok := traceUint(event, "xnow"); ok {
				x = value
			}
		}
		key := traceMissKey{Target: target, M: uint8(m) & 1, X: uint8(x) & 1}
		aggregate := misses[key]
		if aggregate == nil {
			aggregate = &traceMissAggregate{Sources: make(map[uint32]struct{}), Site: site}
			misses[key] = aggregate
		}
		hits, ok := traceUint(event, "hits")
		if !ok || hits == 0 {
			hits = 1
		}
		aggregate.Count += hits
		aggregate.Sources[site] = struct{}{}
		if stack, ok := traceHex(event, "S"); ok && stack < 0x200 {
			aggregate.Hidden = true
		}
	}
	for key, aggregate := range misses {
		finding := classifyTraceMiss(key, aggregate, metadata, image)
		findings = append(findings, finding)
	}
	sort.Slice(findings, func(i, j int) bool {
		if findings[i].Priority != findings[j].Priority {
			return findings[i].Priority > findings[j].Priority
		}
		left, right := uint32(0), uint32(0)
		if findings[i].TargetPC != nil {
			left = *findings[i].TargetPC
		}
		if findings[j].TargetPC != nil {
			right = *findings[j].TargetPC
		}
		return left < right
	})
	return findings
}

func classifyTraceMiss(key traceMissKey, aggregate *traceMissAggregate, metadata *GeneratedMetadata, image romimage.Image) TraceFinding {
	target := key.Target & 0xffffff
	site := aggregate.Site & 0xffffff
	finding := TraceFinding{
		Priority: 90, Kind: "missing_dispatch_target", SitePC: &site, TargetPC: &target, Count: aggregate.Count,
		Message:    fmt.Sprintf("runtime could not dispatch to $%02X:%04X M%dX%d", byte(target>>16), uint16(target), key.M, key.X),
		Suggestion: fmt.Sprintf("func Observed_%02X_%04X_M%dX%d %04X entry_mx:%d,%d", byte(target>>16), uint16(target), key.M, key.X, uint16(target), key.M, key.X),
	}
	if aggregate.Hidden {
		finding.Evidence = append(finding.Evidence, "observed with S<$0200; legacy stderr tripwires could hide this miss")
	}
	if metadata == nil {
		return finding
	}
	keyText := fmt.Sprintf("%02X%04X", byte(target>>16), uint16(target))
	for _, directive := range metadata.CFG["indirect_dispatch"] {
		if strings.EqualFold(directive.Bank, fmt.Sprintf("%02X", byte(target>>16))) && strings.Contains(strings.ToUpper(directive.Text), fmt.Sprintf("RET:%04X", uint16(target))) {
			finding.Priority, finding.Kind, finding.Suggestion = 100, "active_dispatch_continuation", ""
			finding.Message = "do not register: target is the ret: continuation of an active dispatch construct"
			finding.Evidence = append(finding.Evidence, fmt.Sprintf("%s:%d: %s", directive.Bank, directive.Line, directive.Text))
			return finding
		}
	}
	want := fmt.Sprintf("_M%dX%d", key.M, key.X)
	if variants := metadata.Functions[keyText]; len(variants) != 0 {
		for _, variant := range variants {
			if variant == want {
				finding.Priority, finding.Kind, finding.Suggestion = 80, "stale_or_benign_dispatch_miss", ""
				finding.Message = "target variant is already generated; trace is stale or the miss is a benign continuation unwind"
				return finding
			}
		}
		finding.Kind = "missing_mx_variant"
		finding.Message = fmt.Sprintf("target is generated, but M%dX%d is absent", key.M, key.X)
		finding.Evidence = append(finding.Evidence, "generated variants: "+strings.Join(variants, ","))
	}
	if len(image) != 0 && len(metadata.Labels[keyText]) != 0 {
		if description, ok := pairedTraceReturn(image, target, metadata); ok {
			finding.Priority, finding.Kind, finding.Suggestion = 100, "paired_call_return", ""
			finding.Message = "do not register: target is the return PC of a generated paired call and would execute twice"
			finding.Evidence = append(finding.Evidence, description)
		}
	}
	return finding
}

func pairedTraceReturn(image romimage.Image, target uint32, metadata *GeneratedMetadata) (string, bool) {
	bank, address := byte(target>>16), uint16(target)
	checks := []struct {
		length uint16
		opcode byte
		kind   string
	}{{3, 0x20, "JSR"}, {3, 0xfc, "JSR (abs,X)"}, {4, 0x22, "JSL"}}
	for _, check := range checks {
		if address < 0x8000+check.length {
			continue
		}
		site := address - check.length
		bytes, err := image.Slice(bank, site, int(check.length))
		if err != nil || bytes[0] != check.opcode {
			continue
		}
		var callee uint32
		if check.opcode == 0x22 {
			callee = uint32(bytes[3])<<16 | uint32(bytes[2])<<8 | uint32(bytes[1])
		} else {
			callee = uint32(bank)<<16 | uint32(bytes[2])<<8 | uint32(bytes[1])
		}
		calleeKey := fmt.Sprintf("%02X%04X", byte(callee>>16), uint16(callee))
		if len(metadata.Functions[calleeKey]) == 0 && len(metadata.Labels[calleeKey]) == 0 {
			continue
		}
		return fmt.Sprintf("$%02X:%04X is %s return PC from $%02X:%04X to $%02X:%04X", bank, address, check.kind, bank, site, byte(callee>>16), uint16(callee)), true
	}
	return "", false
}

func WriteTraceInspection(output io.Writer, report TraceInspectionReport, format string) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(output)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		fmt.Fprintf(output, "trace inspect v%d: %d events, %d invalid line(s), seq %d..%d, hf %d..%d, gf %d..%d\n",
			report.Version, report.Summary.Events, report.Summary.InvalidLines,
			report.Summary.FirstSeq, report.Summary.LastSeq, report.Summary.FirstHF, report.Summary.LastHF,
			report.Summary.FirstGF, report.Summary.LastGF)
		channels := make([]string, 0, len(report.Summary.Channels))
		for channel := range report.Summary.Channels {
			channels = append(channels, channel)
		}
		sort.Strings(channels)
		for _, channel := range channels {
			fmt.Fprintf(output, "  %-10s %d\n", channel, report.Summary.Channels[channel])
		}
		fmt.Fprintf(output, "  entry-misdecodes=%d call-M/X-leaks=%d dispatch-misses=%d garbage-variants=%d\n",
			report.Summary.Misdecodes, report.Summary.Leaks, report.Summary.DispatchMiss, report.Summary.Garbage)
		for index, finding := range report.Findings {
			fmt.Fprintf(output, "\n  [%d] P%d %s x%d: %s\n", index+1, finding.Priority, finding.Kind, finding.Count, finding.Message)
			for _, evidence := range finding.Evidence {
				fmt.Fprintf(output, "      evidence: %s\n", evidence)
			}
			if finding.Suggestion != "" {
				fmt.Fprintf(output, "      candidate cfg: %s\n", finding.Suggestion)
			}
		}
		for _, event := range report.Events {
			fmt.Fprintln(output, formatTraceEvent(event))
		}
		if report.Truncated != 0 {
			fmt.Fprintf(output, "... (%d more; raise --limit)\n", report.Truncated)
		}
		return nil
	default:
		return fmt.Errorf("unknown trace inspection format %q (want text or json)", format)
	}
}

func formatTraceEvent(event map[string]any) string {
	base := fmt.Sprintf("seq=%7s hf=%s gf=%s %-8s fn=%s", traceString(event, "seq"), traceString(event, "hf"), traceString(event, "gf"), traceString(event, "ch"), traceString(event, "fn"))
	switch traceString(event, "ch") {
	case "func":
		return fmt.Sprintf("%s pc=%s m=%s x=%s expected=m%sx%s", base, traceString(event, "pc"), traceString(event, "m"), traceString(event, "x"), traceString(event, "em"), traceString(event, "ex"))
	case "call":
		return fmt.Sprintf("%s site=%s m=%s x=%s expected=m%sx%s", base, traceString(event, "site"), traceString(event, "m"), traceString(event, "x"), traceString(event, "em"), traceString(event, "ex"))
	case "vram":
		return fmt.Sprintf("%s va=$%s val=$%s path=%s", base, traceString(event, "va"), traceString(event, "val"), traceString(event, "path"))
	case "vmadd":
		return fmt.Sprintf("%s VMADD=$%s (%s)", base, traceString(event, "vmadd"), traceString(event, "how"))
	case "wram", "stack":
		return fmt.Sprintf("%s off=$%s %s->$%s", base, traceString(event, "off"), traceString(event, "old"), traceString(event, "val"))
	case "dispmiss":
		return fmt.Sprintf("%s DISPATCH-MISS %s -> %s", base, traceString(event, "from"), traceString(event, "to"))
	case "dispatch":
		return fmt.Sprintf("%s DISPATCH %s -> %s found=%s continuation=%s trapped=%s hits=%s", base, traceString(event, "site"), traceString(event, "target"), traceString(event, "found"), traceString(event, "continuation"), traceString(event, "trapped"), traceString(event, "hits"))
	default:
		return base
	}
}

func traceString(event map[string]any, key string) string {
	value, ok := event[key]
	if !ok || value == nil {
		return "?"
	}
	return fmt.Sprint(value)
}

func traceUint(event map[string]any, key string) (uint64, bool) {
	value, ok := event[key]
	if !ok {
		return 0, false
	}
	switch typed := value.(type) {
	case json.Number:
		parsed, err := strconv.ParseUint(string(typed), 10, 64)
		return parsed, err == nil
	case string:
		parsed, err := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(typed), "0x"), "$"), 10, 64)
		return parsed, err == nil
	case float64:
		return uint64(typed), true
	default:
		return 0, false
	}
}

func traceHex(event map[string]any, key string) (uint32, bool) {
	value, ok := event[key]
	if !ok {
		return 0, false
	}
	if number, ok := value.(json.Number); ok {
		parsed, err := strconv.ParseUint(string(number), 10, 32)
		return uint32(parsed), err == nil
	}
	text := strings.TrimSpace(fmt.Sprint(value))
	text = strings.TrimPrefix(strings.TrimPrefix(text, "0x"), "0X")
	text = strings.TrimPrefix(text, "$")
	text = strings.ReplaceAll(text, ":", "")
	parsed, err := strconv.ParseUint(text, 16, 32)
	return uint32(parsed), err == nil
}

func traceAddressField(event map[string]any, key string) (uint32, bool) {
	return traceHex(event, key)
}

func traceBool(event map[string]any, key string) bool {
	return traceBoolDefault(event, key, false)
}

func traceBoolDefault(event map[string]any, key string, fallback bool) bool {
	value, ok := event[key]
	if !ok {
		return fallback
	}
	switch typed := value.(type) {
	case bool:
		return typed
	case json.Number:
		return typed != "0"
	case string:
		return typed == "1" || strings.EqualFold(typed, "true")
	default:
		return fallback
	}
}

func optionalTraceAddress(value uint32) *uint32 {
	value &= 0xffffff
	return &value
}

func absoluteUintDifference(left, right uint64) uint64 {
	if left > right {
		return left - right
	}
	return right - left
}
