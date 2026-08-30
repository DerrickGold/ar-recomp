package tooling

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"sort"
	"strconv"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const xrefReportVersion = 2

type XrefOptions struct {
	ROMPath               string
	CFGDir                string
	Jobs                  int
	OnlyBank              *byte
	Query                 XrefQuery
	AccessFilter          string
	IncludeWRAMMirrors    bool
	IncludeRawWords       bool
	IncludeTargetMinusOne bool
}

// XrefQuery preserves the spelling-derived address width. `$1C`, `$001C`,
// and `$00:001C` intentionally ask different questions: direct-page operand,
// 16-bit operand, and architecturally known 24-bit address respectively.
type XrefQuery struct {
	Address uint32 `json:"address"`
	Bits    uint8  `json:"bits"`
}

type XrefSummary struct {
	References      int `json:"references"`
	RawWordEvidence int `json:"raw_word_evidence"`
	UniqueSourcePCs int `json:"unique_source_pcs"`
	InitialVariants int `json:"initial_variants"`
	FinalVariants   int `json:"final_variants"`
	VariantPasses   int `json:"variant_passes"`
	DecodeIssues    int `json:"decode_issues"`
}

// XrefRawWord is deliberately not an XrefReference. It records byte-pattern
// evidence from ROM data without claiming that the bytes are code, a table, or
// reachable. Consumers can combine it with ownership and runtime evidence.
type XrefRawWord struct {
	PC               uint32 `json:"pc"`
	InstructionBytes string `json:"bytes"`
	Value            uint16 `json:"value"`
	Target           uint16 `json:"target"`
	TargetAdjustment int    `json:"target_adjustment"`
	Evidence         string `json:"evidence"`
	Ownership        string `json:"ownership"`
	Reachability     string `json:"reachability"`
}

type XrefReference struct {
	PC               uint32           `json:"pc"`
	InstructionBytes string           `json:"instruction_bytes"`
	Mnemonic         string           `json:"mnemonic"`
	AddressingMode   string           `json:"addressing_mode"`
	Operand          uint32           `json:"operand"`
	Access           string           `json:"access"`
	Resolution       string           `json:"resolution"`
	LiveMX           analysis.MXState `json:"live_mx"`
	FunctionEntries  []uint32         `json:"function_entries"`
	Reachability     string           `json:"reachability"`
}

type XrefReport struct {
	Version      int                 `json:"version"`
	Mode         string              `json:"mode"`
	NoWrite      bool                `json:"no_write"`
	ROM          ShadowROM           `json:"rom"`
	Query        XrefQuery           `json:"query"`
	AccessFilter string              `json:"access_filter"`
	Summary      XrefSummary         `json:"summary"`
	References   []XrefReference     `json:"references"`
	RawWords     []XrefRawWord       `json:"raw_words,omitempty"`
	DecodeIssues []ShadowDecodeIssue `json:"decode_issues,omitempty"`
	Limitations  []string            `json:"limitations,omitempty"`
}

func ParseXrefQuery(value string) (XrefQuery, error) {
	text := strings.TrimSpace(value)
	text = strings.TrimPrefix(text, "$")
	text = strings.TrimPrefix(strings.TrimPrefix(text, "0x"), "0X")
	text = strings.ReplaceAll(text, "_", "")
	bits := uint8(0)
	if strings.Contains(text, ":") {
		parts := strings.Split(text, ":")
		if len(parts) != 2 || len(parts[0]) == 0 || len(parts[0]) > 2 || len(parts[1]) == 0 || len(parts[1]) > 4 {
			return XrefQuery{}, fmt.Errorf("bad 24-bit address %q (want BB:AAAA)", value)
		}
		text = parts[0] + parts[1]
		bits = 24
	} else {
		switch {
		case len(text) >= 1 && len(text) <= 2:
			bits = 8
		case len(text) <= 4:
			bits = 16
		case len(text) <= 6:
			bits = 24
		default:
			return XrefQuery{}, fmt.Errorf("bad hexadecimal address %q", value)
		}
	}
	parsed, err := strconv.ParseUint(text, 16, int(bits))
	if err != nil {
		return XrefQuery{}, fmt.Errorf("parse address %q: %w", value, err)
	}
	return XrefQuery{Address: uint32(parsed), Bits: bits}, nil
}

func BuildXref(options XrefOptions) (XrefReport, error) {
	if options.Jobs <= 0 {
		options.Jobs = 1
	}
	if options.Query.Bits != 8 && options.Query.Bits != 16 && options.Query.Bits != 24 {
		return XrefReport{}, fmt.Errorf("xref query width must be 8, 16, or 24 bits")
	}
	accessFilter, err := normalizeXrefAccessFilter(options.AccessFilter)
	if err != nil {
		return XrefReport{}, err
	}
	image, err := romimage.Load(options.ROMPath)
	if err != nil {
		return XrefReport{}, err
	}
	banks, err := loadShadowBanks(options.CFGDir, options.OnlyBank)
	if err != nil {
		return XrefReport{}, err
	}
	regions := collectShadowDataRegions(banks)
	results, stats, err := discoverShadowDecodeResults(
		image, banks, regions, collectShadowExitMX(banks), options.Jobs)
	if err != nil {
		return XrefReport{}, err
	}
	references, issues := collectXrefReferences(image, results, options.Query, accessFilter, options.IncludeWRAMMirrors)
	rawWords := collectXrefRawWords(image, options.Query, options.OnlyBank, options.IncludeRawWords, options.IncludeTargetMinusOne)
	hash := sha256.Sum256(image)
	uniquePCs := make(map[uint32]struct{})
	for _, reference := range references {
		uniquePCs[reference.PC&0xffffff] = struct{}{}
	}
	return XrefReport{
		Version: xrefReportVersion, Mode: "decoded_instruction_xref", NoWrite: true,
		ROM:   ShadowROM{SHA256: hex.EncodeToString(hash[:]), Size: len(image), Mapper: "lorom"},
		Query: options.Query, AccessFilter: accessFilter,
		Summary: XrefSummary{
			References: len(references), RawWordEvidence: len(rawWords), UniqueSourcePCs: len(uniquePCs),
			InitialVariants: stats.initialVariants, FinalVariants: stats.finalVariants,
			VariantPasses: stats.passes, DecodeIssues: len(issues),
		},
		References: references, RawWords: rawWords, DecodeIssues: issues,
		Limitations: []string{
			"references are limited to configuration- or static-call-rooted decoded instruction boundaries",
			"raw word evidence scans every byte offset and does not imply code/data ownership or reachability",
			"direct-page operands are offsets from live D; absolute data operands are relative to live DB unless the resolution says otherwise",
			"indexed references identify a base operand, not a proven runtime effective address",
		},
	}, nil
}

type xrefReferenceKey struct {
	pc, operand uint32
	m, x        uint8
	mnemonic    string
	mode        cpu65816.AddressingMode
	access      string
	resolution  string
}

func collectXrefReferences(image romimage.Image, results []shadowDecodeResult, query XrefQuery, accessFilter string, includeWRAMMirrors bool) ([]XrefReference, []ShadowDecodeIssue) {
	owners := make(map[xrefReferenceKey]map[uint32]struct{})
	var issues []ShadowDecodeIssue
	for _, result := range results {
		if result.issue != nil {
			issues = append(issues, *result.issue)
			continue
		}
		for _, decoded := range result.instructions {
			access, resolution, match := matchXrefInstruction(decoded, query, includeWRAMMirrors)
			if !match || !xrefAccessMatches(accessFilter, access) {
				continue
			}
			key := xrefReferenceKey{
				pc: decoded.PC & 0xffffff, operand: decoded.Instruction.Operand & 0xffffff,
				m: decoded.M & 1, x: decoded.X & 1, mnemonic: decoded.Instruction.Mnemonic,
				mode: decoded.Instruction.Mode, access: access, resolution: resolution,
			}
			if owners[key] == nil {
				owners[key] = make(map[uint32]struct{})
			}
			owners[key][decoded.FunctionEntry&0xffffff] = struct{}{}
		}
	}
	references := make([]XrefReference, 0, len(owners))
	for key, entries := range owners {
		functionEntries := make([]uint32, 0, len(entries))
		for entry := range entries {
			functionEntries = append(functionEntries, entry)
		}
		sort.Slice(functionEntries, func(i, j int) bool { return functionEntries[i] < functionEntries[j] })
		instruction, _ := decodeShadowInstruction(image, byte(key.pc>>16), uint16(key.pc), key.m, key.x)
		length := uint8(1)
		if instruction != nil {
			length = instruction.Length
		}
		references = append(references, XrefReference{
			PC: key.pc, InstructionBytes: shadowInstructionBytes(image, byte(key.pc>>16), uint16(key.pc), length),
			Mnemonic: key.mnemonic, AddressingMode: key.mode.String(), Operand: key.operand,
			Access: key.access, Resolution: key.resolution,
			LiveMX: analysis.MXState{M: key.m, X: key.x}, FunctionEntries: functionEntries,
			Reachability: "configuration_or_static_call_rooted",
		})
	}
	sort.Slice(references, func(i, j int) bool {
		left, right := references[i], references[j]
		if left.PC != right.PC {
			return left.PC < right.PC
		}
		if left.LiveMX.M != right.LiveMX.M {
			return left.LiveMX.M < right.LiveMX.M
		}
		if left.LiveMX.X != right.LiveMX.X {
			return left.LiveMX.X < right.LiveMX.X
		}
		if left.Mnemonic != right.Mnemonic {
			return left.Mnemonic < right.Mnemonic
		}
		return left.AddressingMode < right.AddressingMode
	})
	sort.Slice(issues, func(i, j int) bool {
		if issues[i].FunctionEntry != issues[j].FunctionEntry {
			return issues[i].FunctionEntry < issues[j].FunctionEntry
		}
		if issues[i].EntryMX.M != issues[j].EntryMX.M {
			return issues[i].EntryMX.M < issues[j].EntryMX.M
		}
		return issues[i].EntryMX.X < issues[j].EntryMX.X
	})
	return references, issues
}

func matchXrefInstruction(decoded shadowDecodedInstruction, query XrefQuery, includeWRAMMirrors bool) (string, string, bool) {
	instruction := &decoded.Instruction
	if xrefBranch(instruction) {
		target := decoded.PC&0xff0000 | uint32(uint16(instruction.Operand))
		return "branch", "program_bank_branch_target", query.Bits == 24 && target == query.Address&0xffffff
	}
	access := xrefAccess(instruction)
	if access == "" {
		return "", "", false
	}
	operand := instruction.Operand & 0xffffff
	switch instruction.Mode {
	case cpu65816.DP:
		return access, "direct_page_offset", query.Bits == 8 && uint8(operand) == uint8(query.Address)
	case cpu65816.DPX, cpu65816.DPY:
		return access, "indexed_direct_page_base", query.Bits == 8 && uint8(operand) == uint8(query.Address)
	case cpu65816.INDIRY, cpu65816.INDIRLY, cpu65816.INDIRDPX, cpu65816.DPINDIR:
		return access, "direct_page_pointer_offset", query.Bits == 8 && uint8(operand) == uint8(query.Address)
	case cpu65816.ABS:
		if query.Bits == 16 {
			return access, xrefAbsoluteResolution(instruction, false), uint16(operand) == uint16(query.Address)
		}
		if query.Bits == 24 && xrefControlTarget(instruction) {
			target := decoded.PC&0xff0000 | uint32(uint16(operand))
			return access, "program_bank_control_target", target == query.Address&0xffffff
		}
	case cpu65816.ABSX, cpu65816.ABSY:
		return access, "indexed_absolute_db_base", query.Bits == 16 && uint16(operand) == uint16(query.Address)
	case cpu65816.INDIR:
		if query.Bits == 16 {
			return access, "bank_zero_indirect_pointer", uint16(operand) == uint16(query.Address)
		}
		if query.Bits == 24 {
			return access, "bank_zero_indirect_pointer", query.Address&0xffffff == uint32(uint16(operand))
		}
	case cpu65816.INDIRX:
		if query.Bits == 16 {
			return access, "program_bank_indexed_pointer_base", uint16(operand) == uint16(query.Address)
		}
		if query.Bits == 24 {
			base := decoded.PC&0xff0000 | uint32(uint16(operand))
			return access, "program_bank_indexed_pointer_base", base == query.Address&0xffffff
		}
	case cpu65816.INDIRL:
		if query.Bits == 16 {
			return access, "bank_zero_long_indirect_pointer", uint16(operand) == uint16(query.Address)
		}
		if query.Bits == 24 {
			return access, "bank_zero_long_indirect_pointer", query.Address&0xffffff == uint32(uint16(operand))
		}
	case cpu65816.LONG:
		if includeWRAMMirrors && query.Bits == 16 && xrefWRAMMirror(operand, query.Address) {
			return access, "long_wram_mirror", true
		}
		return access, xrefLongResolution(instruction, false), query.Bits == 24 && operand == query.Address&0xffffff
	case cpu65816.LONGX:
		if includeWRAMMirrors && query.Bits == 16 && xrefWRAMMirror(operand, query.Address) {
			return access, "indexed_long_wram_mirror", true
		}
		return access, xrefLongResolution(instruction, true), query.Bits == 24 && operand == query.Address&0xffffff
	case cpu65816.STK:
		return access, "stack_relative_offset", query.Bits == 8 && uint8(operand) == uint8(query.Address)
	case cpu65816.STKIY:
		return access, "stack_relative_indirect_y_offset", query.Bits == 8 && uint8(operand) == uint8(query.Address)
	}
	return "", "", false
}

func xrefWRAMMirror(operand, query uint32) bool {
	bank := byte(operand >> 16)
	return (bank == 0x00 || bank == 0x7e || bank == 0x7f) && uint16(operand) == uint16(query)
}

func normalizeXrefAccessFilter(value string) (string, error) {
	filter := strings.ToLower(strings.TrimSpace(value))
	filter = strings.ReplaceAll(filter, "-", "_")
	if filter == "" || filter == "decoded" {
		return "all", nil
	}
	switch filter {
	case "all", "read", "write", "read_write", "control", "branch", "pointer_read":
		return filter, nil
	default:
		return "", fmt.Errorf("unknown xref access filter %q (want all, read, write, read-write, control, branch, or pointer-read)", value)
	}
}

func xrefAccessMatches(filter, access string) bool {
	switch filter {
	case "all":
		return true
	case "read":
		return access == "read" || access == "read_write" || access == "pointer_read"
	case "write":
		return access == "write" || access == "read_write"
	case "control":
		return access == "control" || access == "branch" || access == "pointer_read"
	default:
		return filter == access
	}
}

func xrefBranch(instruction *cpu65816.Instruction) bool {
	switch instruction.Mnemonic {
	case "BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ", "BRA", "BRL":
		return instruction.Mode == cpu65816.REL || instruction.Mode == cpu65816.REL16
	default:
		return false
	}
}

func collectXrefRawWords(image romimage.Image, query XrefQuery, onlyBank *byte, include, includeMinusOne bool) []XrefRawWord {
	if !include || query.Bits == 8 || len(image) < 2 {
		return nil
	}
	target := uint16(query.Address)
	queryBank := byte(query.Address >> 16)
	var words []XrefRawWord
	for bankOffset := 0; bankOffset < len(image); bankOffset += 0x8000 {
		bank := byte(bankOffset / 0x8000)
		if onlyBank != nil && bank != *onlyBank&0x7f {
			continue
		}
		if query.Bits == 24 && bank != queryBank&0x7f {
			continue
		}
		bankLength := len(image) - bankOffset
		if bankLength > 0x8000 {
			bankLength = 0x8000
		}
		for relative := 0; relative+1 < bankLength; relative++ {
			value := uint16(image[bankOffset+relative]) | uint16(image[bankOffset+relative+1])<<8
			adjustment := 0
			if value != target {
				if !includeMinusOne || value != target-1 {
					continue
				}
				adjustment = -1
			}
			words = append(words, XrefRawWord{
				PC:               uint32(bank)<<16 | uint32(0x8000+relative),
				InstructionBytes: fmt.Sprintf("%02X %02X", byte(value), byte(value>>8)),
				Value:            value, Target: target, TargetAdjustment: adjustment,
				Evidence: "raw_rom_word", Ownership: "unclassified", Reachability: "unknown",
			})
		}
	}
	return words
}

func xrefAccess(instruction *cpu65816.Instruction) string {
	switch instruction.Mnemonic {
	case "STA", "STX", "STY", "STZ":
		return "write"
	case "ASL", "LSR", "ROL", "ROR", "INC", "DEC", "TSB", "TRB":
		if instruction.Mode != cpu65816.ACC {
			return "read_write"
		}
		return ""
	case "JMP", "JML", "JSR", "JSL":
		if instruction.Mode == cpu65816.INDIR || instruction.Mode == cpu65816.INDIRX || instruction.Mode == cpu65816.INDIRL {
			return "pointer_read"
		}
		return "control"
	case "PEA", "PER", "BRK", "COP", "MVN", "MVP":
		return ""
	default:
		switch instruction.Mode {
		case cpu65816.IMP, cpu65816.ACC, cpu65816.IMM, cpu65816.REL, cpu65816.REL16:
			return ""
		default:
			return "read"
		}
	}
}

func xrefControlTarget(instruction *cpu65816.Instruction) bool {
	return instruction.Mnemonic == "JSR" || instruction.Mnemonic == "JMP"
}

func xrefAbsoluteResolution(instruction *cpu65816.Instruction, indexed bool) string {
	if xrefControlTarget(instruction) {
		return "program_bank_control_target"
	}
	if indexed {
		return "indexed_absolute_db_base"
	}
	return "absolute_db_operand"
}

func xrefLongResolution(instruction *cpu65816.Instruction, indexed bool) string {
	if instruction.Mnemonic == "JSL" || instruction.Mnemonic == "JML" {
		return "long_control_target"
	}
	if indexed {
		return "indexed_long_base"
	}
	return "long_address"
}

func WriteXrefReport(output io.Writer, report XrefReport, format string) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(output)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		fmt.Fprintf(output, "xref v%d: %s, %d decoded reference(s) at %d unique source PC(s), %d raw word evidence item(s); variants %d -> %d in %d pass(es)\n",
			report.Version, formatXrefQuery(report.Query), report.Summary.References,
			report.Summary.UniqueSourcePCs, report.Summary.RawWordEvidence, report.Summary.InitialVariants,
			report.Summary.FinalVariants, report.Summary.VariantPasses)
		for _, reference := range report.References {
			fmt.Fprintf(output, "  $%02X:%04X %-11s %-5s %-10s M%dX%d %-28s owners=%s bytes=%s\n",
				byte(reference.PC>>16), uint16(reference.PC), reference.Access,
				reference.Mnemonic, reference.AddressingMode,
				reference.LiveMX.M, reference.LiveMX.X, reference.Resolution,
				shadowAddresses(reference.FunctionEntries), reference.InstructionBytes)
		}
		for _, word := range report.RawWords {
			tag := "word"
			if word.TargetAdjustment == -1 {
				tag = "word-1"
			}
			fmt.Fprintf(output, "  $%02X:%04X %-11s .dw  $%04X      ownership=%s reachability=%s bytes=%s\n",
				byte(word.PC>>16), uint16(word.PC), tag, word.Value, word.Ownership, word.Reachability, word.InstructionBytes)
		}
		if len(report.DecodeIssues) > 0 {
			fmt.Fprintf(output, "decode issues=%d (use --format json for details)\n", len(report.DecodeIssues))
		}
		return nil
	default:
		return fmt.Errorf("unknown xref format %q (want text or json)", format)
	}
}

func formatXrefQuery(query XrefQuery) string {
	switch query.Bits {
	case 8:
		return fmt.Sprintf("$%02X direct-page/stack operand", uint8(query.Address))
	case 16:
		return fmt.Sprintf("$%04X 16-bit operand", uint16(query.Address))
	default:
		return fmt.Sprintf("$%02X:%04X architectural address", byte(query.Address>>16), uint16(query.Address))
	}
}
