package tooling

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

type RTSCensusOptions struct {
	ROMPath       string
	CFGDir        string
	Bank          *byte
	Suggest       bool
	YieldHelpers  bool
	YieldFieldMax uint16
	Output        io.Writer
}

type RTSCensusReport struct {
	UncoveredPushes        int
	UncoveredSites         int
	YieldHelpers           int
	YieldContinuations     int
	UncoveredContinuations int
}

type rtsCoverage struct {
	Sites   map[uint16]struct{}
	Targets map[uint16]struct{}
	Returns map[uint16]struct{}
	Funcs   map[uint16]struct{}
}

type continuationPush struct {
	PC, Immediate, Target uint16
	Score                 int
	Covered               bool
}

type dispatchSite struct {
	PC      uint16
	Covered bool
}

type yieldHelperInstruction struct {
	PC          uint16
	Instruction *cpu65816.Instruction
}

type yieldHelperEvidence struct {
	Target, CapturePC, StorePC uint16
	Kind, Capture, Store       string
}

var rtsBankConfigRE = regexp.MustCompile(`(?i)^bank([0-9a-f]{2})\.cfg$`)

// CensusRTSWebs performs the Go-only pushed-continuation RTS-web census.
func CensusRTSWebs(options RTSCensusOptions) (RTSCensusReport, error) {
	if options.Output == nil {
		options.Output = io.Discard
	}
	if options.YieldFieldMax == 0 {
		options.YieldFieldMax = 0x40
	}
	image, err := rom.Load(options.ROMPath)
	if err != nil {
		return RTSCensusReport{}, err
	}
	coverage, err := loadRTSCoverage(options.CFGDir)
	if err != nil {
		return RTSCensusReport{}, err
	}
	bankCount := len(image) / 0x8000
	var banks []int
	if options.Bank != nil {
		banks = []int{int(*options.Bank)}
	} else {
		banks = make([]int, bankCount)
		for index := range banks {
			banks[index] = index
		}
	}
	report := RTSCensusReport{}
	for _, bankNumber := range banks {
		if bankNumber < 0 || bankNumber >= bankCount {
			return report, fmt.Errorf("bank %02X outside %d-bank ROM", bankNumber, bankCount)
		}
		bank := byte(bankNumber)
		cov := coverage[bank]
		if cov.Sites == nil {
			cov = newRTSCoverage()
		}
		var pushes []continuationPush
		var sites []dispatchSite
		base := bankNumber << 15
		for offset := base; offset < base+0x8000-4; offset++ {
			pc := uint16(offset&0x7fff) | 0x8000
			b0, b1, b2, b3 := image[offset], image[offset+1], image[offset+2], image[offset+3]
			if (b0 == 0xA9 && b3 == 0x48) || (b0 == 0xA0 && b3 == 0x5A) {
				immediate := uint16(b1) | uint16(b2)<<8
				target := immediate + 1
				if target >= 0x8000 && target < 0xfff0 {
					score := max(plausibilityScore(image, bank, target, 0, 0, 6), plausibilityScore(image, bank, target, 1, 0, 6))
					if score >= 2 {
						_, inTargets := cov.Targets[target]
						_, inReturns := cov.Returns[target]
						_, inFuncs := cov.Funcs[target]
						pushes = append(pushes, continuationPush{PC: pc, Immediate: immediate, Target: target, Score: score, Covered: inTargets || inReturns || inFuncs})
					}
				}
			}
			if b0 == 0x48 && b1 == 0x60 {
				rtsPC := pc + 1
				_, covered := cov.Sites[rtsPC]
				sites = append(sites, dispatchSite{PC: rtsPC, Covered: covered})
			}
		}
		uncoveredPushes, uncoveredSites := 0, 0
		for _, push := range pushes {
			if !push.Covered {
				uncoveredPushes++
			}
		}
		for _, site := range sites {
			if !site.Covered {
				uncoveredSites++
			}
		}
		var helpers []yieldHelperEvidence
		var continuations map[uint16][]uint16
		if options.YieldHelpers {
			helpers, continuations = censusYieldHelpers(image, bank, options.YieldFieldMax)
		}
		if len(pushes) == 0 && len(sites) == 0 && len(helpers) == 0 {
			continue
		}
		if options.Bank == nil && uncoveredPushes == 0 && uncoveredSites == 0 && len(helpers) == 0 {
			continue
		}
		fmt.Fprintf(options.Output, "== bank %02x: %d continuation-pushes (%d UNCOVERED), %d PHA;RTS sites (%d uncovered)\n", bank, len(pushes), uncoveredPushes, len(sites), uncoveredSites)
		for _, push := range pushes {
			if push.Covered && options.Bank == nil {
				continue
			}
			mark := "UNC"
			if push.Covered {
				mark = "ok "
			}
			fmt.Fprintf(options.Output, "   [%s] push @%02x:%04x  #$%04X -> cont $%04X (decode-score %d)\n", mark, bank, push.PC, push.Immediate, push.Target, push.Score)
		}
		for _, site := range sites {
			if site.Covered && options.Bank == nil {
				continue
			}
			mark := "UNC"
			if site.Covered {
				mark = "ok "
			}
			fmt.Fprintf(options.Output, "   [%s] PHA;RTS dispatch @%02x:%04x\n", mark, bank, site.PC)
		}
		report.UncoveredPushes += uncoveredPushes
		report.UncoveredSites += uncoveredSites
		if options.Suggest && uncoveredPushes > 0 {
			fmt.Fprintf(options.Output, "   -- suggestions (bank %02x) --\n", bank)
			for _, push := range pushes {
				if push.Covered {
					continue
				}
				if _, isReturn := cov.Returns[push.Target]; isReturn {
					fmt.Fprintf(options.Output, "   SKIP  $%04X: `ret:` of a cfg indirect_dispatch (B8C2 class) — benign unwind, registering it recurses. DO NOT register.\n", push.Target)
					continue
				}
				score1 := plausibilityScore(image, bank, push.Target, 1, 0, 6)
				score0 := plausibilityScore(image, bank, push.Target, 0, 0, 6)
				entryM := 0
				if score1 >= score0 {
					entryM = 1
				}
				fmt.Fprintf(options.Output, "   func bank_%02X_%04X %04X entry_mx:%d,0   # push @%02x:%04x; decode m1=%d m0=%d; VERIFY single-shot shape before applying\n", bank, push.Target, push.Target, entryM, bank, push.PC, score1, score0)
			}
		}
		if options.YieldHelpers {
			if len(helpers) != 0 {
				fmt.Fprintf(options.Output, "   -- stack-captured JSR continuations (field offsets below $%02X) --\n", options.YieldFieldMax)
			}
			for _, helper := range helpers {
				calls := continuations[helper.Target]
				fmt.Fprintf(options.Output, "   helper %02X:%04X: %s %s @%04X -> %s @%04X (%d call site(s))\n",
					bank, helper.Target, helper.Kind, helper.Capture, helper.CapturePC, helper.Store, helper.StorePC, len(calls))
				for _, site := range calls {
					continuation := site + 3
					_, covered := cov.Funcs[continuation]
					mark := "ok"
					if !covered {
						mark = "UNCOVERED"
						report.UncoveredContinuations++
					}
					report.YieldContinuations++
					fmt.Fprintf(options.Output, "      JSR @%02X:%04X -> continuation %02X:%04X [%s]\n", bank, site, bank, continuation, mark)
					if !covered && options.Suggest {
						fmt.Fprintf(options.Output, "      func bank_%02X_%04X %04X entry_mx:0,0   # REVIEW: internal resume after stack-capturing helper %04X\n", bank, continuation, continuation, helper.Target)
					}
				}
			}
			report.YieldHelpers += len(helpers)
		}
	}
	fmt.Fprintf(options.Output, "\nTOTAL uncovered: %d continuation pushes, %d PHA;RTS sites\n", report.UncoveredPushes, report.UncoveredSites)
	fmt.Fprintln(options.Output, "Triage: a push whose continuation does PLA/PLX/PLY of loop state needs rts_dispatch (NOT func); a PHA;RTS site needs rts_dispatch <site> <targets> or indirect_dispatch.")
	if options.YieldHelpers {
		fmt.Fprintf(options.Output, "Yield-helper census: %d helper(s), %d JSR continuation(s), %d uncovered. Treat suggestions as internal resume entries, not ordinary recursive calls.\n",
			report.YieldHelpers, report.YieldContinuations, report.UncoveredContinuations)
	}
	return report, nil
}

func censusYieldHelpers(image rom.Image, bank byte, fieldMax uint16) ([]yieldHelperEvidence, map[uint16][]uint16) {
	calls := make(map[uint16][]uint16)
	base := int(bank) << 15
	for offset := base; offset+2 < base+0x8000 && offset+2 < len(image); offset++ {
		if image[offset] != 0x20 {
			continue
		}
		target := uint16(image[offset+1]) | uint16(image[offset+2])<<8
		if target >= 0x8000 {
			site := uint16(offset-base) | 0x8000
			calls[target] = append(calls[target], site)
		}
	}
	var helpers []yieldHelperEvidence
	for target := range calls {
		if evidence, found := detectYieldHelper(image, bank, target, fieldMax); found {
			helpers = append(helpers, evidence)
		}
	}
	sort.Slice(helpers, func(i, j int) bool { return helpers[i].Target < helpers[j].Target })
	for target := range calls {
		sort.Slice(calls[target], func(i, j int) bool { return calls[target][i] < calls[target][j] })
	}
	return helpers, calls
}

func detectYieldHelper(image rom.Image, bank byte, target, fieldMax uint16) (yieldHelperEvidence, bool) {
	pc, m, x := target, uint8(0), uint8(0)
	var instructions []yieldHelperInstruction
	for range 28 {
		offset, err := rom.LoROMOffset(bank, pc)
		if err != nil || offset >= len(image) {
			break
		}
		instruction, err := cpu65816.Decode(image, offset, pc, bank, m, x)
		if err != nil || instruction == nil {
			break
		}
		instructions = append(instructions, yieldHelperInstruction{PC: pc, Instruction: instruction})
		if instruction.Mnemonic == "SEP" {
			if instruction.Operand&0x20 != 0 {
				m = 1
			}
			if instruction.Operand&0x10 != 0 {
				x = 1
			}
		} else if instruction.Mnemonic == "REP" {
			if instruction.Operand&0x20 != 0 {
				m = 0
			}
			if instruction.Operand&0x10 != 0 {
				x = 0
			}
		}
		pc += uint16(instruction.Length)
	}
	pushBalance := 0
	pulls := map[string]string{"PLA": "A", "PLX": "X", "PLY": "Y"}
	pushes := map[string]bool{"PHA": true, "PHX": true, "PHY": true, "PHP": true, "PHB": true, "PHD": true, "PHK": true, "PEA": true, "PEI": true, "PER": true}
	for index, current := range instructions {
		instruction := current.Instruction
		if pushes[instruction.Mnemonic] {
			pushBalance++
			continue
		}
		if instruction.Mnemonic == "LDA" && instruction.Mode == cpu65816.STK && instruction.Operand == 1 && pushBalance == 0 {
			if store, found := findYieldFieldStore(instructions, index+1, "A", fieldMax); found {
				return yieldHelperEvidence{Target: target, CapturePC: current.PC, StorePC: store.PC, Kind: "peek", Capture: formatDisassemblyInstruction(instruction, "LDA"), Store: formatDisassemblyInstruction(store.Instruction, store.Instruction.Mnemonic)}, true
			}
		}
		if register, pull := pulls[instruction.Mnemonic]; pull {
			if pushBalance > 0 {
				pushBalance--
				continue
			}
			if store, found := findYieldFieldStore(instructions, index+1, register, fieldMax); found {
				return yieldHelperEvidence{Target: target, CapturePC: current.PC, StorePC: store.PC, Kind: "pull", Capture: instruction.Mnemonic, Store: formatDisassemblyInstruction(store.Instruction, store.Instruction.Mnemonic)}, true
			}
			return yieldHelperEvidence{}, false
		}
		switch instruction.Mnemonic {
		case "RTS", "RTL", "RTI", "JMP", "JML", "BRA", "BRL", "STP", "BRK":
			return yieldHelperEvidence{}, false
		}
	}
	return yieldHelperEvidence{}, false
}

func findYieldFieldStore(instructions []yieldHelperInstruction, start int, register string, fieldMax uint16) (yieldHelperInstruction, bool) {
	transfers := map[string][2]string{
		"TAX": {"A", "X"}, "TAY": {"A", "Y"}, "TXA": {"X", "A"},
		"TYA": {"Y", "A"}, "TXY": {"X", "Y"}, "TYX": {"Y", "X"},
	}
	stores := map[string]string{"STA": "A", "STX": "X", "STY": "Y"}
	end := start + 8
	if end > len(instructions) {
		end = len(instructions)
	}
	for _, current := range instructions[start:end] {
		instruction := current.Instruction
		if transfer, found := transfers[instruction.Mnemonic]; found && transfer[0] == register {
			register = transfer[1]
		}
		if stores[instruction.Mnemonic] != register || instruction.Operand >= uint32(fieldMax) {
			continue
		}
		switch instruction.Mode {
		case cpu65816.ABSX, cpu65816.ABSY, cpu65816.DPX:
			return current, true
		}
	}
	return yieldHelperInstruction{}, false
}

func plausibilityScore(image rom.Image, bank byte, pc uint16, m, x uint8, limit int) int {
	count := 0
	for range limit {
		offset, err := rom.LoROMOffset(bank, pc)
		if err != nil || offset+4 > len(image) {
			return count
		}
		instruction, err := cpu65816.Decode(image, offset, pc, bank, m, x)
		if err != nil || instruction == nil || instruction.Mnemonic == "BRK" || instruction.Mnemonic == "COP" {
			return count
		}
		if instruction.Mnemonic == "SEP" && image[offset+1]&0x20 != 0 {
			m = 1
		}
		if instruction.Mnemonic == "REP" && image[offset+1]&0x20 != 0 {
			m = 0
		}
		count++
		switch instruction.Mnemonic {
		case "RTS", "RTL", "JMP", "BRA", "BRL", "JML":
			return count
		}
		pc += uint16(instruction.Length)
	}
	return count
}

func newRTSCoverage() rtsCoverage {
	return rtsCoverage{Sites: make(map[uint16]struct{}), Targets: make(map[uint16]struct{}), Returns: make(map[uint16]struct{}), Funcs: make(map[uint16]struct{})}
}

func loadRTSCoverage(cfgDir string) (map[byte]rtsCoverage, error) {
	paths, err := filepath.Glob(filepath.Join(cfgDir, "bank*.cfg"))
	if err != nil {
		return nil, err
	}
	sort.Strings(paths)
	result := make(map[byte]rtsCoverage)
	for _, path := range paths {
		match := rtsBankConfigRE.FindStringSubmatch(filepath.Base(path))
		if match == nil {
			continue
		}
		parsedBank, err := strconv.ParseUint(match[1], 16, 8)
		if err != nil {
			return nil, err
		}
		bank := byte(parsedBank)
		coverage := newRTSCoverage()
		file, err := os.Open(path)
		if err != nil {
			return nil, err
		}
		scanner := bufio.NewScanner(file)
		for scanner.Scan() {
			line, _, _ := strings.Cut(scanner.Text(), "#")
			fields := strings.Fields(line)
			if len(fields) == 0 {
				continue
			}
			switch fields[0] {
			case "rts_dispatch":
				if len(fields) >= 2 {
					if value, ok := parseHex16(fields[1]); ok {
						coverage.Sites[value] = struct{}{}
					}
				}
				for _, token := range fields[2:] {
					if value, ok := parseHex16(token); ok {
						coverage.Targets[value] = struct{}{}
					}
				}
			case "indirect_dispatch":
				if len(fields) >= 2 {
					if value, ok := parseHex16(fields[1]); ok {
						coverage.Sites[value] = struct{}{}
					}
				}
				for _, token := range fields[2:] {
					if value, found := strings.CutPrefix(token, "ret:"); found {
						if parsed, ok := parseHex16(value); ok {
							coverage.Returns[parsed] = struct{}{}
						}
					}
				}
			case "func":
				for _, token := range fields[1:] {
					if len(token) == 4 {
						if value, ok := parseHex16(token); ok {
							coverage.Funcs[value] = struct{}{}
							break
						}
					}
				}
			}
		}
		scanErr := scanner.Err()
		closeErr := file.Close()
		if scanErr != nil {
			return nil, scanErr
		}
		if closeErr != nil {
			return nil, closeErr
		}
		result[bank] = coverage
	}
	return result, nil
}

func parseHex16(token string) (uint16, bool) {
	value, err := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(token, "0x"), "0X"), 16, 16)
	return uint16(value), err == nil
}
