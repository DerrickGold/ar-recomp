package regen

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/emitter"
)

var (
	topLevelFunctionRE = regexp.MustCompile(`(?m)^(?:RecompReturn|void)\s+([A-Za-z_]\w*)\(CpuState \*cpu\) \{`)
	variantCallRE      = regexp.MustCompile(`\b([A-Za-z_]\w*_M[01]X[01])\(cpu\)`)
	variantSuffixRE    = regexp.MustCompile(`_M[01]X[01]$`)
	syntheticNameRE    = regexp.MustCompile(`^bank_([0-9A-Fa-f]{2})_([0-9A-Fa-f]{4})$`)
)

const forwardMarker = "/* Forward declarations for in-bank entries. */"

func (repo *repository) writeOutputs(options Options, results map[byte][]*emitter.FunctionResult) (files, changed, unresolvedIndirects int, err error) {
	if mkdirErr := os.MkdirAll(options.OutputDir, 0o755); mkdirErr != nil {
		err = mkdirErr
		return
	}
	for _, bank := range repo.banks {
		bankResults, selected := results[bank.ID]
		if !selected {
			continue
		}
		source, composeErr := emitter.ComposeBank(bank.ID, bank.Config.Entries, bankResults, "")
		if composeErr != nil {
			err = composeErr
			return
		}
		for _, result := range bankResults {
			unresolvedIndirects += len(result.UnresolvedIndirects)
		}
		outputs, splitErr := splitBank(source, bank.ID, bank.Config.Entries, options.ChunkThresholdBytes, options.ChunkPCSpan)
		if splitErr != nil {
			err = splitErr
			return
		}
		wanted := make(map[string]struct{}, len(outputs))
		for name, content := range outputs {
			wanted[name] = struct{}{}
			wrote, writeErr := writeIfChanged(filepath.Join(options.OutputDir, name), content)
			if writeErr != nil {
				err = writeErr
				return
			}
			if wrote {
				changed++
			}
			files++
		}
		old, globErr := filepath.Glob(filepath.Join(options.OutputDir, fmt.Sprintf("bank%02x*_v2.c", bank.ID)))
		if globErr != nil {
			err = globErr
			return
		}
		for _, path := range old {
			if _, keep := wanted[filepath.Base(path)]; keep {
				continue
			}
			if removeErr := os.Remove(path); removeErr != nil {
				err = removeErr
				return
			}
			changed++
		}
	}

	if options.OnlyBanks == nil {
		for name, content := range map[string]string{
			"unresolved_stubs_v2.c": repo.unresolvedStubsSource(),
			"dispatch_v2.c":         repo.dispatchSource(),
		} {
			wrote, writeErr := writeIfChanged(filepath.Join(options.OutputDir, name), content)
			if writeErr != nil {
				err = writeErr
				return
			}
			if wrote {
				changed++
			}
			files++
		}
	}
	return
}

func splitBank(source string, bank byte, entries []config.Entry, thresholdBytes, pcSpan int) (map[string]string, error) {
	monoName := fmt.Sprintf("bank%02x_v2.c", bank)
	if pcSpan <= 0 || len([]byte(source)) < thresholdBytes {
		return map[string]string{monoName: source}, nil
	}
	matches := topLevelFunctionRE.FindAllStringSubmatchIndex(source, -1)
	if len(matches) == 0 {
		return map[string]string{monoName: source}, nil
	}
	preamble := source[:matches[0][0]]
	marker := strings.Index(preamble, forwardMarker)
	if marker < 0 {
		return nil, fmt.Errorf("bank $%02X: emitted source lacks forward-declaration marker", bank)
	}
	includePreamble := strings.TrimRight(preamble[:marker], " \t\r\n") + "\n\n"
	nameToPC := make(map[string]uint16)
	for _, entry := range entries {
		name := entry.Name
		if name == "" {
			name = fmt.Sprintf("bank_%02X_%04X", bank, entry.Start)
		}
		nameToPC[name] = entry.Start
	}
	chunks := make(map[int][]string)
	for index, match := range matches {
		end := len(source)
		if index+1 < len(matches) {
			end = matches[index+1][0]
		}
		body := strings.TrimSpace(source[match[0]:end]) + "\n"
		symbol := source[match[2]:match[3]]
		base := variantSuffixRE.ReplaceAllString(symbol, "")
		pc, found := nameToPC[base]
		if !found {
			synthetic := syntheticNameRE.FindStringSubmatch(base)
			if synthetic != nil {
				var parsedBank, parsedPC uint64
				fmt.Sscanf(synthetic[1], "%X", &parsedBank)
				fmt.Sscanf(synthetic[2], "%X", &parsedPC)
				if byte(parsedBank) == bank {
					pc, found = uint16(parsedPC), true
				}
			}
		}
		if !found {
			return nil, fmt.Errorf("bank $%02X: cannot assign emitted function %q to stable PC chunk", bank, symbol)
		}
		part := 0
		if pc >= 0x8000 {
			part = int(pc-0x8000) / pcSpan
		}
		chunks[part] = append(chunks[part], body)
	}
	outputs := make(map[string]string, len(chunks))
	for part, bodies := range chunks {
		joined := strings.Join(bodies, "\n")
		refsSet := make(map[string]struct{})
		for _, match := range variantCallRE.FindAllStringSubmatch(joined, -1) {
			refsSet[match[1]] = struct{}{}
		}
		refs := make([]string, 0, len(refsSet))
		for ref := range refsSet {
			refs = append(refs, ref)
		}
		sort.Strings(refs)
		var declarations strings.Builder
		for _, ref := range refs {
			fmt.Fprintf(&declarations, "RecompReturn %s(CpuState *cpu);\n", ref)
		}
		start := 0x8000 + part*pcSpan
		end := start + pcSpan - 1
		if end > 0xffff {
			end = 0xffff
		}
		header := fmt.Sprintf("/* Split translation unit: bank $%02X, part %02X; entry PCs $%04X-$%04X. */\n", bank, part, start, end)
		outputs[fmt.Sprintf("bank%02x_part%02x_v2.c", bank, part)] = includePreamble + header + "\n" + declarations.String() + "\n" + strings.TrimRight(joined, " \t\r\n") + "\n"
	}
	return outputs, nil
}

func writeIfChanged(path, content string) (bool, error) {
	old, err := os.ReadFile(path)
	if err == nil && string(old) == content {
		return false, nil
	}
	if err != nil && !os.IsNotExist(err) {
		return false, err
	}
	if mkdirErr := os.MkdirAll(filepath.Dir(path), 0o755); mkdirErr != nil {
		return false, mkdirErr
	}
	if writeErr := os.WriteFile(path, []byte(content), 0o644); writeErr != nil {
		return false, writeErr
	}
	return true, nil
}

func (repo *repository) unresolvedStubsSource() string {
	lines := []string{
		"/* Auto-generated by snesrecomp v2 v2_regen. Do NOT hand-edit.", " *", " * Stub bodies for Call targets that resolved to a ROM bank not", " * in the cfg set. These are typically data decoded as code", " * (garbled JSL operands). Real execution paths should never", " * reach them; each stub chains into cpu_trace_unresolved_stub_trap", " * so a runtime fire is captured (loud stderr line + TCP-queryable", " * snapshot via unresolved_stub_get) instead of silently returning.", " * One stub per (target, m, x) variant requested by the gen.", " *", " * Always emitted — file may be empty (no stubs needed) when", " * every (target, m, x) demand resolved within the cfg set.", " */", "", "#include \"cpu_state.h\"", "#include \"cpu_trace.h\"", "",
	}
	variants := make([]codegen.Variant, 0, len(repo.unresolved))
	for v := range repo.unresolved {
		variants = append(variants, v)
	}
	sort.Slice(variants, func(i, j int) bool {
		if variants[i].Address != variants[j].Address {
			return variants[i].Address < variants[j].Address
		}
		if variants[i].M != variants[j].M {
			return variants[i].M < variants[j].M
		}
		return variants[i].X < variants[j].X
	})
	for _, variant := range variants {
		name := fmt.Sprintf("bank_%02X_%04X_M%dX%d", byte(variant.Address>>16), uint16(variant.Address), variant.M&1, variant.X&1)
		lines = append(lines, fmt.Sprintf("RecompReturn %s(CpuState *cpu) { return cpu_trace_unresolved_stub_trap(cpu, 0x%06x, \"%s\"); }", name, variant.Address&0xffffff, name))
	}
	return strings.Join(lines, "\n") + "\n"
}

func (repo *repository) dispatchSource() string {
	variants := make(map[uint32]map[[2]uint8]struct{})
	for _, bank := range repo.banks {
		for _, entry := range bank.Config.Entries {
			address := uint32(bank.ID)<<16 | uint32(entry.Start)
			if variants[address] == nil {
				variants[address] = make(map[[2]uint8]struct{})
			}
			variants[address][[2]uint8{entry.EntryMX.M & 1, entry.EntryMX.X & 1}] = struct{}{}
		}
	}
	for variant := range repo.unresolved {
		if variants[variant.Address] == nil {
			variants[variant.Address] = make(map[[2]uint8]struct{})
		}
		variants[variant.Address][[2]uint8{variant.M & 1, variant.X & 1}] = struct{}{}
	}
	addresses := make([]uint32, 0, len(variants))
	for address := range variants {
		addresses = append(addresses, address)
	}
	sort.Slice(addresses, func(i, j int) bool { return addresses[i] < addresses[j] })
	lines := []string{
		"/* Auto-generated by snesrecomp v2 v2_regen. Do NOT hand-edit.", " *", " * PEI-trampoline dispatch table — runtime cpu_dispatch_pc() looks", " * up function entries here when an RTS/RTL on a trampoline-flagged", " * function hits the unbalanced-cpu->S branch in _emit_return.", " *", " * Sorted by pc24 for binary search. variant[] holds fnptrs for", " * (M0X0, M0X1, M1X0, M1X1) — NULL when that variant wasn't emitted.", " */", "", "#include \"cpu_state.h\"", "",
	}
	seen := make(map[string]struct{})
	for _, address := range addresses {
		base := repo.nameAt(address)
		for _, pair := range [][2]uint8{{0, 0}, {0, 1}, {1, 0}, {1, 1}} {
			if _, found := variants[address][pair]; !found {
				continue
			}
			name := fmt.Sprintf("%s_M%dX%d", base, pair[0], pair[1])
			if _, found := seen[name]; found {
				continue
			}
			seen[name] = struct{}{}
			lines = append(lines, fmt.Sprintf("RecompReturn %s(CpuState *cpu);", name))
		}
	}
	lines = append(lines, "", "const DispatchEntry g_dispatch_table[] = {")
	if len(addresses) == 0 {
		lines = append(lines, "    { 0xFFFFFFu, { NULL, NULL, NULL, NULL } },  /* sentinel — empty cfg */")
	}
	for _, address := range addresses {
		base := repo.nameAt(address)
		slots := [4]string{"NULL", "NULL", "NULL", "NULL"}
		for pair := range variants[address] {
			slots[int(pair[0]&1)<<1|int(pair[1]&1)] = fmt.Sprintf("%s_M%dX%d", base, pair[0]&1, pair[1]&1)
		}
		lines = append(lines, fmt.Sprintf("    { 0x%06Xu, { %s, %s, %s, %s } },  /* %s */", address&0xffffff, slots[0], slots[1], slots[2], slots[3], base))
	}
	lines = append(lines, "};", "", "const unsigned g_dispatch_table_count = (unsigned)(sizeof(g_dispatch_table) / sizeof(g_dispatch_table[0]));", "")
	return strings.Join(lines, "\n")
}

func (repo *repository) nameAt(address uint32) string {
	if name := repo.names[address&0xffffff]; name != "" {
		return name
	}
	return fmt.Sprintf("bank_%02X_%04X", byte(address>>16), uint16(address))
}
