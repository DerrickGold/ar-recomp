package tooling

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const disassemblyReportVersion = 1

type DisassemblyOptions struct {
	ROMPath      string
	MetadataPath string
	StartPC      uint32
	EntryM       uint8
	EntryX       uint8
	Count        int
	UntilFlow    bool
}

type DisassemblyInstruction struct {
	PC               uint32           `json:"pc"`
	InstructionBytes string           `json:"instruction_bytes"`
	Mnemonic         string           `json:"mnemonic"`
	AddressingMode   string           `json:"addressing_mode"`
	Operand          uint32           `json:"operand"`
	Formatted        string           `json:"formatted"`
	LiveMX           analysis.MXState `json:"live_mx"`
	Target           *uint32          `json:"target,omitempty"`
	Annotations      []string         `json:"annotations,omitempty"`
}

type DisassemblyReport struct {
	Version      int                      `json:"version"`
	Mode         string                   `json:"mode"`
	NoWrite      bool                     `json:"no_write"`
	StartPC      uint32                   `json:"start_pc"`
	EntryMX      analysis.MXState         `json:"entry_mx"`
	Instructions []DisassemblyInstruction `json:"instructions"`
	StopReason   string                   `json:"stop_reason"`
}

// ParseProgramAddress accepts a fully qualified SNES program address. Requiring
// a bank avoids silently disassembling the right 16-bit address in the wrong
// LoROM bank.
func ParseProgramAddress(value string) (uint32, error) {
	text := strings.TrimSpace(value)
	text = strings.TrimPrefix(text, "$")
	text = strings.TrimPrefix(strings.TrimPrefix(text, "0x"), "0X")
	text = strings.ReplaceAll(text, "_", "")
	if strings.Contains(text, ":") {
		parts := strings.Split(text, ":")
		if len(parts) != 2 || len(parts[0]) == 0 || len(parts[0]) > 2 || len(parts[1]) == 0 || len(parts[1]) > 4 {
			return 0, fmt.Errorf("bad program address %q (want BB:AAAA)", value)
		}
		text = parts[0] + parts[1]
	}
	if len(text) != 6 {
		return 0, fmt.Errorf("program address %q needs an explicit bank (want BB:AAAA)", value)
	}
	parsed, err := strconv.ParseUint(text, 16, 24)
	if err != nil {
		return 0, fmt.Errorf("parse program address %q: %w", value, err)
	}
	return uint32(parsed), nil
}

func BuildDisassembly(options DisassemblyOptions) (DisassemblyReport, error) {
	if options.Count <= 0 {
		return DisassemblyReport{}, fmt.Errorf("disassembly count must be positive")
	}
	image, err := romimage.Load(options.ROMPath)
	if err != nil {
		return DisassemblyReport{}, err
	}
	metadata, err := loadOptionalGeneratedMetadata(options.MetadataPath)
	if err != nil {
		return DisassemblyReport{}, err
	}

	report := DisassemblyReport{
		Version: disassemblyReportVersion, Mode: "linear_65816_disassembly", NoWrite: true,
		StartPC: options.StartPC & 0xffffff,
		EntryMX: analysis.MXState{M: options.EntryM & 1, X: options.EntryX & 1},
	}
	pc := options.StartPC & 0xffffff
	m, x := options.EntryM&1, options.EntryX&1
	for len(report.Instructions) < options.Count {
		bank, address := byte(pc>>16), uint16(pc)
		instruction, decodeErr := decodeShadowInstruction(image, bank, address, m, x)
		if decodeErr != nil {
			return DisassemblyReport{}, fmt.Errorf("decode $%02X:%04X M%dX%d: %w", bank, address, m, x, decodeErr)
		}
		mnemonic := disassemblyMnemonic(instruction)
		addressingMode := instruction.Mode.String()
		if instruction.Opcode == 0xdc {
			addressingMode = "[abs]"
		}
		target := disassemblyTarget(instruction, mnemonic, bank)
		line := DisassemblyInstruction{
			PC: pc, InstructionBytes: shadowInstructionBytes(image, bank, address, instruction.Length),
			Mnemonic: mnemonic, AddressingMode: addressingMode, Operand: instruction.Operand,
			Formatted: formatDisassemblyInstruction(instruction, mnemonic),
			LiveMX:    analysis.MXState{M: m, X: x}, Target: target,
			Annotations: disassemblyAnnotations(metadata, pc, target),
		}
		report.Instructions = append(report.Instructions, line)
		if options.UntilFlow && disassemblyFlowEnd(mnemonic) {
			report.StopReason = "flow_end"
			return report, nil
		}
		if mnemonic == "SEP" {
			if instruction.Operand&0x20 != 0 {
				m = 1
			}
			if instruction.Operand&0x10 != 0 {
				x = 1
			}
		} else if mnemonic == "REP" {
			if instruction.Operand&0x20 != 0 {
				m = 0
			}
			if instruction.Operand&0x10 != 0 {
				x = 0
			}
		}
		pc = uint32(bank)<<16 | uint32(address+uint16(instruction.Length))
	}
	report.StopReason = "count"
	return report, nil
}

func loadOptionalGeneratedMetadata(path string) (*GeneratedMetadata, error) {
	if strings.TrimSpace(path) == "" {
		return nil, nil
	}
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("read metadata: %w", err)
	}
	var metadata GeneratedMetadata
	if err := json.Unmarshal(data, &metadata); err != nil {
		return nil, fmt.Errorf("parse metadata %s: %w", path, err)
	}
	return &metadata, nil
}

func disassemblyMnemonic(instruction *cpu65816.Instruction) string {
	// The compiler models long jumps as JMP+LONG and JML [abs] as
	// JMP+INDIR internally. Use the architectural spelling in user output.
	if instruction.Opcode == 0x5c || instruction.Opcode == 0xdc {
		return "JML"
	}
	return instruction.Mnemonic
}

func disassemblyTarget(instruction *cpu65816.Instruction, mnemonic string, bank byte) *uint32 {
	var target uint32
	switch instruction.Mode {
	case cpu65816.REL, cpu65816.REL16:
		if !xrefBranch(instruction) {
			return nil
		}
		target = uint32(bank)<<16 | uint32(uint16(instruction.Operand))
	case cpu65816.ABS:
		if mnemonic != "JSR" && mnemonic != "JMP" {
			return nil
		}
		target = uint32(bank)<<16 | uint32(uint16(instruction.Operand))
	case cpu65816.LONG:
		if mnemonic != "JSL" && mnemonic != "JML" {
			return nil
		}
		target = instruction.Operand & 0xffffff
	default:
		return nil
	}
	return &target
}

func formatDisassemblyInstruction(instruction *cpu65816.Instruction, mnemonic string) string {
	return strings.TrimSpace(fmt.Sprintf("%-5s %s", mnemonic, formatDisassemblyOperand(instruction)))
}

func formatDisassemblyOperand(instruction *cpu65816.Instruction) string {
	value := instruction.Operand
	if instruction.Opcode == 0xdc {
		return fmt.Sprintf("[$%04X]", uint16(value))
	}
	switch instruction.Mode {
	case cpu65816.IMP:
		return ""
	case cpu65816.ACC:
		return "A"
	case cpu65816.IMM:
		if instruction.Mnemonic == "MVN" || instruction.Mnemonic == "MVP" {
			return fmt.Sprintf("$%02X,$%02X", byte(value>>8), byte(value))
		}
		if instruction.Length == 3 {
			return fmt.Sprintf("#$%04X", uint16(value))
		}
		return fmt.Sprintf("#$%02X", byte(value))
	case cpu65816.DP:
		return fmt.Sprintf("$%02X", byte(value))
	case cpu65816.DPX:
		return fmt.Sprintf("$%02X,X", byte(value))
	case cpu65816.DPY:
		return fmt.Sprintf("$%02X,Y", byte(value))
	case cpu65816.ABS:
		return fmt.Sprintf("$%04X", uint16(value))
	case cpu65816.ABSX:
		return fmt.Sprintf("$%04X,X", uint16(value))
	case cpu65816.ABSY:
		return fmt.Sprintf("$%04X,Y", uint16(value))
	case cpu65816.LONG:
		return fmt.Sprintf("$%02X:%04X", byte(value>>16), uint16(value))
	case cpu65816.LONGX:
		return fmt.Sprintf("$%02X:%04X,X", byte(value>>16), uint16(value))
	case cpu65816.REL, cpu65816.REL16:
		return fmt.Sprintf("$%04X", uint16(value))
	case cpu65816.STK:
		return fmt.Sprintf("$%02X,S", byte(value))
	case cpu65816.INDIR:
		return fmt.Sprintf("($%04X)", uint16(value))
	case cpu65816.INDIRX:
		return fmt.Sprintf("($%04X,X)", uint16(value))
	case cpu65816.INDIRY:
		return fmt.Sprintf("($%02X),Y", byte(value))
	case cpu65816.INDIRLY:
		return fmt.Sprintf("[$%02X],Y", byte(value))
	case cpu65816.INDIRL:
		return fmt.Sprintf("[$%02X]", byte(value))
	case cpu65816.INDIRDPX:
		return fmt.Sprintf("($%02X,X)", byte(value))
	case cpu65816.DPINDIR:
		return fmt.Sprintf("($%02X)", byte(value))
	case cpu65816.STKIY:
		return fmt.Sprintf("($%02X,S),Y", byte(value))
	default:
		return fmt.Sprintf("$%X", value)
	}
}

func disassemblyFlowEnd(mnemonic string) bool {
	switch mnemonic {
	case "RTS", "RTL", "RTI", "STP", "JMP", "JML", "BRA", "BRL":
		return true
	default:
		return false
	}
}

func disassemblyAnnotations(metadata *GeneratedMetadata, pc uint32, target *uint32) []string {
	if metadata == nil {
		return nil
	}
	key := fmt.Sprintf("%02X%04X", byte(pc>>16), uint16(pc))
	var annotations []string
	if variants := metadata.Functions[key]; len(variants) != 0 {
		cleaned := make([]string, len(variants))
		for index, variant := range variants {
			cleaned[index] = strings.TrimPrefix(variant, "_")
		}
		annotations = append(annotations, fmt.Sprintf("FUNC[%s]", strings.Join(cleaned, ",")))
	} else if len(metadata.Labels[key]) != 0 {
		annotations = append(annotations, "label")
	}
	if target != nil {
		targetKey := fmt.Sprintf("%02X%04X", byte(*target>>16), uint16(*target))
		formatted := fmt.Sprintf("$%02X:%04X", byte(*target>>16), uint16(*target))
		if len(metadata.Functions[targetKey]) != 0 {
			annotations = append(annotations, "->FUNC "+formatted)
		} else if len(metadata.Labels[targetKey]) != 0 {
			annotations = append(annotations, "->label "+formatted)
		}
	}
	return annotations
}

func WriteDisassemblyReport(output io.Writer, report DisassemblyReport, format string, raw bool) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(output)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		for _, instruction := range report.Instructions {
			bytes := ""
			if raw {
				bytes = fmt.Sprintf("%-12s", instruction.InstructionBytes)
			}
			note := ""
			if len(instruction.Annotations) != 0 {
				note = "   ; " + strings.Join(instruction.Annotations, " ")
			}
			fmt.Fprintf(output, "%02X:%04X  %s%-20s m=%d x=%d%s\n",
				byte(instruction.PC>>16), uint16(instruction.PC), bytes, instruction.Formatted,
				instruction.LiveMX.M, instruction.LiveMX.X, note)
		}
		return nil
	default:
		return fmt.Errorf("unknown disassembly format %q (want text or json)", format)
	}
}
