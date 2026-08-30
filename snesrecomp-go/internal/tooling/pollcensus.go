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

const pollCensusVersion = 1

type PollCensusOptions struct {
	ROMPath, CFGDir string
	Jobs            int
	OnlyBank        *byte
	Registers       []uint16
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
	HLECovered       bool             `json:"hle_covered"`
	Reachability     string           `json:"reachability"`
}

type PollCensusSummary struct {
	Sites           int `json:"sites"`
	PollLoops       int `json:"poll_loops"`
	LivePollLoops   int `json:"live_poll_loops"`
	ClearReads      int `json:"clear_reads"`
	PostReads       int `json:"post_reads"`
	SingleReads     int `json:"single_reads"`
	HLECovered      int `json:"hle_covered"`
	InitialVariants int `json:"initial_variants"`
	FinalVariants   int `json:"final_variants"`
	VariantPasses   int `json:"variant_passes"`
	DecodeIssues    int `json:"decode_issues"`
}

type PollCensusReport struct {
	Version      int                 `json:"version"`
	Mode         string              `json:"mode"`
	NoWrite      bool                `json:"no_write"`
	ROM          ShadowROM           `json:"rom"`
	Registers    []uint16            `json:"registers"`
	Summary      PollCensusSummary   `json:"summary"`
	Sites        []PollSite          `json:"sites"`
	DecodeIssues []ShadowDecodeIssue `json:"decode_issues,omitempty"`
	Limitations  []string            `json:"limitations"`
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
	for _, register := range options.Registers {
		registers[register] = struct{}{}
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
	hleFunctions := make(map[uint32]struct{})
	for _, bank := range banks {
		for pc := range bank.Config.HLEFunctions {
			hleFunctions[uint32(bank.ID)<<16|uint32(pc)] = struct{}{}
		}
		for pc := range bank.Config.HLEFunctionsIf {
			hleFunctions[uint32(bank.ID)<<16|uint32(pc)] = struct{}{}
		}
	}
	accumulators := make(map[pollSiteKey]*pollSiteAccumulator)
	var issues []ShadowDecodeIssue
	for _, result := range results {
		if result.issue != nil {
			issues = append(issues, *result.issue)
			continue
		}
		classifyPollResult(result, registers, hleFunctions, accumulators)
	}
	var sites []PollSite
	for _, accumulator := range accumulators {
		for owner := range accumulator.Owners {
			accumulator.Site.FunctionEntries = append(accumulator.Site.FunctionEntries, owner)
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
	report := PollCensusReport{
		Version: pollCensusVersion, Mode: "decoded_hardware_poll_census", NoWrite: true,
		ROM:       ShadowROM{SHA256: hex.EncodeToString(hash[:]), Size: len(image), Mapper: "lorom"},
		Registers: append([]uint16(nil), options.Registers...), Sites: sites, DecodeIssues: issues,
		Summary: PollCensusSummary{Sites: len(sites), InitialVariants: stats.initialVariants, FinalVariants: stats.finalVariants, VariantPasses: stats.passes, DecodeIssues: len(issues)},
		Limitations: []string{
			"sites are limited to configuration- or static-call-rooted decoded instruction boundaries",
			"absolute hardware operands assume DB resolves to bank zero; long operands explicitly prove bank zero",
			"poll-loop classification is structural evidence and does not by itself authorize scheduler lowering",
		},
	}
	populatePollInstructionBytes(image, &report)
	for _, site := range sites {
		switch site.Classification {
		case "poll_loop":
			report.Summary.PollLoops++
			if !site.HLECovered {
				report.Summary.LivePollLoops++
			}
		case "clear_read":
			report.Summary.ClearReads++
		case "post_poll_read":
			report.Summary.PostReads++
		default:
			report.Summary.SingleReads++
		}
		if site.HLECovered {
			report.Summary.HLECovered++
		}
	}
	return report, nil
}

func classifyPollResult(result shadowDecodeResult, registers map[uint16]struct{}, hleFunctions map[uint32]struct{}, accumulators map[pollSiteKey]*pollSiteAccumulator) {
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
			_, hle := hleFunctions[decoded.FunctionEntry&0xffffff]
			accumulator = &pollSiteAccumulator{Site: PollSite{
				PC: pc, Register: register,
				InstructionBytes: "", Mnemonic: decoded.Instruction.Mnemonic, AddressingMode: decoded.Instruction.Mode.String(),
				Resolution: resolution, Classification: classification, BranchPC: branchPC, BranchTarget: branchTarget,
				LiveMX: analysis.MXState{M: decoded.M & 1, X: decoded.X & 1}, HLECovered: hle,
				Reachability: "configuration_or_static_call_rooted",
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
		if byte(instruction.Operand>>16) == 0 {
			return register, "long_bank_zero", true
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
		fmt.Fprintf(output, "poll census v%d: %d read site(s), poll=%d (live=%d) clear=%d post=%d single=%d HLE-covered=%d\n",
			report.Version, report.Summary.Sites, report.Summary.PollLoops, report.Summary.LivePollLoops,
			report.Summary.ClearReads, report.Summary.PostReads, report.Summary.SingleReads, report.Summary.HLECovered)
		for _, site := range report.Sites {
			hle := ""
			if site.HLECovered {
				hle = " HLE-covered"
			}
			detail := ""
			if site.BranchPC != nil && site.BranchTarget != nil {
				detail = fmt.Sprintf(" branch=$%02X:%04X->$%02X:%04X", byte(*site.BranchPC>>16), uint16(*site.BranchPC), byte(*site.BranchTarget>>16), uint16(*site.BranchTarget))
			}
			fmt.Fprintf(output, "  $%02X:%04X $%04X %-14s %-4s %-24s M%dX%d%s%s owners=%s bytes=%s\n",
				byte(site.PC>>16), uint16(site.PC), site.Register, site.Classification, site.Mnemonic,
				site.Resolution, site.LiveMX.M, site.LiveMX.X, hle, detail,
				shadowAddresses(site.FunctionEntries), site.InstructionBytes)
		}
		return nil
	default:
		return fmt.Errorf("unknown poll census format %q (want text or json)", format)
	}
}
