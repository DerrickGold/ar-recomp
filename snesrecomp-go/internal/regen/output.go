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
	variantSuffixRE    = regexp.MustCompile(`_M[01]X[01]$`)
	syntheticNameRE    = regexp.MustCompile(`^bank_([0-9A-Fa-f]{2})_([0-9A-Fa-f]{4})$`)
	regionOwnerPCRE    = regexp.MustCompile(`/\*\s*resumable-region owner_pc:\$([0-9A-Fa-f]{4})\s*\*/`)
)

const forwardMarker = "/* Forward declarations for in-bank entries. */"

func (repo *repository) writeOutputs(options Options, results map[byte][]*emitter.FunctionResult) (files, changed, unresolvedIndirects int, oversized []string, err error) {
	if mkdirErr := os.MkdirAll(options.OutputDir, 0o755); mkdirErr != nil {
		err = mkdirErr
		return
	}
	symbols, symbolsErr := repo.generatedSymbols(results)
	if symbolsErr != nil {
		err = symbolsErr
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
		outputs, bankOversized, splitErr := splitBank(source, bank.ID, bank.Config.Entries,
			options.ChunkThresholdBytes, options.ChunkPCSpan, options.MaxUnitBytes, symbols)
		if splitErr != nil {
			err = splitErr
			return
		}
		oversized = append(oversized, bankOversized...)
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

// generatedSymbols is the declaration table for every generated symbol a unit
// may use: each bank's entry variants and void aliases (every bank, so a
// partial regeneration still declares the entries of banks it did not
// re-emit), the unresolved trap stubs, and every definition this run emitted,
// whose own declarators fix the shared-region helpers' linkage and parameters.
func (repo *repository) generatedSymbols(results map[byte][]*emitter.FunctionResult) (*emitter.SymbolTable, error) {
	symbols := emitter.NewSymbolTable()
	for _, bank := range repo.banks {
		if err := symbols.DeclareBankEntries(bank.ID, bank.Config.Entries); err != nil {
			return nil, err
		}
	}
	for variant := range repo.unresolved {
		if err := symbols.DeclareVariant(fmt.Sprintf("bank_%02X_%04X_M%dX%d",
			byte(variant.Address>>16), uint16(variant.Address), variant.M&1, variant.X&1)); err != nil {
			return nil, err
		}
	}
	for _, bank := range repo.banks {
		for _, result := range results[bank.ID] {
			if result == nil {
				continue
			}
			if err := symbols.DeclareDefinitions(result.Source); err != nil {
				return nil, err
			}
		}
	}
	return symbols, nil
}

// splitBank partitions one composed bank into translation units. A bank below
// thresholdBytes (and within unitLimit) stays one unit. Otherwise its
// functions go to stable pcSpan-wide PC chunks, and a chunk whose function
// source exceeds unitLimit is divided further (see divideUnit). Functions
// move between units whole: every member of a resumable region carries its
// owner's PC, so a region's private helper always shares a unit with its
// wrappers. It returns the units by file name and a description of every unit
// that stays over the limit because it cannot be divided.
func splitBank(source string, bank byte, entries []config.Entry, thresholdBytes, pcSpan, unitLimit int, symbols *emitter.SymbolTable) (map[string]string, []string, error) {
	monoName := fmt.Sprintf("bank%02x_v2.c", bank)
	if pcSpan <= 0 || (len(source) < thresholdBytes && (unitLimit <= 0 || len(source) <= unitLimit)) {
		return map[string]string{monoName: declareReferencedVariants(source, symbols)}, nil, nil
	}
	matches := topLevelFunctionRE.FindAllStringSubmatchIndex(source, -1)
	if len(matches) == 0 {
		return map[string]string{monoName: source}, nil, nil
	}
	preamble := source[:matches[0][0]]
	marker := strings.Index(preamble, forwardMarker)
	if marker < 0 {
		return nil, nil, fmt.Errorf("bank $%02X: emitted source lacks forward-declaration marker", bank)
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
	chunks := make(map[int][]unitBody)
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
			return nil, nil, fmt.Errorf("bank $%02X: cannot assign emitted function %q to stable PC chunk", bank, symbol)
		}
		// Every wrapper for one resumable region must remain in its owner's
		// translation unit because the shared body has private static-inline
		// linkage. The marker is emitted only from proven region wrappers.
		region := false
		if owner := regionOwnerPCRE.FindStringSubmatch(body); owner != nil {
			var parsed uint64
			if _, scanErr := fmt.Sscanf(owner[1], "%X", &parsed); scanErr != nil {
				return nil, nil, fmt.Errorf("bank $%02X: parse resumable-region owner PC for %q: %w", bank, symbol, scanErr)
			}
			pc, region = uint16(parsed), true
		}
		part := 0
		if pc >= 0x8000 {
			part = int(pc-0x8000) / pcSpan
		}
		chunks[part] = append(chunks[part], unitBody{pc: int(pc), symbol: symbol, region: region, text: body})
	}
	parts := make([]int, 0, len(chunks))
	for part := range chunks {
		parts = append(parts, part)
	}
	sort.Ints(parts)
	outputs := make(map[string]string, len(chunks))
	var oversized []string
	unit := func(name, header string, bodies []unitBody) {
		texts := make([]string, len(bodies))
		for index, body := range bodies {
			texts[index] = body.text
		}
		joined := strings.Join(texts, "\n")
		outputs[name] = includePreamble + header + "\n" + symbols.Declarations(joined) + "\n" + strings.TrimRight(joined, " \t\r\n") + "\n"
		// Only a lone function or region can remain over the limit.
		if size := unitBytes(bodies); unitLimit > 0 && size > unitLimit {
			what := fmt.Sprintf("the single function %s", bodies[0].symbol)
			if bodies[0].region {
				what = fmt.Sprintf("the resumable region owned by $%04X (%d functions and its shared body)", bodies[0].pc, len(bodies))
			}
			oversized = append(oversized, fmt.Sprintf(
				"%s holds %d KiB of function source, over the %d KiB unit limit: it is %s, which cannot be divided",
				name, size/1024, unitLimit/1024, what))
		}
	}
	for _, part := range parts {
		bodies := chunks[part]
		start := 0x8000 + part*pcSpan
		end := min(start+pcSpan-1, 0xffff)
		// Part 0 also holds any code below $8000; division always sends it
		// to the lowest piece, so the range stays aligned.
		pieces := divideUnit(bodies, start, end, unitLimit)
		if len(pieces) == 1 {
			unit(fmt.Sprintf("bank%02x_part%02x_v2.c", bank, part),
				fmt.Sprintf("/* Split translation unit: bank $%02X, part %02X; entry PCs $%04X-$%04X. */\n", bank, part, start, end),
				bodies)
			continue
		}
		for _, piece := range pieces {
			name := fmt.Sprintf("bank%02x_part%02x_%04x", bank, part, piece.low)
			header := fmt.Sprintf("/* Split translation unit: bank $%02X, part %02X; entry PCs $%04X-$%04X. */\n",
				bank, part, piece.low, piece.high)
			if piece.count > 1 {
				name += fmt.Sprintf("_%02d", piece.ordinal)
				header = fmt.Sprintf("/* Split translation unit: bank $%02X, part %02X; entry PC $%04X, piece %d of %d. */\n",
					bank, part, piece.low, piece.ordinal+1, piece.count)
			}
			unit(name+"_v2.c", header, piece.bodies)
		}
	}
	return outputs, oversized, nil
}

// unitBody is one emitted top-level function with any shared-region helper
// that follows it, at its effective PC: its entry's PC, or its resumable
// region owner's.
type unitBody struct {
	pc     int
	symbol string
	region bool
	text   string
}

type unitPiece struct {
	low, high int
	// ordinal and count number the pieces of one PC whose functions alone
	// exceed the limit; count is 1 otherwise.
	ordinal, count int
	bodies         []unitBody
}

func unitBytes(bodies []unitBody) int {
	size := 0
	for _, body := range bodies {
		size += len(body.text)
	}
	return size
}

// divideUnit splits bodies, whose effective PCs lie in [low, high] (a PC
// below low counts as low), into pieces of at most limit bytes of function
// source. It halves the PC range at fixed, aligned points rather than packing
// to the limit, so a local edit moves no boundary unless it changes whether a
// range fits. A range narrowed to one PC holding more than limit bytes is
// packed greedily, keeping each resumable region whole. A single function or
// region larger than limit is left as a piece of its own. Bodies keep their
// emitted order in every piece, except that one PC's region members are
// gathered when that PC is packed.
func divideUnit(bodies []unitBody, low, high, limit int) []unitPiece {
	if limit <= 0 || unitBytes(bodies) <= limit {
		return []unitPiece{{low: low, high: high, count: 1, bodies: bodies}}
	}
	if low == high {
		return packUnitAtoms(bodies, low, limit)
	}
	middle := low + (high-low+1)/2
	var lower, upper []unitBody
	for _, body := range bodies {
		if body.pc < middle {
			lower = append(lower, body)
		} else {
			upper = append(upper, body)
		}
	}
	var pieces []unitPiece
	if len(lower) > 0 {
		pieces = append(pieces, divideUnit(lower, low, middle-1, limit)...)
	}
	if len(upper) > 0 {
		pieces = append(pieces, divideUnit(upper, middle, high, limit)...)
	}
	return pieces
}

// packUnitAtoms splits the functions of one effective PC. Its resumable
// region, if any, is one atom; every other function is an atom of its own.
func packUnitAtoms(bodies []unitBody, pc, limit int) []unitPiece {
	var atoms [][]unitBody
	regionAtom := -1
	for _, body := range bodies {
		if body.region {
			if regionAtom < 0 {
				regionAtom = len(atoms)
				atoms = append(atoms, nil)
			}
			atoms[regionAtom] = append(atoms[regionAtom], body)
			continue
		}
		atoms = append(atoms, []unitBody{body})
	}
	var pieces []unitPiece
	var current []unitBody
	for _, atom := range atoms {
		if len(current) > 0 && unitBytes(current)+unitBytes(atom) > limit {
			pieces = append(pieces, unitPiece{low: pc, high: pc, bodies: current})
			current = nil
		}
		current = append(current, atom...)
	}
	pieces = append(pieces, unitPiece{low: pc, high: pc, bodies: current})
	for index := range pieces {
		pieces[index].ordinal, pieces[index].count = index, len(pieces)
	}
	return pieces
}

// declareReferencedVariants forward-declares every generated symbol an
// unsplit bank uses. ComposeBank declares only one variant per configured
// entry in this bank, so without this a mono translation unit would use
// undeclared functions in ordinary cases: a cross-bank JSL target, an in-bank
// variant whose definition follows the call site, a shared-region helper. The
// split path derives the same set per chunk; this keeps both shapes
// equivalent.
//
// Declarations are emitted for every used symbol without checking whether this
// file also defines it. A redundant declaration ahead of a definition is valid
// C, and filtering on "defined somewhere in this file" would drop exactly the
// forward references that need declaring.
func declareReferencedVariants(source string, symbols *emitter.SymbolTable) string {
	matches := topLevelFunctionRE.FindAllStringSubmatchIndex(source, -1)
	if len(matches) == 0 {
		return source
	}
	insert := matches[0][0]
	declarations := symbols.Declarations(source[insert:])
	if declarations == "" {
		return source
	}
	return source[:insert] +
		"/* Forward declarations for referenced entries. */\n" +
		declarations + "\n" + source[insert:]
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
		"/* Auto-generated by snesrecomp v2 v2_regen. Do NOT hand-edit.", " *", " * Trap bodies for emitted calls that cannot have a generated body:", " * invalid LoROM addresses, banks outside the cfg set, or targets in", " * an explicit data_region. Real execution paths should not reach", " * them; each stub reports the target once instead of leaving an", " * undefined symbol or returning silently.", " * One stub per (target, m, x) variant requested by emitted code.", " *", " * Always emitted — file may be empty (no stubs needed) when", " * every emitted (target, m, x) demand resolved within the cfg set.", " */", "", "#include \"snesrecomp/game/cpu.h\"", "#include \"snesrecomp/game/trace.h\"", "",
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
		"/* Auto-generated by snesrecomp v2 v2_regen. Do NOT hand-edit.", " *", " * PEI-trampoline dispatch table — runtime cpu_dispatch_pc() looks", " * up function entries here when an RTS/RTL on a trampoline-flagged", " * function hits the unbalanced-cpu->S branch in _emit_return.", " *", " * Sorted by pc24 for binary search. variant[] holds fnptrs for", " * (M0X0, M0X1, M1X0, M1X1) — NULL when that variant wasn't emitted.", " */", "", "#include \"snesrecomp/game/cpu.h\"", "",
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
