package tooling

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

const wramInspectVersion = 1

var wramSymbolRow = regexp.MustCompile(`(?i)^\|\s*` + "`?" + `\$7([EF]):([0-9A-F]{4})` + "`?" + `\s*\|(?:\s*\d+\s*\|)?\s*(.+?)\s*\|`)
var markdownDecoration = regexp.MustCompile(`[*` + "`" + `]`)
var symbolSlugPunctuation = regexp.MustCompile(`[^a-z0-9]+`)

type WRAMSymbol struct {
	Address uint32 `json:"address"`
	Name    string `json:"name"`
	Slug    string `json:"slug"`
}

type WRAMSymbolTable struct {
	ByAddress map[uint32]WRAMSymbol
	Symbols   []WRAMSymbol
}

type WRAMFileIdentity struct {
	Path   string `json:"path"`
	Bytes  int    `json:"bytes"`
	SHA256 string `json:"sha256"`
}

type WRAMValue struct {
	Address uint32 `json:"address"`
	Symbol  string `json:"symbol,omitempty"`
	Byte    byte   `json:"byte"`
	Word    uint16 `json:"word"`
}

type WRAMGetFile struct {
	Identity WRAMFileIdentity `json:"identity"`
	Values   []WRAMValue      `json:"values"`
}

type WRAMGetReport struct {
	Version int           `json:"version"`
	Mode    string        `json:"mode"`
	NoWrite bool          `json:"no_write"`
	Files   []WRAMGetFile `json:"files"`
}

type WRAMDifference struct {
	Address uint32 `json:"address"`
	Before  byte   `json:"before"`
	After   byte   `json:"after"`
	Symbol  string `json:"symbol,omitempty"`
}

type WRAMDiffCluster struct {
	Start uint32 `json:"start"`
	End   uint32 `json:"end"`
	Count int    `json:"count"`
}

type WRAMDiffReport struct {
	Version       int               `json:"version"`
	Mode          string            `json:"mode"`
	NoWrite       bool              `json:"no_write"`
	Before        WRAMFileIdentity  `json:"before"`
	After         WRAMFileIdentity  `json:"after"`
	Compared      int               `json:"compared_bytes"`
	Differences   int               `json:"differences"`
	Rows          []WRAMDifference  `json:"rows"`
	Clusters      []WRAMDiffCluster `json:"clusters,omitempty"`
	RowsTruncated int               `json:"rows_truncated,omitempty"`
}

type WRAMScanMatch struct {
	Address uint32 `json:"address"`
	Symbol  string `json:"symbol,omitempty"`
}

type WRAMScanReport struct {
	Version   int              `json:"version"`
	Mode      string           `json:"mode"`
	NoWrite   bool             `json:"no_write"`
	Input     WRAMFileIdentity `json:"input"`
	Width     int              `json:"width"`
	Value     uint16           `json:"value"`
	Low       uint32           `json:"low"`
	High      uint32           `json:"high"`
	Matches   []WRAMScanMatch  `json:"matches"`
	Truncated int              `json:"truncated,omitempty"`
}

func LoadWRAMSymbols(path string) (WRAMSymbolTable, error) {
	table := WRAMSymbolTable{ByAddress: make(map[uint32]WRAMSymbol)}
	if strings.TrimSpace(path) == "" {
		return table, nil
	}
	content, err := os.ReadFile(path)
	if err != nil {
		return table, fmt.Errorf("read WRAM symbol map %s: %w", path, err)
	}
	for _, line := range strings.Split(string(content), "\n") {
		match := wramSymbolRow.FindStringSubmatch(line)
		if match == nil {
			continue
		}
		addressValue, _ := strconv.ParseUint(match[2], 16, 16)
		address := uint32(addressValue)
		if strings.EqualFold(match[1], "F") {
			address += 0x10000
		}
		name := strings.TrimSpace(markdownDecoration.ReplaceAllString(match[3], ""))
		if index := strings.Index(name, "—"); index >= 0 {
			name = strings.TrimSpace(name[:index])
		}
		if index := strings.Index(name, "--"); index >= 0 {
			name = strings.TrimSpace(name[:index])
		}
		if index := strings.Index(name, "("); index >= 0 {
			name = strings.TrimSpace(name[:index])
		}
		if len(name) > 80 {
			name = name[:80]
		}
		slug := strings.Trim(symbolSlugPunctuation.ReplaceAllString(strings.ToLower(name), "-"), "-")
		if _, exists := table.ByAddress[address]; exists || name == "" {
			continue
		}
		symbol := WRAMSymbol{Address: address, Name: name, Slug: slug}
		table.ByAddress[address] = symbol
		table.Symbols = append(table.Symbols, symbol)
	}
	sort.Slice(table.Symbols, func(i, j int) bool { return table.Symbols[i].Address < table.Symbols[j].Address })
	return table, nil
}

func ParseWRAMOffset(value string, symbols WRAMSymbolTable) (uint32, error) {
	trimmed := strings.ToLower(strings.TrimSpace(value))
	trimmed = strings.TrimPrefix(trimmed, "$")
	trimmed = strings.ReplaceAll(trimmed, ":", "")
	hexOnly := trimmed != ""
	for _, character := range trimmed {
		if !strings.ContainsRune("0123456789abcdef", character) {
			hexOnly = false
			break
		}
	}
	if !hexOnly {
		var matches []WRAMSymbol
		for _, symbol := range symbols.Symbols {
			if strings.Contains(symbol.Slug, trimmed) || strings.Contains(strings.ToLower(symbol.Name), trimmed) {
				matches = append(matches, symbol)
			}
		}
		if len(matches) == 1 {
			return matches[0].Address, nil
		}
		if len(matches) == 0 {
			return 0, fmt.Errorf("unknown WRAM symbol %q", value)
		}
		return 0, fmt.Errorf("ambiguous WRAM symbol %q (%s, %s)", value, matches[0].Name, matches[1].Name)
	}
	parsed, err := strconv.ParseUint(trimmed, 16, 24)
	if err != nil {
		return 0, fmt.Errorf("parse WRAM address %q: %w", value, err)
	}
	address := uint32(parsed)
	if address >= 0x7f0000 {
		address = 0x10000 + address&0xffff
	} else if address >= 0x7e0000 {
		address &= 0xffff
	}
	if address > 0x1ffff {
		return 0, fmt.Errorf("WRAM address %q resolves outside $7E-$7F", value)
	}
	return address, nil
}

func readWRAMFile(path string) ([]byte, WRAMFileIdentity, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, WRAMFileIdentity{}, fmt.Errorf("read WRAM dump %s: %w", path, err)
	}
	if len(content) != 0x10000 && len(content) != 0x20000 {
		return nil, WRAMFileIdentity{}, fmt.Errorf("WRAM dump %s is %d bytes (want 65536 or 131072)", path, len(content))
	}
	digest := sha256.Sum256(content)
	return content, WRAMFileIdentity{Path: path, Bytes: len(content), SHA256: hex.EncodeToString(digest[:])}, nil
}

func BuildWRAMGet(paths, queries []string, symbols WRAMSymbolTable) (WRAMGetReport, error) {
	report := WRAMGetReport{Version: wramInspectVersion, Mode: "get", NoWrite: true}
	addresses := make([]uint32, 0, len(queries))
	for _, query := range queries {
		address, err := ParseWRAMOffset(query, symbols)
		if err != nil {
			return report, err
		}
		addresses = append(addresses, address)
	}
	for _, path := range paths {
		content, identity, err := readWRAMFile(path)
		if err != nil {
			return report, err
		}
		file := WRAMGetFile{Identity: identity}
		for _, address := range addresses {
			if int(address)+1 >= len(content) {
				return report, fmt.Errorf("WRAM address %s is outside %s", FormatWRAMOffset(address), path)
			}
			value := WRAMValue{Address: address, Byte: content[address], Word: uint16(content[address]) | uint16(content[address+1])<<8}
			if symbol, found := symbols.ByAddress[address]; found {
				value.Symbol = symbol.Name
			}
			file.Values = append(file.Values, value)
		}
		report.Files = append(report.Files, file)
	}
	return report, nil
}

func BuildWRAMDiff(beforePath, afterPath string, lowDetail uint32, all bool, top int, symbols WRAMSymbolTable) (WRAMDiffReport, error) {
	report := WRAMDiffReport{Version: wramInspectVersion, Mode: "diff", NoWrite: true}
	before, beforeIdentity, err := readWRAMFile(beforePath)
	if err != nil {
		return report, err
	}
	after, afterIdentity, err := readWRAMFile(afterPath)
	if err != nil {
		return report, err
	}
	report.Before, report.After = beforeIdentity, afterIdentity
	compared := len(before)
	if len(after) < compared {
		compared = len(after)
	}
	report.Compared = compared
	var high []uint32
	for index := 0; index < compared; index++ {
		if before[index] == after[index] {
			continue
		}
		report.Differences++
		address := uint32(index)
		if all || address < lowDetail {
			row := WRAMDifference{Address: address, Before: before[index], After: after[index]}
			if symbol, found := symbols.ByAddress[address]; found {
				row.Symbol = symbol.Name
			}
			report.Rows = append(report.Rows, row)
		} else {
			high = append(high, address)
		}
	}
	if !all && len(high) != 0 {
		start, previous, count := high[0], high[0], 1
		for _, address := range high[1:] {
			if address-previous > 16 {
				report.Clusters = append(report.Clusters, WRAMDiffCluster{Start: start, End: previous, Count: count})
				start, count = address, 0
			}
			previous, count = address, count+1
		}
		report.Clusters = append(report.Clusters, WRAMDiffCluster{Start: start, End: previous, Count: count})
	}
	if top > 0 && len(report.Rows) > top {
		report.RowsTruncated = len(report.Rows) - top
		report.Rows = report.Rows[:top]
	}
	return report, nil
}

func BuildWRAMScan(path string, width int, value uint16, low, high uint32, top int, symbols WRAMSymbolTable) (WRAMScanReport, error) {
	report := WRAMScanReport{Version: wramInspectVersion, Mode: "scan", NoWrite: true, Width: width, Value: value, Low: low, High: high}
	content, identity, err := readWRAMFile(path)
	if err != nil {
		return report, err
	}
	report.Input = identity
	if width != 1 && width != 2 {
		return report, fmt.Errorf("WRAM scan width must be 1 or 2")
	}
	if high >= uint32(len(content)) {
		high = uint32(len(content) - 1)
	}
	for address := low; address <= high; address++ {
		match := false
		if width == 1 {
			match = content[address] == byte(value)
		} else if address+1 <= high {
			match = uint16(content[address])|uint16(content[address+1])<<8 == value
		}
		if match {
			row := WRAMScanMatch{Address: address}
			if symbol, found := symbols.ByAddress[address]; found {
				row.Symbol = symbol.Name
			}
			report.Matches = append(report.Matches, row)
		}
		if address == ^uint32(0) {
			break
		}
	}
	if top > 0 && len(report.Matches) > top {
		report.Truncated = len(report.Matches) - top
		report.Matches = report.Matches[:top]
	}
	return report, nil
}

func FormatWRAMOffset(address uint32) string {
	bank := "7E"
	if address >= 0x10000 {
		bank = "7F"
	}
	return fmt.Sprintf("$%s:%04X", bank, address&0xffff)
}

func writeWRAMJSON(writer io.Writer, report any) error {
	encoder := json.NewEncoder(writer)
	encoder.SetIndent("", "  ")
	return encoder.Encode(report)
}

func WriteWRAMGet(writer io.Writer, report WRAMGetReport, format string) error {
	if format == "json" {
		return writeWRAMJSON(writer, report)
	}
	if format != "" && format != "text" {
		return fmt.Errorf("unknown WRAM format %q (want text or json)", format)
	}
	for _, file := range report.Files {
		fmt.Fprintf(writer, "%s", file.Identity.Path)
		for _, value := range file.Values {
			label := FormatWRAMOffset(value.Address)
			if value.Symbol != "" {
				label += "[" + value.Symbol + "]"
			}
			fmt.Fprintf(writer, "  %s=%02X (w:%04X)", label, value.Byte, value.Word)
		}
		fmt.Fprintln(writer)
	}
	return nil
}

func WriteWRAMDiff(writer io.Writer, report WRAMDiffReport, format string) error {
	if format == "json" {
		return writeWRAMJSON(writer, report)
	}
	if format != "" && format != "text" {
		return fmt.Errorf("unknown WRAM format %q (want text or json)", format)
	}
	fmt.Fprintf(writer, "%d byte diffs (%s -> %s)\n", report.Differences, report.Before.Path, report.After.Path)
	for _, row := range report.Rows {
		suffix := ""
		if row.Symbol != "" {
			suffix = "   " + row.Symbol
		}
		fmt.Fprintf(writer, "  %s: %02X -> %02X%s\n", FormatWRAMOffset(row.Address), row.Before, row.After, suffix)
	}
	if report.RowsTruncated != 0 {
		fmt.Fprintf(writer, "  ... %d more detailed row(s)\n", report.RowsTruncated)
	}
	if len(report.Clusters) != 0 {
		fmt.Fprintf(writer, "  high clusters (%d):", len(report.Clusters))
		for _, cluster := range report.Clusters {
			fmt.Fprintf(writer, " %s-%04X", FormatWRAMOffset(cluster.Start), cluster.End&0xffff)
		}
		fmt.Fprintln(writer)
	}
	return nil
}

func WriteWRAMScan(writer io.Writer, report WRAMScanReport, format string) error {
	if format == "json" {
		return writeWRAMJSON(writer, report)
	}
	if format != "" && format != "text" {
		return fmt.Errorf("unknown WRAM format %q (want text or json)", format)
	}
	for _, match := range report.Matches {
		suffix := ""
		if match.Symbol != "" {
			suffix = "   " + match.Symbol
		}
		width := 2
		if report.Width == 2 {
			width = 4
		}
		fmt.Fprintf(writer, "  %s = %0*X%s\n", FormatWRAMOffset(match.Address), width, report.Value, suffix)
	}
	if report.Truncated != 0 {
		fmt.Fprintf(writer, "... %d more match(es)\n", report.Truncated)
	}
	return nil
}

func WriteWRAMSymbols(writer io.Writer, symbols WRAMSymbolTable, filter, format string) error {
	selected := make([]WRAMSymbol, 0, len(symbols.Symbols))
	for _, symbol := range symbols.Symbols {
		if strings.Contains(strings.ToLower(symbol.Name), strings.ToLower(filter)) || strings.Contains(symbol.Slug, strings.ToLower(filter)) {
			selected = append(selected, symbol)
		}
	}
	if format == "json" {
		return writeWRAMJSON(writer, struct {
			Version int          `json:"version"`
			Mode    string       `json:"mode"`
			NoWrite bool         `json:"no_write"`
			Symbols []WRAMSymbol `json:"symbols"`
		}{wramInspectVersion, "symbols", true, selected})
	}
	if format != "" && format != "text" {
		return fmt.Errorf("unknown WRAM format %q (want text or json)", format)
	}
	for _, symbol := range selected {
		fmt.Fprintf(writer, "%s  %s\n", FormatWRAMOffset(symbol.Address), symbol.Name)
	}
	return nil
}

type repeatedWRAMPath []string

func (values *repeatedWRAMPath) String() string { return strings.Join(*values, ",") }
func (values *repeatedWRAMPath) Set(value string) error {
	*values = append(*values, value)
	return nil
}

func addWRAMCommonFlags(flags *flag.FlagSet, defaultRoot string) (root, symbols, format *string) {
	root = flags.String("root", defaultRoot, "game project root used to resolve relative paths")
	symbols = flags.String("symbols", "", "optional Markdown WRAM symbol map, relative to project root")
	format = flags.String("format", "text", "report format: text or json")
	return
}

func resolveWRAMPath(root, path string) string {
	if strings.TrimSpace(path) == "" || filepath.IsAbs(path) {
		return path
	}
	return filepath.Join(root, path)
}

func loadWRAMCLIContext(rootValue, symbolsValue string) (string, WRAMSymbolTable, error) {
	root, err := filepath.Abs(rootValue)
	if err != nil {
		return "", WRAMSymbolTable{}, fmt.Errorf("resolve WRAM project root: %w", err)
	}
	symbols, err := LoadWRAMSymbols(resolveWRAMPath(root, symbolsValue))
	return root, symbols, err
}

// RunWRAMCommand implements the shared v2regen/snesbuild WRAM subcommand.
// Paths are explicit and all operations are read-only.
func RunWRAMCommand(args []string, defaultRoot string, output io.Writer) error {
	if len(args) == 0 {
		return fmt.Errorf("wram needs get, diff, scan, or symbols")
	}
	switch args[0] {
	case "get":
		flags := flag.NewFlagSet("wram get", flag.ContinueOnError)
		rootValue, symbolsValue, format := addWRAMCommonFlags(flags, defaultRoot)
		var paths repeatedWRAMPath
		flags.Var(&paths, "file", "WRAM dump path (repeatable, relative to project root)")
		if err := flags.Parse(args[1:]); err != nil {
			return err
		}
		if len(paths) == 0 || flags.NArg() == 0 {
			return fmt.Errorf("wram get needs at least one --file and one address or symbol")
		}
		root, symbols, err := loadWRAMCLIContext(*rootValue, *symbolsValue)
		if err != nil {
			return err
		}
		for index := range paths {
			paths[index] = resolveWRAMPath(root, paths[index])
		}
		report, err := BuildWRAMGet(paths, flags.Args(), symbols)
		if err != nil {
			return err
		}
		return WriteWRAMGet(output, report, *format)
	case "diff":
		flags := flag.NewFlagSet("wram diff", flag.ContinueOnError)
		rootValue, symbolsValue, format := addWRAMCommonFlags(flags, defaultRoot)
		all := flags.Bool("all", false, "list every differing byte instead of clustering high WRAM")
		low := flags.Uint64("low", 0x800, "show individual differences below this WRAM offset")
		top := flags.Int("top", 1000, "maximum detailed rows")
		if err := flags.Parse(args[1:]); err != nil {
			return err
		}
		if flags.NArg() != 2 || *low > 0x20000 {
			return fmt.Errorf("wram diff needs BEFORE AFTER (and --low within WRAM)")
		}
		root, symbols, err := loadWRAMCLIContext(*rootValue, *symbolsValue)
		if err != nil {
			return err
		}
		report, err := BuildWRAMDiff(resolveWRAMPath(root, flags.Arg(0)), resolveWRAMPath(root, flags.Arg(1)), uint32(*low), *all, *top, symbols)
		if err != nil {
			return err
		}
		return WriteWRAMDiff(output, report, *format)
	case "scan":
		flags := flag.NewFlagSet("wram scan", flag.ContinueOnError)
		rootValue, symbolsValue, format := addWRAMCommonFlags(flags, defaultRoot)
		byteValue := flags.String("byte", "", "hexadecimal byte value")
		wordValue := flags.String("word", "", "little-endian hexadecimal word value")
		rangeValue := flags.String("range", "0000-1FFFF", "inclusive hexadecimal WRAM offset range")
		top := flags.Int("top", 200, "maximum matches")
		if err := flags.Parse(args[1:]); err != nil {
			return err
		}
		if flags.NArg() != 1 || (*byteValue == "") == (*wordValue == "") {
			return fmt.Errorf("wram scan needs exactly one FILE and exactly one of --byte or --word")
		}
		width, text, bits := 1, *byteValue, 8
		if *wordValue != "" {
			width, text, bits = 2, *wordValue, 16
		}
		text = strings.TrimPrefix(strings.TrimPrefix(strings.TrimPrefix(strings.TrimSpace(text), "0x"), "0X"), "$")
		parsed, err := strconv.ParseUint(text, 16, bits)
		if err != nil {
			return fmt.Errorf("parse WRAM scan value: %w", err)
		}
		selectedRange, err := ParseTraceRange(*rangeValue)
		if err != nil || selectedRange.High > 0x1ffff {
			return fmt.Errorf("parse WRAM scan range %q within 0000-1FFFF", *rangeValue)
		}
		root, symbols, err := loadWRAMCLIContext(*rootValue, *symbolsValue)
		if err != nil {
			return err
		}
		report, err := BuildWRAMScan(resolveWRAMPath(root, flags.Arg(0)), width, uint16(parsed), selectedRange.Low, selectedRange.High, *top, symbols)
		if err != nil {
			return err
		}
		return WriteWRAMScan(output, report, *format)
	case "symbols":
		flags := flag.NewFlagSet("wram symbols", flag.ContinueOnError)
		rootValue, symbolsValue, format := addWRAMCommonFlags(flags, defaultRoot)
		if err := flags.Parse(args[1:]); err != nil {
			return err
		}
		if flags.NArg() > 1 || strings.TrimSpace(*symbolsValue) == "" {
			return fmt.Errorf("wram symbols needs --symbols MAP and at most one filter")
		}
		_, symbols, err := loadWRAMCLIContext(*rootValue, *symbolsValue)
		if err != nil {
			return err
		}
		filter := ""
		if flags.NArg() == 1 {
			filter = flags.Arg(0)
		}
		return WriteWRAMSymbols(output, symbols, filter, *format)
	default:
		return fmt.Errorf("unknown wram command %q (want get, diff, scan, or symbols)", args[0])
	}
}
