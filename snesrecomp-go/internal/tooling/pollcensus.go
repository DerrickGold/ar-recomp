package tooling

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const pollCensusVersion = 3

type PollHLECoverage string

const (
	PollHLECoverageNone        PollHLECoverage = "none"
	PollHLECoverageConditional PollHLECoverage = "conditional"
	PollHLECoverageWholeBody   PollHLECoverage = "whole_body"
)

type PollCensusOptions struct {
	ROMPath, CFGDir       string
	Jobs                  int
	OnlyBank              *byte
	Registers             []uint16
	DiscoverInterruptSync bool
}

type PollSite struct {
	PC               uint32           `json:"pc"`
	Register         uint16           `json:"register"`
	InstructionBytes string           `json:"instruction_bytes"`
	Mnemonic         string           `json:"mnemonic"`
	AddressingMode   string           `json:"addressing_mode"`
	Resolution       string           `json:"resolution"`
	Classification   string           `json:"classification"`
	BranchPC         *uint32          `json:"branch_pc,omitempty"`
	BranchTarget     *uint32          `json:"branch_target,omitempty"`
	LiveMX           analysis.MXState `json:"live_mx"`
	FunctionEntries  []uint32         `json:"function_entries"`
	HLECoverage      PollHLECoverage  `json:"hle_coverage"`
	Reachability     string           `json:"reachability"`
	Selection        string           `json:"selection"`
}

type PollCensusSummary struct {
	Sites                  int `json:"sites"`
	PollLoops              int `json:"poll_loops"`
	LivePollLoops          int `json:"live_poll_loops"`
	ClearReads             int `json:"clear_reads"`
	PostReads              int `json:"post_reads"`
	SingleReads            int `json:"single_reads"`
	HLEConditional         int `json:"hle_conditional"`
	HLEWholeBody           int `json:"hle_whole_body"`
	InitialVariants        int `json:"initial_variants"`
	FinalVariants          int `json:"final_variants"`
	VariantPasses          int `json:"variant_passes"`
	DecodeIssues           int `json:"decode_issues"`
	InterruptSyncAddresses int `json:"interrupt_sync_addresses"`
}

type PollInterruptSync struct {
	Address          uint16   `json:"address"`
	InterruptEntries []uint32 `json:"interrupt_entries"`
	WriterPCs        []uint32 `json:"writer_pcs"`
}

type PollCensusReport struct {
	Version       int                 `json:"version"`
	Mode          string              `json:"mode"`
	NoWrite       bool                `json:"no_write"`
	ROM           ShadowROM           `json:"rom"`
	Registers     []uint16            `json:"registers"`
	InterruptSync []PollInterruptSync `json:"interrupt_sync,omitempty"`
	Summary       PollCensusSummary   `json:"summary"`
	Sites         []PollSite          `json:"sites"`
	DecodeIssues  []ShadowDecodeIssue `json:"decode_issues,omitempty"`
	Limitations   []string            `json:"limitations"`
}

type pollSiteKey struct {
	PC       uint32
	Register uint16
	M, X     uint8
}

type pollSiteAccumulator struct {
	Site   PollSite
	Owners map[uint32]struct{}
}

func BuildPollCensus(options PollCensusOptions) (PollCensusReport, error) {
	if options.Jobs <= 0 {
		options.Jobs = 1
	}
	if len(options.Registers) == 0 {
		options.Registers = []uint16{0x4210, 0x4212}
	}
	registers := make(map[uint16]struct{}, len(options.Registers))
	selection := make(map[uint16]string, len(options.Registers))
	for _, register := range options.Registers {
		registers[register] = struct{}{}
		selection[register] = "explicit"
	}
	image, err := romimage.Load(options.ROMPath)
	if err != nil {
		return PollCensusReport{}, err
	}
	banks, err := loadShadowBanks(options.CFGDir, options.OnlyBank)
	if err != nil {
		return PollCensusReport{}, err
	}
	regions := collectShadowDataRegions(banks)
	results, stats, err := discoverShadowDecodeResults(image, banks, regions, collectShadowExitMX(banks), options.Jobs)
	if err != nil {
		return PollCensusReport{}, err
	}
	var interruptSync []PollInterruptSync
	if options.DiscoverInterruptSync {
		interruptSync = discoverPollInterruptSync(image, results)
		for _, candidate := range interruptSync {
			registers[candidate.Address] = struct{}{}
			if selection[candidate.Address] == "" {
				selection[candidate.Address] = "interrupt_write"
			}
		}
	}
	hleCoverage := make(map[uint32]PollHLECoverage)
	for _, bank := range banks {
		for pc := range bank.Config.HLEFunctions {
			hleCoverage[uint32(bank.ID)<<16|uint32(pc)] =
				PollHLECoverageWholeBody
		}
		for pc := range bank.Config.HLEFunctionsIf {
			hleCoverage[uint32(bank.ID)<<16|uint32(pc)] =
				PollHLECoverageConditional
		}
		for _, pc := range bank.Config.HLESPCUpload {
			hleCoverage[uint32(bank.ID)<<16|uint32(pc)] =
				PollHLECoverageWholeBody
		}
	}
	accumulators := make(map[pollSiteKey]*pollSiteAccumulator)
	var issues []ShadowDecodeIssue
	for _, result := range results {
		if result.issue != nil {
			issues = append(issues, *result.issue)
			continue
		}
		classifyPollResult(result, registers, selection, accumulators)
	}
	var sites []PollSite
	for _, accumulator := range accumulators {
		allWholeBody := len(accumulator.Owners) != 0
		anyCovered := false
		for owner := range accumulator.Owners {
			accumulator.Site.FunctionEntries = append(accumulator.Site.FunctionEntries, owner)
			coverage := hleCoverage[owner]
			if coverage != PollHLECoverageWholeBody {
				allWholeBody = false
			}
			if coverage != "" && coverage != PollHLECoverageNone {
				anyCovered = true
			}
		}
		switch {
		case allWholeBody:
			accumulator.Site.HLECoverage = PollHLECoverageWholeBody
		case anyCovered:
			accumulator.Site.HLECoverage = PollHLECoverageConditional
		default:
			accumulator.Site.HLECoverage = PollHLECoverageNone
		}
		sort.Slice(accumulator.Site.FunctionEntries, func(i, j int) bool { return accumulator.Site.FunctionEntries[i] < accumulator.Site.FunctionEntries[j] })
		sites = append(sites, accumulator.Site)
	}
	sort.Slice(sites, func(i, j int) bool {
		if sites[i].PC != sites[j].PC {
			return sites[i].PC < sites[j].PC
		}
		if sites[i].LiveMX.M != sites[j].LiveMX.M {
			return sites[i].LiveMX.M < sites[j].LiveMX.M
		}
		return sites[i].LiveMX.X < sites[j].LiveMX.X
	})
	sort.Slice(issues, func(i, j int) bool { return issues[i].FunctionEntry < issues[j].FunctionEntry })
	hash := sha256.Sum256(image)
	selectedRegisters := make([]uint16, 0, len(registers))
	for register := range registers {
		selectedRegisters = append(selectedRegisters, register)
	}
	sort.Slice(selectedRegisters, func(i, j int) bool { return selectedRegisters[i] < selectedRegisters[j] })
	report := PollCensusReport{
		Version: pollCensusVersion, Mode: "decoded_hardware_poll_census", NoWrite: true,
		ROM:       ShadowROM{SHA256: hex.EncodeToString(hash[:]), Size: len(image), Mapper: "lorom"},
		Registers: selectedRegisters, InterruptSync: interruptSync, Sites: sites, DecodeIssues: issues,
		Summary: PollCensusSummary{Sites: len(sites), InitialVariants: stats.initialVariants, FinalVariants: stats.finalVariants, VariantPasses: stats.passes, DecodeIssues: len(issues), InterruptSyncAddresses: len(interruptSync)},
		Limitations: []string{
			"sites are limited to configuration- or static-call-rooted decoded instruction boundaries",
			"absolute hardware operands assume DB resolves to bank zero; long operands explicitly prove bank zero",
			"poll-loop classification is structural evidence and does not by itself authorize scheduler lowering",
			"interrupt-sync discovery includes only low WRAM mirror addresses both written by decoded NMI/IRQ ownership and read by decoded code",
		},
	}
	populatePollInstructionBytes(image, &report)
	for _, site := range sites {
		switch site.Classification {
		case "poll_loop":
			report.Summary.PollLoops++
			if site.HLECoverage != PollHLECoverageWholeBody {
				report.Summary.LivePollLoops++
			}
		case "clear_read":
			report.Summary.ClearReads++
		case "post_poll_read":
			report.Summary.PostReads++
		default:
			report.Summary.SingleReads++
		}
		switch site.HLECoverage {
		case PollHLECoverageConditional:
			report.Summary.HLEConditional++
		case PollHLECoverageWholeBody:
			report.Summary.HLEWholeBody++
		}
	}
	return report, nil
}

func discoverPollInterruptSync(image romimage.Image, results []shadowDecodeResult) []PollInterruptSync {
	interruptRoots := make(map[uint32]struct{})
	if len(image) >= 0x8000 {
		for _, vector := range parseROMVectors(image, 0x7fc0) {
			if !strings.HasSuffix(vector.Name, "_nmi") &&
				!strings.HasSuffix(vector.Name, "_irq") {
				continue
			}
			interruptRoots[decoderAddress24(0, vector.Address)] = struct{}{}
		}
	}
	instructionsByEntry := make(map[uint32][]shadowDecodedInstruction)
	entries := make(map[uint32]struct{})
	for _, result := range results {
		for _, decoded := range result.instructions {
			entry := decoded.FunctionEntry & 0xffffff
			entries[entry] = struct{}{}
			instructionsByEntry[entry] = append(instructionsByEntry[entry], decoded)
		}
	}
	owned := make(map[uint32]map[uint32]struct{})
	queue := make([]uint32, 0, len(interruptRoots))
	for root := range interruptRoots {
		if _, decoded := entries[root]; !decoded {
			continue
		}
		owned[root] = map[uint32]struct{}{root: {}}
		queue = append(queue, root)
	}
	for len(queue) > 0 {
		entry := queue[0]
		queue = queue[1:]
		for _, decoded := range instructionsByEntry[entry] {
			target, ok := pollDirectCallOrTailTarget(decoded.Instruction)
			if !ok {
				continue
			}
			if target <= 0xffff {
				target = uint32(byte(decoded.PC>>16))<<16 | uint32(uint16(target))
			}
			target &= 0xffffff
			if _, decodedTarget := entries[target]; !decodedTarget {
				continue
			}
			roots := owned[target]
			if roots == nil {
				roots = make(map[uint32]struct{})
				owned[target] = roots
			}
			changed := false
			for root := range owned[entry] {
				if _, found := roots[root]; !found {
					roots[root] = struct{}{}
					changed = true
				}
			}
			if changed {
				queue = append(queue, target)
			}
		}
	}
	type candidate struct {
		roots   map[uint32]struct{}
		writers map[uint32]struct{}
	}
	candidates := make(map[uint16]*candidate)
	for entry, roots := range owned {
		for _, decoded := range instructionsByEntry[entry] {
			address, ok := pollLowWRAMAddress(&decoded.Instruction, true)
			if !ok {
				continue
			}
			item := candidates[address]
			if item == nil {
				item = &candidate{roots: make(map[uint32]struct{}), writers: make(map[uint32]struct{})}
				candidates[address] = item
			}
			for root := range roots {
				item.roots[root] = struct{}{}
			}
			item.writers[decoded.PC&0xffffff] = struct{}{}
		}
	}
	readAddresses := make(map[uint16]struct{})
	for _, instructions := range instructionsByEntry {
		for _, decoded := range instructions {
			address, ok := pollLowWRAMAddress(&decoded.Instruction, false)
			if ok {
				readAddresses[address] = struct{}{}
			}
		}
	}
	result := make([]PollInterruptSync, 0, len(candidates))
	for address, item := range candidates {
		if _, read := readAddresses[address]; !read {
			continue
		}
		entry := PollInterruptSync{Address: address}
		for root := range item.roots {
			entry.InterruptEntries = append(entry.InterruptEntries, root)
		}
		for writer := range item.writers {
			entry.WriterPCs = append(entry.WriterPCs, writer)
		}
		sort.Slice(entry.InterruptEntries, func(i, j int) bool { return entry.InterruptEntries[i] < entry.InterruptEntries[j] })
		sort.Slice(entry.WriterPCs, func(i, j int) bool { return entry.WriterPCs[i] < entry.WriterPCs[j] })
		result = append(result, entry)
	}
	sort.Slice(result, func(i, j int) bool { return result[i].Address < result[j].Address })
	return result
}

func decoderAddress24(bank byte, pc uint16) uint32 {
	return uint32(bank)<<16 | uint32(pc)
}

func pollDirectCallOrTailTarget(instruction cpu65816.Instruction) (uint32, bool) {
	switch {
	case instruction.Mnemonic == "JSR" && instruction.Mode == cpu65816.ABS:
		return instruction.Operand, true
	case instruction.Mnemonic == "JSL":
		return instruction.Operand, true
	case instruction.Mnemonic == "JMP" && instruction.Mode == cpu65816.ABS:
		return instruction.Operand, true
	case instruction.Mnemonic == "JML", instruction.Mnemonic == "JMP" && instruction.Mode == cpu65816.LONG:
		return instruction.Operand, true
	default:
		return 0, false
	}
}

func pollLowWRAMAddress(instruction *cpu65816.Instruction, write bool) (uint16, bool) {
	access := xrefAccess(instruction)
	if write {
		if access != "write" && access != "read_write" {
			return 0, false
		}
	} else if access != "read" && access != "read_write" {
		return 0, false
	}
	address := uint16(instruction.Operand)
	if address >= 0x2000 {
		return 0, false
	}
	switch instruction.Mode {
	case cpu65816.ABS, cpu65816.ABSX, cpu65816.ABSY:
		return address, true
	case cpu65816.LONG, cpu65816.LONGX:
		bank := byte(instruction.Operand >> 16)
		if bank == 0 || bank == 0x7e || bank == 0x7f {
			return address, true
		}
	}
	return 0, false
}

func classifyPollResult(result shadowDecodeResult, registers map[uint16]struct{}, selection map[uint16]string, accumulators map[pollSiteKey]*pollSiteAccumulator) {
	byPC := make(map[uint32][]shadowDecodedInstruction)
	readRegister := make(map[uint32]uint16)
	for _, decoded := range result.instructions {
		pc := decoded.PC & 0xffffff
		byPC[pc] = append(byPC[pc], decoded)
		if register, _, ok := pollReadRegister(&decoded.Instruction, registers); ok {
			readRegister[pc] = register
		}
	}
	postReads := make(map[uint32]uint16)
	for _, decoded := range result.instructions {
		pc := decoded.PC & 0xffffff
		register, resolution, ok := pollReadRegister(&decoded.Instruction, registers)
		if !ok {
			continue
		}
		classification := "single_read"
		var branchPC, branchTarget *uint32
		nextPC := pc + uint32(decoded.Instruction.Length)
		if nextRegister, exists := readRegister[nextPC]; exists && nextRegister == register {
			classification = "clear_read"
		}
		cursor := nextPC
		for step := 0; step < 8; step++ {
			candidates := byPC[cursor]
			if len(candidates) == 0 {
				break
			}
			candidate := candidates[0].Instruction
			if pollConditionalBranch(&candidate) {
				target := cursor&0xff0000 | uint32(uint16(candidate.Operand))
				if target == pc || classification != "clear_read" && readRegister[target] == register {
					classification = "poll_loop"
					branchValue, targetValue := cursor, target
					branchPC, branchTarget = &branchValue, &targetValue
					postReads[cursor+uint32(candidate.Length)] = register
				}
				break
			}
			if disassemblyFlowEnd(disassemblyMnemonic(&candidate)) {
				break
			}
			cursor += uint32(candidate.Length)
		}
		key := pollSiteKey{PC: pc, Register: register, M: decoded.M & 1, X: decoded.X & 1}
		accumulator := accumulators[key]
		if accumulator == nil {
			accumulator = &pollSiteAccumulator{Site: PollSite{
				PC: pc, Register: register,
				InstructionBytes: "", Mnemonic: decoded.Instruction.Mnemonic, AddressingMode: decoded.Instruction.Mode.String(),
				Resolution: resolution, Classification: classification, BranchPC: branchPC, BranchTarget: branchTarget,
				LiveMX:       analysis.MXState{M: decoded.M & 1, X: decoded.X & 1},
				Reachability: "configuration_or_static_call_rooted",
				Selection:    selection[register],
			}, Owners: make(map[uint32]struct{})}
			accumulators[key] = accumulator
		}
		accumulator.Owners[decoded.FunctionEntry&0xffffff] = struct{}{}
		if pollClassificationPriority(classification) > pollClassificationPriority(accumulator.Site.Classification) {
			accumulator.Site.Classification, accumulator.Site.BranchPC, accumulator.Site.BranchTarget = classification, branchPC, branchTarget
		}
	}
	for pc, register := range postReads {
		for key, accumulator := range accumulators {
			if key.PC == pc && key.Register == register && pollClassificationPriority(accumulator.Site.Classification) < pollClassificationPriority("post_poll_read") {
				accumulator.Site.Classification = "post_poll_read"
			}
		}
	}
}

func pollReadRegister(instruction *cpu65816.Instruction, registers map[uint16]struct{}) (uint16, string, bool) {
	access := xrefAccess(instruction)
	if access != "read" && access != "read_write" {
		return 0, "", false
	}
	register := uint16(instruction.Operand)
	if _, ok := registers[register]; !ok {
		return 0, "", false
	}
	switch instruction.Mode {
	case cpu65816.ABS:
		return register, "absolute_operand_db_unproven", true
	case cpu65816.LONG:
		bank := byte(instruction.Operand >> 16)
		if bank == 0 {
			return register, "long_bank_zero", true
		}
		if bank == 0x7e || bank == 0x7f {
			return register, "long_wram", true
		}
	}
	return 0, "", false
}

func pollConditionalBranch(instruction *cpu65816.Instruction) bool {
	switch instruction.Mnemonic {
	case "BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ":
		return instruction.Mode == cpu65816.REL
	default:
		return false
	}
}

func pollClassificationPriority(classification string) int {
	switch classification {
	case "poll_loop":
		return 4
	case "post_poll_read":
		return 3
	case "clear_read":
		return 2
	default:
		return 1
	}
}

func populatePollInstructionBytes(image romimage.Image, report *PollCensusReport) {
	for index := range report.Sites {
		site := &report.Sites[index]
		instruction, err := decodeShadowInstruction(image, byte(site.PC>>16), uint16(site.PC), site.LiveMX.M, site.LiveMX.X)
		if err == nil {
			site.InstructionBytes = shadowInstructionBytes(image, byte(site.PC>>16), uint16(site.PC), instruction.Length)
		}
	}
}

func WritePollCensus(output io.Writer, report PollCensusReport, format string) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(output)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		fmt.Fprintf(output, "poll census v%d: %d read site(s), poll=%d (live=%d) clear=%d post=%d single=%d HLE-conditional=%d HLE-whole=%d\n",
			report.Version, report.Summary.Sites, report.Summary.PollLoops, report.Summary.LivePollLoops,
			report.Summary.ClearReads, report.Summary.PostReads, report.Summary.SingleReads,
			report.Summary.HLEConditional, report.Summary.HLEWholeBody)
		if len(report.InterruptSync) > 0 {
			fmt.Fprintf(output, "interrupt sync discovery: %d low-WRAM address(es) written by NMI/IRQ ownership and read by decoded code\n",
				len(report.InterruptSync))
			for _, sync := range report.InterruptSync {
				fmt.Fprintf(output, "  sync=$%04X interrupt_entries=%s writers=%s\n",
					sync.Address, shadowAddresses(sync.InterruptEntries), shadowAddresses(sync.WriterPCs))
			}
		}
		for _, site := range report.Sites {
			hle := " HLE=" + string(site.HLECoverage)
			detail := ""
			if site.BranchPC != nil && site.BranchTarget != nil {
				detail = fmt.Sprintf(" branch=$%02X:%04X->$%02X:%04X", byte(*site.BranchPC>>16), uint16(*site.BranchPC), byte(*site.BranchTarget>>16), uint16(*site.BranchTarget))
			}
			fmt.Fprintf(output, "  $%02X:%04X $%04X %-14s %-4s %-24s M%dX%d source=%s%s%s owners=%s bytes=%s\n",
				byte(site.PC>>16), uint16(site.PC), site.Register, site.Classification, site.Mnemonic,
				site.Resolution, site.LiveMX.M, site.LiveMX.X, site.Selection, hle, detail,
				shadowAddresses(site.FunctionEntries), site.InstructionBytes)
		}
		return nil
	default:
		return fmt.Errorf("unknown poll census format %q (want text or json)", format)
	}
}
