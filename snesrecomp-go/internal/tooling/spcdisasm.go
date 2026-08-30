package tooling

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
)

const spcDisassemblyVersion = 1

var spcOpcodeRows = []string{
	"nop|tcall 0|set1 {b}.0|bbs {b}.0,{r}|or a,{b}|or a,!{w}|or a,(x)|or a,[{b}+x]|or a,#{b}|or {b},{b2}|or1 c,{m}|asl {b}|asl !{w}|push psw|tset1 !{w}|brk",
	"bpl {r}|tcall 1|clr1 {b}.0|bbc {b}.0,{r}|or a,{b}+x|or a,!{w}+x|or a,!{w}+y|or a,[{b}]+y|or {b},#{b2}|or (x),(y)|decw {b}|asl {b}+x|asl a|dec x|cmp x,!{w}|jmp [!{w}+x]",
	"clrp|tcall 2|set1 {b}.1|bbs {b}.1,{r}|and a,{b}|and a,!{w}|and a,(x)|and a,[{b}+x]|and a,#{b}|and {b},{b2}|or1 c,/{m}|rol {b}|rol !{w}|push a|cbne {b},{r}|bra {r}",
	"bmi {r}|tcall 3|clr1 {b}.1|bbc {b}.1,{r}|and a,{b}+x|and a,!{w}+x|and a,!{w}+y|and a,[{b}]+y|and {b},#{b2}|and (x),(y)|incw {b}|rol {b}+x|rol a|inc x|cmp x,{b}|call !{w}",
	"setp|tcall 4|set1 {b}.2|bbs {b}.2,{r}|eor a,{b}|eor a,!{w}|eor a,(x)|eor a,[{b}+x]|eor a,#{b}|eor {b},{b2}|and1 c,{m}|lsr {b}|lsr !{w}|push x|tclr1 !{w}|pcall ${b}",
	"bvc {r}|tcall 5|clr1 {b}.2|bbc {b}.2,{r}|eor a,{b}+x|eor a,!{w}+x|eor a,!{w}+y|eor a,[{b}]+y|eor {b},#{b2}|eor (x),(y)|cmpw ya,{b}|lsr {b}+x|lsr a|mov x,a|cmp y,!{w}|jmp !{w}",
	"clrc|tcall 6|set1 {b}.3|bbs {b}.3,{r}|cmp a,{b}|cmp a,!{w}|cmp a,(x)|cmp a,[{b}+x]|cmp a,#{b}|cmp {b},{b2}|and1 c,/{m}|ror {b}|ror !{w}|push y|dbnz {b},{r}|ret",
	"bvs {r}|tcall 7|clr1 {b}.3|bbc {b}.3,{r}|cmp a,{b}+x|cmp a,!{w}+x|cmp a,!{w}+y|cmp a,[{b}]+y|cmp {b},#{b2}|cmp (x),(y)|addw ya,{b}|ror {b}+x|ror a|mov a,x|cmp y,{b}|reti",
	"setc|tcall 8|set1 {b}.4|bbs {b}.4,{r}|adc a,{b}|adc a,!{w}|adc a,(x)|adc a,[{b}+x]|adc a,#{b}|adc {b},{b2}|eor1 c,{m}|dec {b}|dec !{w}|mov y,#{b}|pop psw|mov {b},#{b2}",
	"bcc {r}|tcall 9|clr1 {b}.4|bbc {b}.4,{r}|adc a,{b}+x|adc a,!{w}+x|adc a,!{w}+y|adc a,[{b}]+y|adc {b},#{b2}|adc (x),(y)|subw ya,{b}|dec {b}+x|dec a|mov x,sp|div ya,x|xcn a",
	"ei|tcall 10|set1 {b}.5|bbs {b}.5,{r}|sbc a,{b}|sbc a,!{w}|sbc a,(x)|sbc a,[{b}+x]|sbc a,#{b}|sbc {b},{b2}|mov1 c,{m}|inc {b}|inc !{w}|cmp y,#{b}|pop a|mov (x)+,a",
	"bcs {r}|tcall 11|clr1 {b}.5|bbc {b}.5,{r}|sbc a,{b}+x|sbc a,!{w}+x|sbc a,!{w}+y|sbc a,[{b}]+y|sbc {b},#{b2}|sbc (x),(y)|movw ya,{b}|inc {b}+x|inc a|mov sp,x|das a|mov a,(x)+",
	"di|tcall 12|set1 {b}.6|bbs {b}.6,{r}|mov {b},a|mov !{w},a|mov (x),a|mov [{b}+x],a|cmp x,#{b}|mov !{w},x|mov1 {m},c|mov {b},y|mov !{w},y|mov x,#{b}|pop x|mul ya",
	"bne {r}|tcall 13|clr1 {b}.6|bbc {b}.6,{r}|mov {b}+x,a|mov !{w}+x,a|mov !{w}+y,a|mov [{b}]+y,a|mov {b},x|mov {b}+y,x|movw {b},ya|mov {b}+x,y|dec y|mov a,y|cbne {b}+x,{r}|daa a",
	"clrv|tcall 14|set1 {b}.7|bbs {b}.7,{r}|mov a,{b}|mov a,!{w}|mov a,(x)|mov a,[{b}+x]|mov a,#{b}|mov x,!{w}|not1 {m}|mov y,{b}|mov y,!{w}|notc|pop y|sleep",
	"beq {r}|tcall 15|clr1 {b}.7|bbc {b}.7,{r}|mov a,{b}+x|mov a,!{w}+x|mov a,!{w}+y|mov a,[{b}]+y|mov x,{b}|mov x,{b}+y|mov {b},{b2}|mov y,{b}+x|inc y|mov y,a|dbnz y,{r}|stop",
}

type SPCDisassemblyOptions struct {
	InputPath         string
	UploadBlockOffset *int
	FileOffset        int
	LoadAddress       uint16
	StartAddress      uint16
	EndAddress        uint16
	FindReference     *uint16
}

type SPCInstruction struct {
	PC         uint16   `json:"pc"`
	Bytes      string   `json:"bytes"`
	Opcode     byte     `json:"opcode"`
	Text       string   `json:"text"`
	References []uint16 `json:"references,omitempty"`
}

type SPCDisassemblyReport struct {
	Version      int              `json:"version"`
	Mode         string           `json:"mode"`
	NoWrite      bool             `json:"no_write"`
	InputSHA256  string           `json:"input_sha256"`
	FileOffset   int              `json:"file_offset"`
	LoadAddress  uint16           `json:"load_address"`
	PayloadBytes int              `json:"payload_bytes"`
	StartAddress uint16           `json:"start_address"`
	EndAddress   uint16           `json:"end_address"`
	Instructions []SPCInstruction `json:"instructions"`
	Truncated    bool             `json:"truncated"`
}

func BuildSPCDisassembly(options SPCDisassemblyOptions) (SPCDisassemblyReport, error) {
	data, err := os.ReadFile(options.InputPath)
	if err != nil {
		return SPCDisassemblyReport{}, fmt.Errorf("read SPC input: %w", err)
	}
	if options.FileOffset < 0 || options.FileOffset > len(data) {
		return SPCDisassemblyReport{}, fmt.Errorf("SPC file offset %#x outside %d-byte input", options.FileOffset, len(data))
	}
	payload := data[options.FileOffset:]
	loadAddress := options.LoadAddress
	fileOffset := options.FileOffset
	if options.UploadBlockOffset != nil {
		offset := *options.UploadBlockOffset
		if offset < 0 || offset+4 > len(data) {
			return SPCDisassemblyReport{}, fmt.Errorf("SPC upload block offset %#x outside %d-byte input", offset, len(data))
		}
		length := int(data[offset]) | int(data[offset+1])<<8
		loadAddress = uint16(data[offset+2]) | uint16(data[offset+3])<<8
		fileOffset = offset + 4
		if fileOffset+length > len(data) {
			return SPCDisassemblyReport{}, fmt.Errorf("SPC upload block at %#x declares %d bytes beyond input", offset, length)
		}
		payload = data[fileOffset : fileOffset+length]
	}
	if options.EndAddress <= options.StartAddress {
		return SPCDisassemblyReport{}, fmt.Errorf("SPC end address must be above start address")
	}
	endOfPayload := uint32(loadAddress) + uint32(len(payload))
	if options.StartAddress < loadAddress || uint32(options.EndAddress) > endOfPayload {
		return SPCDisassemblyReport{}, fmt.Errorf("SPC range $%04X-$%04X is outside payload $%04X-$%04X", options.StartAddress, options.EndAddress, loadAddress, uint16(endOfPayload))
	}
	hash := sha256.Sum256(data)
	report := SPCDisassemblyReport{
		Version: spcDisassemblyVersion, Mode: "linear_spc700_disassembly", NoWrite: true,
		InputSHA256: hex.EncodeToString(hash[:]), FileOffset: fileOffset, LoadAddress: loadAddress,
		PayloadBytes: len(payload), StartAddress: options.StartAddress, EndAddress: options.EndAddress,
	}
	templates := spcOpcodeTemplates()
	for pc := options.StartAddress; pc < options.EndAddress; {
		offset := int(pc - loadAddress)
		opcode := payload[offset]
		template := templates[opcode]
		length := 1 + spcOperandLength(template)
		if offset+length > len(payload) || int(pc)+length > int(options.EndAddress) {
			report.Truncated = true
			break
		}
		raw := payload[offset : offset+length]
		references := spcOperandReferences(template, raw)
		if options.FindReference == nil || containsSPCReference(references, *options.FindReference) {
			report.Instructions = append(report.Instructions, SPCInstruction{
				PC: pc, Bytes: formatHexBytes(raw), Opcode: opcode,
				Text: spcDecodeOperand(template, raw, pc), References: references,
			})
		}
		pc += uint16(length)
	}
	return report, nil
}

func spcOpcodeTemplates() [256]string {
	var templates [256]string
	index := 0
	for _, row := range spcOpcodeRows {
		for _, entry := range strings.Split(row, "|") {
			if index < len(templates) {
				templates[index] = entry
			}
			index++
		}
	}
	return templates
}

func spcOperandLength(template string) int {
	if strings.Contains(template, "{w}") || strings.Contains(template, "{m}") || strings.Contains(template, "{b2}") || strings.Contains(template, "{b}") && strings.Contains(template, "{r}") {
		return 2
	}
	if strings.Contains(template, "{b}") || strings.Contains(template, "{r}") {
		return 1
	}
	return 0
}

func spcDecodeOperand(template string, raw []byte, pc uint16) string {
	replace := func(key, value string) { template = strings.ReplaceAll(template, "{"+key+"}", value) }
	if strings.Contains(template, "{w}") || strings.Contains(template, "{m}") {
		word := uint16(raw[1]) | uint16(raw[2])<<8
		replace("w", fmt.Sprintf("$%04X", word))
		replace("m", fmt.Sprintf("$%04X.%d", word&0x1fff, word>>13))
	} else if strings.Contains(template, "{b2}") {
		replace("b", fmt.Sprintf("$%02X", raw[2]))
		replace("b2", fmt.Sprintf("$%02X", raw[1]))
	} else if strings.Contains(template, "{b}") {
		replace("b", fmt.Sprintf("$%02X", raw[1]))
	}
	if strings.Contains(template, "{r}") {
		displacement := int8(raw[len(raw)-1])
		target := uint16(int(pc) + len(raw) + int(displacement))
		replace("r", fmt.Sprintf("$%04X", target))
	}
	return template
}

func spcOperandReferences(template string, raw []byte) []uint16 {
	if strings.Contains(template, "{w}") {
		return []uint16{uint16(raw[1]) | uint16(raw[2])<<8}
	}
	if strings.Contains(template, "{m}") {
		return []uint16{(uint16(raw[1]) | uint16(raw[2])<<8) & 0x1fff}
	}
	if strings.Contains(template, "{b2}") {
		return []uint16{uint16(raw[1]), uint16(raw[2])}
	}
	if strings.Contains(template, "{b}") {
		return []uint16{uint16(raw[1])}
	}
	return nil
}

func containsSPCReference(values []uint16, target uint16) bool {
	for _, value := range values {
		if value == target {
			return true
		}
	}
	return false
}

func formatHexBytes(values []byte) string {
	parts := make([]string, len(values))
	for index, value := range values {
		parts[index] = fmt.Sprintf("%02X", value)
	}
	return strings.Join(parts, " ")
}

func ParseSPCAddress(value string) (uint16, error) {
	text := strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(value), "$"), "0x")
	parsed, err := strconv.ParseUint(text, 16, 16)
	if err != nil {
		return 0, fmt.Errorf("parse SPC address %q: %w", value, err)
	}
	return uint16(parsed), nil
}

func WriteSPCDisassembly(output io.Writer, report SPCDisassemblyReport, format string) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(output)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		for _, instruction := range report.Instructions {
			fmt.Fprintf(output, "%04X: %-8s %s\n", instruction.PC, instruction.Bytes, instruction.Text)
		}
		if report.Truncated {
			fmt.Fprintln(output, "warning: final instruction is truncated by the requested range or payload")
		}
		return nil
	default:
		return fmt.Errorf("unknown SPC disassembly format %q (want text or json)", format)
	}
}
