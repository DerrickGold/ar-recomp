// Package config parses the v2 bank cfg format used by snesrecomp-go.
package config

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
	"unicode"
)

type MX struct {
	M uint8 `json:"m"`
	X uint8 `json:"x"`
}

type Entry struct {
	Name         string  `json:"name"`
	Start        uint16  `json:"start"`
	End          *uint16 `json:"end,omitempty"`
	EntryMX      MX      `json:"entry_mx"`
	ExitMX       *MX     `json:"exit_mx,omitempty"`
	TailCallPC   *uint16 `json:"tail_call_pc,omitempty"`
	EntrySOffset int     `json:"entry_s_offset"`
}

type NameDecl struct {
	Address uint32 `json:"address"`
	Name    string `json:"name"`
}

type Range struct {
	Start uint16 `json:"start"`
	End   uint16 `json:"end"`
}

type DataRegion struct {
	Bank  byte   `json:"bank"`
	Start uint16 `json:"start"`
	End   uint16 `json:"end"`
}

type ExitMXAt struct {
	Address uint32 `json:"address"`
	Exit    MX     `json:"exit"`
}

type IndirectDispatch struct {
	SitePC     uint16   `json:"site_pc"`
	Count      int      `json:"count"`
	IndexReg   string   `json:"index_reg"`
	TableBases []uint16 `json:"table_bases,omitempty"`
	ReturnPC   *uint16  `json:"return_pc,omitempty"`
	SEPMask    byte     `json:"sep_mask,omitempty"`
}

type RTSDispatch struct {
	SitePC  uint16   `json:"site_pc"`
	Targets []uint16 `json:"targets"`
}

type Config struct {
	Bank             byte               `json:"bank"`
	Includes         []string           `json:"includes,omitempty"`
	Entries          []Entry            `json:"entries"`
	Names            []NameDecl         `json:"names,omitempty"`
	ExcludeRanges    []Range            `json:"exclude_ranges,omitempty"`
	DataRegions      []DataRegion       `json:"data_regions,omitempty"`
	ExitMXAt         []ExitMXAt         `json:"exit_mx_at,omitempty"`
	AutoVectors      bool               `json:"auto_vectors,omitempty"`
	IndirectDispatch []IndirectDispatch `json:"indirect_dispatch,omitempty"`
	RTSDispatch      []RTSDispatch      `json:"rts_dispatch,omitempty"`
	HLESPCUpload     []uint16           `json:"hle_spc_upload,omitempty"`
	HLEFunctions     map[uint16]string  `json:"hle_functions,omitempty"`
	HLEDispatch      map[uint16]string  `json:"hle_dispatch,omitempty"`
	ForceVariantAt   map[uint32]MX      `json:"force_variant_at,omitempty"`
}

func Load(path string) (*Config, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open cfg: %w", err)
	}
	defer file.Close()

	cfg := &Config{
		HLEFunctions:   make(map[uint16]string),
		HLEDispatch:    make(map[uint16]string),
		ForceVariantAt: make(map[uint32]MX),
	}
	bankSeen := false
	scanner := bufio.NewScanner(file)
	lineNo := 0
	for scanner.Scan() {
		lineNo++
		line := scanner.Text()
		if comment := strings.IndexByte(line, '#'); comment >= 0 {
			line = line[:comment]
		}
		fields := strings.Fields(line)
		if len(fields) == 0 {
			continue
		}
		fail := func(format string, args ...any) error {
			return fmt.Errorf("%s:%d: %s", path, lineNo, fmt.Sprintf(format, args...))
		}

		switch fields[0] {
		case "bank":
			if len(fields) < 3 || fields[1] != "=" {
				continue
			}
			value, parseErr := parseHex(fields[2], 8)
			if parseErr != nil {
				return nil, fail("bad bank %q: %v", fields[2], parseErr)
			}
			cfg.Bank = byte(value)
			bankSeen = true
		case "includes":
			if len(fields) >= 3 && fields[1] == "=" {
				cfg.Includes = append([]string(nil), fields[2:]...)
			}
		case "comment":
			// v2 intentionally ignores this v1 directive.
		case "auto_vectors":
			cfg.AutoVectors = true
		case "hle_spc_upload":
			if len(fields) != 2 {
				return nil, fail("hle_spc_upload needs exactly one <pc> argument")
			}
			pc, parseErr := parseHex(fields[1], 16)
			if parseErr != nil {
				return nil, fail("hle_spc_upload bad pc %q: %v", fields[1], parseErr)
			}
			cfg.HLESPCUpload = append(cfg.HLESPCUpload, uint16(pc))
		case "hle_func", "hle_dispatch":
			if len(fields) != 3 {
				return nil, fail("%s needs <pc> <c_function_name>", fields[0])
			}
			pc, parseErr := parseHex(fields[1], 16)
			if parseErr != nil {
				return nil, fail("%s bad pc %q: %v", fields[0], fields[1], parseErr)
			}
			if !isCIdentifier(fields[2]) {
				return nil, fail("%s requires a valid C identifier, got %q", fields[0], fields[2])
			}
			if fields[0] == "hle_func" {
				cfg.HLEFunctions[uint16(pc)] = fields[2]
			} else {
				cfg.HLEDispatch[uint16(pc)] = fields[2]
			}
		case "force_variant_at":
			if len(fields) != 4 {
				return nil, fail("force_variant_at needs <site_pc24> <m> <x>")
			}
			site, parseErr := parseHex(fields[1], 24)
			if parseErr != nil {
				return nil, fail("force_variant_at bad site %q: %v", fields[1], parseErr)
			}
			mx, parseErr := parseMX(fields[2], fields[3])
			if parseErr != nil {
				return nil, fail("force_variant_at: %v", parseErr)
			}
			if _, duplicate := cfg.ForceVariantAt[uint32(site)]; duplicate {
				return nil, fail("force_variant_at duplicate site $%06X", site)
			}
			cfg.ForceVariantAt[uint32(site)] = mx
		case "indirect_dispatch":
			directive, parseErr := parseIndirectDispatch(fields)
			if parseErr != nil {
				return nil, fail("%v", parseErr)
			}
			cfg.IndirectDispatch = append(cfg.IndirectDispatch, directive)
		case "rts_dispatch":
			if len(fields) < 3 {
				return nil, fail("rts_dispatch needs <site_pc16> <target1> [target2 ...]")
			}
			site, parseErr := parseHex(fields[1], 16)
			if parseErr != nil {
				return nil, fail("rts_dispatch bad site %q: %v", fields[1], parseErr)
			}
			directive := RTSDispatch{SitePC: uint16(site)}
			for _, token := range fields[2:] {
				target, targetErr := parseHex(token, 16)
				if targetErr != nil {
					return nil, fail("rts_dispatch bad target %q: %v", token, targetErr)
				}
				directive.Targets = append(directive.Targets, uint16(target))
			}
			cfg.RTSDispatch = append(cfg.RTSDispatch, directive)
		case "func":
			if len(fields) < 3 {
				continue
			}
			start, parseErr := parseHex(fields[2], 16)
			if parseErr != nil {
				return nil, fail("func bad start %q: %v", fields[2], parseErr)
			}
			entry := Entry{Name: fields[1], Start: uint16(start), EntryMX: MX{M: 1, X: 1}}
			parseEntryOptions(&entry, fields[3:])
			cfg.Entries = append(cfg.Entries, entry)
		case "name":
			if len(fields) < 3 {
				continue
			}
			address, parseErr := parseHex(fields[1], 24)
			if parseErr == nil {
				cfg.Names = append(cfg.Names, NameDecl{Address: uint32(address), Name: fields[2]})
			}
		case "exclude_range":
			if len(fields) < 3 {
				continue
			}
			start, startErr := parseHex(fields[1], 16)
			end, endErr := parseHex(fields[2], 16)
			if startErr == nil && endErr == nil {
				cfg.ExcludeRanges = append(cfg.ExcludeRanges, Range{Start: uint16(start), End: uint16(end)})
			}
		case "exit_mx_at":
			if len(fields) < 4 {
				continue
			}
			address, addressErr := parseHex(fields[1], 24)
			mx, mxErr := parseMXMasked(fields[2], fields[3])
			if addressErr == nil && mxErr == nil {
				cfg.ExitMXAt = append(cfg.ExitMXAt, ExitMXAt{Address: uint32(address), Exit: mx})
			}
		case "data_region":
			if len(fields) < 4 {
				continue
			}
			bank, bankErr := parseHex(fields[1], 8)
			start, startErr := parseHex(fields[2], 16)
			end, endErr := parseHex(fields[3], 16)
			if bankErr == nil && startErr == nil && endErr == nil {
				cfg.DataRegions = append(cfg.DataRegions, DataRegion{Bank: byte(bank), Start: uint16(start), End: uint16(end)})
			}
		default:
			// Ignore v1-only and future directives like the Python loader.
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("scan cfg %s: %w", path, err)
	}
	if !bankSeen {
		return nil, fmt.Errorf("%s: missing 'bank = NN' line", path)
	}

	// Match Python's in-bank name auto-promotion.
	existing := make(map[uint16]struct{}, len(cfg.Entries))
	for _, entry := range cfg.Entries {
		existing[entry.Start] = struct{}{}
	}
	for _, name := range cfg.Names {
		if byte(name.Address>>16) != cfg.Bank {
			continue
		}
		pc := uint16(name.Address)
		if _, found := existing[pc]; found {
			continue
		}
		cfg.Entries = append(cfg.Entries, Entry{Name: name.Name, Start: pc, EntryMX: MX{M: 1, X: 1}})
		existing[pc] = struct{}{}
	}
	return cfg, nil
}

func parseEntryOptions(entry *Entry, options []string) {
	for _, option := range options {
		key, value, found := strings.Cut(option, ":")
		if !found {
			continue
		}
		switch key {
		case "end":
			if parsed, err := parseHex(value, 16); err == nil {
				v := uint16(parsed)
				entry.End = &v
			}
		case "tail_call":
			if parsed, err := parseHex(value, 16); err == nil {
				v := uint16(parsed)
				entry.TailCallPC = &v
			}
		case "entry_mx":
			if mx, err := parseMXCSV(value); err == nil {
				entry.EntryMX = mx
			}
		case "exit_mx":
			if mx, err := parseMXCSV(value); err == nil {
				entry.ExitMX = &mx
			}
		case "entry_s_offset":
			if parsed, err := strconv.Atoi(value); err == nil {
				entry.EntrySOffset = parsed
			}
		}
	}
}

func parseIndirectDispatch(fields []string) (IndirectDispatch, error) {
	if len(fields) < 4 {
		return IndirectDispatch{}, fmt.Errorf("indirect_dispatch needs <site_pc> <count> idx:<reg>")
	}
	site, err := parseHex(fields[1], 16)
	if err != nil {
		return IndirectDispatch{}, fmt.Errorf("indirect_dispatch bad site %q: %w", fields[1], err)
	}
	count64, err := strconv.ParseInt(fields[2], 0, 32)
	if err != nil || count64 <= 0 || count64 > 4096 {
		return IndirectDispatch{}, fmt.Errorf("indirect_dispatch count %q outside 1..4096", fields[2])
	}
	directive := IndirectDispatch{SitePC: uint16(site), Count: int(count64)}
	for _, option := range fields[3:] {
		key, value, found := strings.Cut(option, ":")
		if !found {
			return IndirectDispatch{}, fmt.Errorf("indirect_dispatch unknown option %q", option)
		}
		switch key {
		case "idx":
			directive.IndexReg = strings.ToUpper(value)
			if directive.IndexReg != "X" && directive.IndexReg != "Y" && directive.IndexReg != "A" {
				return IndirectDispatch{}, fmt.Errorf("indirect_dispatch idx must be X, Y, or A")
			}
		case "tables":
			parts := strings.Split(value, ",")
			if len(parts) < 1 || len(parts) > 3 {
				return IndirectDispatch{}, fmt.Errorf("indirect_dispatch tables needs 1-3 bases")
			}
			for _, part := range parts {
				base, parseErr := parseHex(part, 16)
				if parseErr != nil {
					return IndirectDispatch{}, fmt.Errorf("indirect_dispatch bad table %q: %w", part, parseErr)
				}
				directive.TableBases = append(directive.TableBases, uint16(base))
			}
		case "ret":
			pc, parseErr := parseHex(value, 16)
			if parseErr != nil {
				return IndirectDispatch{}, fmt.Errorf("indirect_dispatch bad return pc %q: %w", value, parseErr)
			}
			v := uint16(pc)
			directive.ReturnPC = &v
		case "sep":
			mask, parseErr := parseHex(value, 8)
			if parseErr != nil {
				return IndirectDispatch{}, fmt.Errorf("indirect_dispatch bad SEP mask %q: %w", value, parseErr)
			}
			directive.SEPMask = byte(mask)
		default:
			return IndirectDispatch{}, fmt.Errorf("indirect_dispatch unknown option %q", option)
		}
	}
	if directive.IndexReg == "" {
		return IndirectDispatch{}, fmt.Errorf("indirect_dispatch needs idx:X, idx:Y, or idx:A")
	}
	if directive.IndexReg == "A" && len(directive.TableBases) == 0 {
		return IndirectDispatch{}, fmt.Errorf("indirect_dispatch idx:A needs tables:<base>")
	}
	return directive, nil
}

func parseHex(token string, bits int) (uint64, error) {
	token = strings.TrimPrefix(strings.TrimPrefix(token, "0x"), "0X")
	value, err := strconv.ParseUint(token, 16, bits)
	if err != nil {
		return 0, err
	}
	return value, nil
}

func parseMX(m, x string) (MX, error) {
	mValue, mErr := strconv.Atoi(m)
	xValue, xErr := strconv.Atoi(x)
	if mErr != nil || xErr != nil || (mValue != 0 && mValue != 1) || (xValue != 0 && xValue != 1) {
		return MX{}, fmt.Errorf("m and x must each be 0 or 1")
	}
	return MX{M: uint8(mValue), X: uint8(xValue)}, nil
}

func parseMXMasked(m, x string) (MX, error) {
	mValue, mErr := strconv.Atoi(m)
	xValue, xErr := strconv.Atoi(x)
	if mErr != nil || xErr != nil {
		return MX{}, fmt.Errorf("invalid m/x")
	}
	return MX{M: uint8(mValue & 1), X: uint8(xValue & 1)}, nil
}

func parseMXCSV(value string) (MX, error) {
	parts := strings.Split(value, ",")
	if len(parts) != 2 {
		return MX{}, fmt.Errorf("expected M,X")
	}
	return parseMXMasked(parts[0], parts[1])
}

func isCIdentifier(value string) bool {
	if value == "" {
		return false
	}
	for index, r := range value {
		if index == 0 && unicode.IsDigit(r) {
			return false
		}
		if r != '_' && !unicode.IsLetter(r) && !unicode.IsDigit(r) {
			return false
		}
	}
	return true
}
