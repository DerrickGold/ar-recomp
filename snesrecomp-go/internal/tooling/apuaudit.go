package tooling

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"
)

const (
	apuAuditVersion       = 1
	apuRAMBytes           = 0x10000
	dspRegisterBytes      = 0x80
	apuWrittenBitmapBytes = apuRAMBytes / 8
	spcRAMOffset          = 0x100
	spcDSPOffset          = 0x10100
)

type APUAuditOptions struct {
	Prefix        string
	SPCPath       string
	ARAMPath      string
	DSPPath       string
	WrittenPath   string
	TracePath     string
	DirectoryPage *uint8
	Sources       []uint8
}

type APUAuditInput struct {
	Kind   string `json:"kind"`
	Path   string `json:"path"`
	SHA256 string `json:"sha256"`
	Bytes  int    `json:"bytes"`
}

type APUVoiceEvidence struct {
	Origin        string `json:"origin"`
	Voice         int    `json:"voice"`
	Source        uint8  `json:"source"`
	DirectoryPage uint8  `json:"directory_page"`
	StartAddress  uint16 `json:"start_address"`
	LoopAddress   uint16 `json:"loop_address"`
	VolumeLeft    int8   `json:"volume_left"`
	VolumeRight   int8   `json:"volume_right"`
	Envelope      uint8  `json:"envelope,omitempty"`
	Output        int8   `json:"output,omitempty"`
	Observations  uint64 `json:"observations"`
}

type APUSampleFinding struct {
	Severity string `json:"severity"`
	Reason   string `json:"reason"`
	Address  uint16 `json:"address,omitempty"`
	Detail   string `json:"detail"`
}

type APUSampleAudit struct {
	Source         uint8              `json:"source"`
	DirectoryPage  uint8              `json:"directory_page"`
	StartAddress   uint16             `json:"start_address"`
	LoopAddress    uint16             `json:"loop_address"`
	BoundExclusive uint32             `json:"bound_exclusive"`
	BoundReason    string             `json:"bound_reason"`
	EndAddress     *uint16            `json:"end_address,omitempty"`
	BlockCount     int                `json:"block_count"`
	Looping        bool               `json:"looping"`
	WriteCoverage  string             `json:"write_coverage"`
	Status         string             `json:"status"`
	Evidence       []APUVoiceEvidence `json:"evidence"`
	Findings       []APUSampleFinding `json:"findings,omitempty"`
}

type APUPortOverwrite struct {
	Port             uint8  `json:"port"`
	PreviousValue    uint8  `json:"previous_value"`
	ReplacementValue uint8  `json:"replacement_value"`
	FirstFrame       uint32 `json:"first_frame"`
	LastFrame        uint32 `json:"last_frame"`
	SourceBlock      uint32 `json:"source_block,omitempty"`
	Function         string `json:"function,omitempty"`
	Hits             uint64 `json:"hits"`
}

type APUPortAudit struct {
	CPUWrites             uint64             `json:"cpu_writes"`
	AppliedWrites         uint64             `json:"applied_writes"`
	SPCReads              uint64             `json:"spc_reads"`
	RetainedCPUWrites     uint64             `json:"retained_cpu_write_events"`
	RetainedAppliedWrites uint64             `json:"retained_applied_write_events"`
	RetainedSPCReads      uint64             `json:"retained_spc_read_events"`
	SameValueRewrites     [4]uint64          `json:"same_value_rewrites"`
	TraceOverflow         bool               `json:"trace_overflow"`
	Overwrites            []APUPortOverwrite `json:"overwrites,omitempty"`
	UnobservedPorts       []uint8            `json:"unobserved_ports,omitempty"`
}

type APUAuditSummary struct {
	SourcesAudited     int `json:"sources_audited"`
	ValidSamples       int `json:"valid_samples"`
	InvalidSamples     int `json:"invalid_samples"`
	Inconclusive       int `json:"inconclusive"`
	ActiveVoices       int `json:"active_voices"`
	ObservedKeyOns     int `json:"observed_key_ons"`
	PortOverwritePairs int `json:"port_overwrite_pairs"`
}

type APUAuditReport struct {
	Version       int                `json:"version"`
	Mode          string             `json:"mode"`
	NoWrite       bool               `json:"no_write"`
	Inputs        []APUAuditInput    `json:"inputs"`
	DirectoryPage uint8              `json:"directory_page"`
	CoverageKnown bool               `json:"coverage_known"`
	SilentCapture bool               `json:"silent_capture"`
	Evidence      []APUVoiceEvidence `json:"evidence,omitempty"`
	Samples       []APUSampleAudit   `json:"samples"`
	Ports         APUPortAudit       `json:"ports"`
	Summary       APUAuditSummary    `json:"summary"`
	Limitations   []string           `json:"limitations,omitempty"`
}

type apuTraceLine struct {
	Format                   string    `json:"format"`
	Version                  int       `json:"version"`
	Kind                     string    `json:"kind"`
	Type                     string    `json:"type"`
	EventCount               uint64    `json:"event_count"`
	OldestEvent              uint64    `json:"oldest_event"`
	TraceOverflow            bool      `json:"trace_overflow"`
	CPUWrites                uint64    `json:"cpu_port_writes"`
	APUPortApplies           uint64    `json:"apu_port_applies"`
	SPCPortReads             uint64    `json:"spc_port_reads"`
	CPUPortOverwrites        [4]uint64 `json:"cpu_port_overwrites"`
	CPUPortSameValueRewrites [4]uint64 `json:"cpu_port_same_value_rewrites"`
	Index                    uint64    `json:"index"`
	Sample                   uint64    `json:"sample"`
	Frame                    uint32    `json:"frame"`
	Address                  uint8     `json:"address"`
	Value                    uint8     `json:"value"`
	SourceBlock              uint32    `json:"source_block"`
	Function                 string    `json:"function"`
}

type apuQueuedWrite struct {
	value       uint8
	frame       uint32
	sourceBlock uint32
	function    string
}

type apuPendingWrite struct {
	valid bool
	apuQueuedWrite
}

func popAPUQueuedWrite(queue []apuQueuedWrite, value uint8) (apuQueuedWrite, []apuQueuedWrite) {
	for index, candidate := range queue {
		if candidate.value != value {
			continue
		}
		return candidate, queue[index+1:]
	}
	if len(queue) != 0 {
		candidate := queue[0]
		candidate.value = value
		return candidate, queue[1:]
	}
	return apuQueuedWrite{value: value}, queue
}

func BuildAPUAudit(options APUAuditOptions) (APUAuditReport, error) {
	resolveAPUAuditPrefix(&options)
	report := APUAuditReport{
		Version: apuAuditVersion, Mode: "apu_sample_and_port_integrity",
		NoWrite: true,
	}
	aram, dsp, inputs, err := loadAPUAuditMemory(options)
	if err != nil {
		return APUAuditReport{}, err
	}
	report.Inputs = append(report.Inputs, inputs...)
	var written []byte
	if strings.TrimSpace(options.WrittenPath) != "" {
		written, err = os.ReadFile(options.WrittenPath)
		if err != nil {
			if !errors.Is(err, os.ErrNotExist) {
				return APUAuditReport{}, fmt.Errorf("read APU write bitmap: %w", err)
			}
		} else {
			if len(written) != apuWrittenBitmapBytes {
				return APUAuditReport{}, fmt.Errorf("APU write bitmap must be %d bytes, got %d", apuWrittenBitmapBytes, len(written))
			}
			report.CoverageKnown = true
			report.Inputs = append(report.Inputs, describeAPUAuditInput("write_bitmap", options.WrittenPath, written))
		}
	}
	directoryPage := uint8(0)
	if len(dsp) == dspRegisterBytes {
		directoryPage = dsp[0x5d]
	}
	if options.DirectoryPage != nil {
		directoryPage = *options.DirectoryPage
	}
	report.DirectoryPage = directoryPage

	traceEvidence, ports, traceInput, traceLimitations, err := loadAPUTrace(options.TracePath, aram)
	if err != nil {
		return APUAuditReport{}, err
	}
	if traceInput != nil {
		report.Inputs = append(report.Inputs, *traceInput)
	}
	report.Ports = ports
	report.Limitations = append(report.Limitations, traceLimitations...)
	if len(traceEvidence) != 0 {
		report.Limitations = append(report.Limitations,
			"historical key-ons are checked against the final ARAM snapshot; capture near a failure when a game replaces sample banks at runtime")
	}

	if len(dsp) == dspRegisterBytes {
		report.Evidence = append(report.Evidence, activeDSPVoices(dsp, aram, directoryPage)...)
	}
	report.Summary.ActiveVoices = len(report.Evidence)
	report.Summary.ObservedKeyOns = len(traceEvidence)
	report.Evidence = append(report.Evidence, traceEvidence...)
	for _, source := range options.Sources {
		start, loop := sampleDirectoryEntry(aram, directoryPage, source)
		report.Evidence = append(report.Evidence, APUVoiceEvidence{
			Origin: "explicit", Voice: -1, Source: source, DirectoryPage: directoryPage,
			StartAddress: start, LoopAddress: loop, Observations: 1,
		})
	}
	report.Evidence = mergeAPUVoiceEvidence(report.Evidence)
	report.SilentCapture = len(report.Evidence) == 0
	if report.SilentCapture {
		report.Limitations = append(report.Limitations,
			"no active voice, observed key-on, or explicit source was available; stale DSP source registers are not treated as evidence")
	}

	type sampleKey struct {
		page   uint8
		source uint8
	}
	grouped := make(map[sampleKey][]APUVoiceEvidence)
	for _, evidence := range report.Evidence {
		key := sampleKey{page: evidence.DirectoryPage, source: evidence.Source}
		grouped[key] = append(grouped[key], evidence)
	}
	keys := make([]sampleKey, 0, len(grouped))
	for key := range grouped {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].page != keys[j].page {
			return keys[i].page < keys[j].page
		}
		return keys[i].source < keys[j].source
	})
	for _, key := range keys {
		sample := auditAPUSample(aram, written, report.CoverageKnown, key.page, key.source, grouped[key])
		report.Samples = append(report.Samples, sample)
		switch sample.Status {
		case "valid":
			report.Summary.ValidSamples++
		case "invalid":
			report.Summary.InvalidSamples++
		default:
			report.Summary.Inconclusive++
		}
	}
	report.Summary.SourcesAudited = len(report.Samples)
	report.Summary.PortOverwritePairs = len(report.Ports.Overwrites)
	return report, nil
}

func resolveAPUAuditPrefix(options *APUAuditOptions) {
	if options == nil || strings.TrimSpace(options.Prefix) == "" {
		return
	}
	if options.ARAMPath == "" {
		options.ARAMPath = options.Prefix + ".aram"
	}
	if options.DSPPath == "" {
		options.DSPPath = options.Prefix + ".dsp"
	}
	if options.WrittenPath == "" {
		options.WrittenPath = options.Prefix + ".written"
	}
	if options.TracePath == "" {
		options.TracePath = options.Prefix + ".audio.jsonl"
	}
}

func loadAPUAuditMemory(options APUAuditOptions) ([]byte, []byte, []APUAuditInput, error) {
	if strings.TrimSpace(options.SPCPath) != "" {
		data, err := os.ReadFile(options.SPCPath)
		if err != nil {
			return nil, nil, nil, fmt.Errorf("read SPC snapshot: %w", err)
		}
		if len(data) < spcDSPOffset+dspRegisterBytes || !strings.HasPrefix(string(data), "SNES-SPC700 Sound File Data") {
			return nil, nil, nil, fmt.Errorf("%s is not a complete SPC snapshot", options.SPCPath)
		}
		aram := append([]byte(nil), data[spcRAMOffset:spcRAMOffset+apuRAMBytes]...)
		dsp := append([]byte(nil), data[spcDSPOffset:spcDSPOffset+dspRegisterBytes]...)
		return aram, dsp, []APUAuditInput{describeAPUAuditInput("spc_snapshot", options.SPCPath, data)}, nil
	}
	if strings.TrimSpace(options.ARAMPath) == "" {
		return nil, nil, nil, errors.New("apu-audit requires --prefix, --spc, or --aram")
	}
	aram, err := os.ReadFile(options.ARAMPath)
	if err != nil {
		return nil, nil, nil, fmt.Errorf("read ARAM snapshot: %w", err)
	}
	if len(aram) != apuRAMBytes {
		return nil, nil, nil, fmt.Errorf("ARAM snapshot must be %d bytes, got %d", apuRAMBytes, len(aram))
	}
	inputs := []APUAuditInput{describeAPUAuditInput("aram", options.ARAMPath, aram)}
	var dsp []byte
	if strings.TrimSpace(options.DSPPath) != "" {
		dsp, err = os.ReadFile(options.DSPPath)
		if err != nil {
			if !errors.Is(err, os.ErrNotExist) {
				return nil, nil, nil, fmt.Errorf("read DSP snapshot: %w", err)
			}
		} else {
			if len(dsp) != dspRegisterBytes {
				return nil, nil, nil, fmt.Errorf("DSP snapshot must be %d bytes, got %d", dspRegisterBytes, len(dsp))
			}
			inputs = append(inputs, describeAPUAuditInput("dsp", options.DSPPath, dsp))
		}
	}
	return aram, dsp, inputs, nil
}

func describeAPUAuditInput(kind, path string, data []byte) APUAuditInput {
	hash := sha256.Sum256(data)
	return APUAuditInput{Kind: kind, Path: path, SHA256: hex.EncodeToString(hash[:]), Bytes: len(data)}
}

func sampleDirectoryEntry(aram []byte, page, source uint8) (uint16, uint16) {
	base := uint16(page)<<8 | uint16(source)<<2
	start := uint16(aram[base]) | uint16(aram[uint16(base+1)])<<8
	loop := uint16(aram[uint16(base+2)]) | uint16(aram[uint16(base+3)])<<8
	return start, loop
}

func activeDSPVoices(dsp, aram []byte, directoryPage uint8) []APUVoiceEvidence {
	var evidence []APUVoiceEvidence
	noise := dsp[0x3d]
	for voice := 0; voice < 8; voice++ {
		base := voice * 0x10
		left, right := int8(dsp[base]), int8(dsp[base+1])
		envelope, output := dsp[base+8], int8(dsp[base+9])
		if noise&(1<<voice) != 0 || left == 0 && right == 0 || envelope == 0 && output == 0 {
			continue
		}
		source := dsp[base+4]
		start, loop := sampleDirectoryEntry(aram, directoryPage, source)
		evidence = append(evidence, APUVoiceEvidence{
			Origin: "active_snapshot", Voice: voice, Source: source,
			DirectoryPage: directoryPage, StartAddress: start, LoopAddress: loop,
			VolumeLeft: left, VolumeRight: right, Envelope: envelope, Output: output,
			Observations: 1,
		})
	}
	return evidence
}

func mergeAPUVoiceEvidence(values []APUVoiceEvidence) []APUVoiceEvidence {
	type key struct {
		origin string
		voice  int
		source uint8
		page   uint8
		start  uint16
		loop   uint16
	}
	merged := make(map[key]APUVoiceEvidence)
	for _, value := range values {
		itemKey := key{value.Origin, value.Voice, value.Source, value.DirectoryPage, value.StartAddress, value.LoopAddress}
		if existing, ok := merged[itemKey]; ok {
			existing.Observations += value.Observations
			merged[itemKey] = existing
		} else {
			merged[itemKey] = value
		}
	}
	result := make([]APUVoiceEvidence, 0, len(merged))
	for _, value := range merged {
		result = append(result, value)
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].Source != result[j].Source {
			return result[i].Source < result[j].Source
		}
		if result[i].Origin != result[j].Origin {
			return result[i].Origin < result[j].Origin
		}
		return result[i].Voice < result[j].Voice
	})
	return result
}

func auditAPUSample(aram, written []byte, coverageKnown bool, page, source uint8, evidence []APUVoiceEvidence) APUSampleAudit {
	start, loop := sampleDirectoryEntry(aram, page, source)
	result := APUSampleAudit{
		Source: source, DirectoryPage: page, StartAddress: start, LoopAddress: loop,
		BoundExclusive: apuRAMBytes, BoundReason: "16_bit_aram_address_cycle", Status: "inconclusive",
		WriteCoverage: "unknown", Evidence: evidence,
	}
	allWritten := coverageKnown
	address := start
	visited := make([]bool, apuRAMBytes)
	for !visited[address] {
		visited[address] = true
		result.BlockCount++
		if coverageKnown && !apuRangeWritten(written, address, 9) {
			allWritten = false
			if len(result.Findings) == 0 || result.Findings[len(result.Findings)-1].Reason != "unwritten_brr" {
				result.Findings = append(result.Findings, APUSampleFinding{
					Severity: "error", Reason: "unwritten_brr", Address: address,
					Detail: "BRR block includes bytes not covered by an observed SPC or HLE upload write",
				})
			}
		}
		header := aram[address]
		if header&1 != 0 {
			end := uint16(uint32(address) + 8)
			result.EndAddress = &end
			result.Looping = header&2 != 0
			break
		}
		address = uint16(uint32(address) + 9)
	}
	if coverageKnown {
		if allWritten {
			result.WriteCoverage = "complete"
			result.Status = "valid"
		} else {
			result.WriteCoverage = "incomplete"
			result.Status = "invalid"
		}
	}
	if result.EndAddress == nil {
		result.Status = "invalid"
		result.Findings = append(result.Findings, APUSampleFinding{
			Severity: "error", Reason: "missing_end_before_address_cycle", Address: address,
			Detail: fmt.Sprintf("BRR walk returned to $%04X without finding an end block in the 16-bit ARAM address cycle", address),
		})
		return result
	}
	if result.Looping {
		endBlock := uint16(uint32(*result.EndAddress) - 8)
		if !visited[loop] {
			result.Status = "invalid"
			result.Findings = append(result.Findings, APUSampleFinding{
				Severity: "error", Reason: "invalid_loop_address", Address: loop,
				Detail: fmt.Sprintf("loop $%04X is not a BRR-block boundary visited between start $%04X and end block $%04X", loop, start, endBlock),
			})
		}
	}
	return result
}

func apuRangeWritten(bitmap []byte, address uint16, count int) bool {
	if len(bitmap) != apuWrittenBitmapBytes {
		return false
	}
	for index := 0; index < count; index++ {
		value := uint16(uint32(address) + uint32(index))
		if bitmap[value>>3]&(1<<(value&7)) == 0 {
			return false
		}
	}
	return true
}

func loadAPUTrace(path string, aram []byte) ([]APUVoiceEvidence, APUPortAudit, *APUAuditInput, []string, error) {
	var ports APUPortAudit
	if strings.TrimSpace(path) == "" {
		return nil, ports, nil, []string{"no audio event trace was supplied; historical key-ons and CPU-to-APU handshakes were not audited"}, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, ports, nil, []string{"audio event trace is absent; historical key-ons and CPU-to-APU handshakes were not audited"}, nil
		}
		return nil, ports, nil, nil, fmt.Errorf("read audio trace: %w", err)
	}
	input := describeAPUAuditInput("audio_trace", path, data)
	var dsp [dspRegisterBytes]byte
	var evidence []APUVoiceEvidence
	var queued [4][]apuQueuedWrite
	var pending [4]apuPendingWrite
	var retainedSameValueRewrites [4]uint64
	headerSeen := false
	type overwriteKey struct{ port, previous, replacement uint8 }
	overwrites := make(map[overwriteKey]APUPortOverwrite)
	readPorts := make(map[uint8]bool)
	writtenPorts := make(map[uint8]bool)
	scanner := bufio.NewScanner(strings.NewReader(string(data)))
	scanner.Buffer(make([]byte, 4096), 1024*1024)
	lineNumber := 0
	for scanner.Scan() {
		lineNumber++
		var line apuTraceLine
		if err := json.Unmarshal(scanner.Bytes(), &line); err != nil {
			return nil, ports, nil, nil, fmt.Errorf("parse audio trace line %d: %w", lineNumber, err)
		}
		if line.Format == "snesrecomp-audio-trace" {
			headerSeen = true
			ports.TraceOverflow = line.TraceOverflow || line.OldestEvent != 0
			ports.CPUWrites = line.CPUWrites
			ports.AppliedWrites = line.APUPortApplies
			ports.SPCReads = line.SPCPortReads
			ports.SameValueRewrites = line.CPUPortSameValueRewrites
			continue
		}
		if line.Kind != "event" {
			continue
		}
		switch line.Type {
		case "dsp_write":
			address := line.Address & 0x7f
			dsp[address] = line.Value
			if address == 0x4c {
				page := dsp[0x5d]
				for voice := 0; voice < 8; voice++ {
					if line.Value&(1<<voice) == 0 {
						continue
					}
					base := voice * 0x10
					source := dsp[base+4]
					start, loop := sampleDirectoryEntry(aram, page, source)
					evidence = append(evidence, APUVoiceEvidence{
						Origin: "observed_key_on", Voice: voice, Source: source,
						DirectoryPage: page, StartAddress: start, LoopAddress: loop,
						VolumeLeft: int8(dsp[base]), VolumeRight: int8(dsp[base+1]),
						Observations: 1,
					})
				}
			}
		case "cpu_port_write":
			port := line.Address & 3
			ports.RetainedCPUWrites++
			writtenPorts[port] = true
			queued[port] = append(queued[port], apuQueuedWrite{
				value: line.Value, frame: line.Frame, sourceBlock: line.SourceBlock, function: line.Function,
			})
		case "apu_port_apply":
			port := line.Address & 3
			ports.RetainedAppliedWrites++
			write, remaining := popAPUQueuedWrite(queued[port], line.Value)
			queued[port] = remaining
			write.value = line.Value
			if line.SourceBlock != 0 || line.Function != "" {
				write.sourceBlock = line.SourceBlock
				write.function = line.Function
			}
			if pending[port].valid {
				if pending[port].value == line.Value {
					retainedSameValueRewrites[port]++
				} else {
					key := overwriteKey{port, pending[port].value, line.Value}
					item := overwrites[key]
					if item.Hits == 0 {
						item = APUPortOverwrite{
							Port: port, PreviousValue: pending[port].value, ReplacementValue: line.Value,
							FirstFrame: pending[port].frame, SourceBlock: write.sourceBlock, Function: write.function,
						}
					}
					item.LastFrame = line.Frame
					item.Hits++
					overwrites[key] = item
				}
			}
			pending[port] = apuPendingWrite{valid: true, apuQueuedWrite: write}
		case "spc_port_read":
			port := line.Address & 3
			ports.RetainedSPCReads++
			readPorts[port] = true
			pending[port].valid = false
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, ports, nil, nil, fmt.Errorf("scan audio trace: %w", err)
	}
	if !headerSeen {
		ports.CPUWrites = ports.RetainedCPUWrites
		ports.AppliedWrites = ports.RetainedAppliedWrites
		ports.SPCReads = ports.RetainedSPCReads
		ports.SameValueRewrites = retainedSameValueRewrites
	} else if ports.SameValueRewrites == [4]uint64{} && retainedSameValueRewrites != [4]uint64{} {
		// Version-1 headers did not include this split; retained events are the
		// best available evidence when reading those captures.
		ports.SameValueRewrites = retainedSameValueRewrites
	}
	for _, item := range overwrites {
		ports.Overwrites = append(ports.Overwrites, item)
	}
	sort.Slice(ports.Overwrites, func(i, j int) bool {
		if ports.Overwrites[i].Hits != ports.Overwrites[j].Hits {
			return ports.Overwrites[i].Hits > ports.Overwrites[j].Hits
		}
		if ports.Overwrites[i].Port != ports.Overwrites[j].Port {
			return ports.Overwrites[i].Port < ports.Overwrites[j].Port
		}
		return ports.Overwrites[i].PreviousValue < ports.Overwrites[j].PreviousValue
	})
	for port := uint8(0); port < 4; port++ {
		if writtenPorts[port] && !readPorts[port] {
			ports.UnobservedPorts = append(ports.UnobservedPorts, port)
		}
	}
	var limitations []string
	if ports.TraceOverflow {
		limitations = append(limitations, "the audio event ring overflowed; findings cover only retained events")
	}
	return mergeAPUVoiceEvidence(evidence), ports, &input, limitations, nil
}

func WriteAPUAudit(output io.Writer, report APUAuditReport, format string) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(output)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		fmt.Fprintf(output, "APU audit v%d: %d source(s), %d valid, %d invalid, %d inconclusive\n",
			report.Version, report.Summary.SourcesAudited, report.Summary.ValidSamples,
			report.Summary.InvalidSamples, report.Summary.Inconclusive)
		fmt.Fprintf(output, "evidence: %d active snapshot voice(s), %d observed key-on voice/source record(s), coverage=%t, silent=%t\n",
			report.Summary.ActiveVoices, report.Summary.ObservedKeyOns, report.CoverageKnown, report.SilentCapture)
		for _, sample := range report.Samples {
			end := "none"
			if sample.EndAddress != nil {
				end = fmt.Sprintf("$%04X", *sample.EndAddress)
			}
			fmt.Fprintf(output, "[%s] SRCN $%02X start=$%04X loop=$%04X end=%s blocks=%d scan_limit=$%X(%s) writes=%s\n",
				strings.ToUpper(sample.Status), sample.Source, sample.StartAddress, sample.LoopAddress,
				end, sample.BlockCount, sample.BoundExclusive, sample.BoundReason, sample.WriteCoverage)
			for _, finding := range sample.Findings {
				fmt.Fprintf(output, "  - %s: %s\n", finding.Reason, finding.Detail)
			}
		}
		sameValueRewrites := report.Ports.SameValueRewrites[0] + report.Ports.SameValueRewrites[1] +
			report.Ports.SameValueRewrites[2] + report.Ports.SameValueRewrites[3]
		fmt.Fprintf(output, "ports: cpu_writes=%d applied=%d spc_reads=%d retained_read_events=%d overwrite_pairs=%d same_value_rewrites=%d overflow=%t\n",
			report.Ports.CPUWrites, report.Ports.AppliedWrites, report.Ports.SPCReads,
			report.Ports.RetainedSPCReads, len(report.Ports.Overwrites), sameValueRewrites, report.Ports.TraceOverflow)
		for _, overwrite := range report.Ports.Overwrites {
			fmt.Fprintf(output, "[PORT-OVERWRITE] port=%d $%02X->$%02X hits=%d frames=%d..%d block=$%06X fn=%s\n",
				overwrite.Port, overwrite.PreviousValue, overwrite.ReplacementValue,
				overwrite.Hits, overwrite.FirstFrame, overwrite.LastFrame,
				overwrite.SourceBlock, overwrite.Function)
		}
		if len(report.Ports.UnobservedPorts) != 0 {
			fmt.Fprintf(output, "warning: CPU wrote APU port(s) %v but the retained trace contains no SPC read\n", report.Ports.UnobservedPorts)
		}
		for _, limitation := range report.Limitations {
			fmt.Fprintf(output, "note: %s\n", limitation)
		}
		return nil
	default:
		return fmt.Errorf("unknown APU audit format %q (want text or json)", format)
	}
}

func ParseAPUSourceList(value string) ([]uint8, error) {
	if strings.TrimSpace(value) == "" {
		return nil, nil
	}
	seen := make(map[uint8]bool)
	var sources []uint8
	for _, token := range strings.Split(value, ",") {
		text := strings.TrimSpace(token)
		text = strings.TrimPrefix(strings.TrimPrefix(text, "0x"), "0X")
		text = strings.TrimPrefix(text, "$")
		parsed, err := strconv.ParseUint(text, 16, 8)
		if err != nil {
			return nil, fmt.Errorf("parse APU source %q: %w", token, err)
		}
		source := uint8(parsed)
		if !seen[source] {
			seen[source] = true
			sources = append(sources, source)
		}
	}
	sort.Slice(sources, func(i, j int) bool { return sources[i] < sources[j] })
	return sources, nil
}
