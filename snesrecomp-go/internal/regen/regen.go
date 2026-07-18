// Package regen orchestrates the standalone Go decoder/emitter over a repo.
package regen

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/emitter"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

type Options struct {
	ROMPath, ConfigDir, OutputDir string
	Jobs                          int
	ChunkThresholdBytes           int
	ChunkPCSpan                   int
	OnlyBanks                     map[byte]struct{}
	AllowStubs                    bool
	Progress                      func(string, ...any)
}

type Report struct {
	Banks, InitialEntries, FinalEntries int
	Functions, Files, ChangedFiles      int
	Passes, ExitMXRoutes                int
	UnresolvedIndirects, StubHits       int
	Elapsed                             time.Duration
}

type bankState struct {
	ID     byte
	Path   string
	Config *config.Config
}

type repository struct {
	image            rom.Image
	banks            []*bankState
	byBank           map[byte]*bankState
	names            map[uint32]string
	canonical        map[uint32]map[[2]uint8]struct{}
	dispatchHelpers  map[uint32]string
	exitMX           map[decoder.Variant]decoder.MX
	allDataRegions   []decoder.DataRegion
	forceVariants    map[uint32][2]uint8
	validVariants    map[uint32]map[[2]uint8]struct{}
	provenEquivalent map[uint32]map[[2]uint8]map[[2]uint8]struct{}
	unresolved       map[codegen.Variant]struct{}
	cumulativeDirty  map[codegen.Variant]struct{}
	cumulativeEmit   map[codegen.Variant]struct{}
	cumulativePrune  map[codegen.Variant]struct{}
}

var bankConfigRE = regexp.MustCompile(`(?i)^bank([0-9a-f]+)\.cfg$`)

func Run(options Options) (Report, error) {
	started := time.Now()
	if options.Jobs <= 0 {
		options.Jobs = runtime.NumCPU()
	}
	if options.ChunkThresholdBytes == 0 {
		options.ChunkThresholdBytes = 4 * 1024 * 1024
	}
	if options.ChunkPCSpan == 0 {
		options.ChunkPCSpan = 0x800
	}
	logf := options.Progress
	if logf == nil {
		logf = func(string, ...any) {}
	}

	repo, err := loadRepository(options.ROMPath, options.ConfigDir)
	if err != nil {
		return Report{}, err
	}
	report := Report{Banks: len(repo.banks)}
	for _, bank := range repo.banks {
		report.InitialEntries += len(bank.Config.Entries)
	}
	logf("loaded %d banks and %d cfg entries", report.Banks, report.InitialEntries)

	repo.expandAutoVectors(logf)
	repo.promoteCrossBankNames()
	repo.rebuildNames()
	repo.discoverDispatchHelpers(options.Jobs, logf)

	for pass := 0; pass < 24; pass++ {
		report.Passes = pass + 1
		added, decodeErr := repo.discoverVariants(options.Jobs)
		if decodeErr != nil {
			return report, decodeErr
		}
		exitChanged, routeCount := repo.inferExitMX(options.Jobs)
		report.ExitMXRoutes = routeCount
		logf("fixpoint %d: added %d variants; %d exit-M/X routes%s", pass+1, added, routeCount, map[bool]string{true: " changed", false: ""}[exitChanged])
		if added == 0 && !exitChanged {
			break
		}
		if pass == 23 {
			return report, fmt.Errorf("variant/exit-MX fixpoint did not converge in 24 passes")
		}
	}

	for _, bank := range repo.banks {
		report.FinalEntries += len(bank.Config.Entries)
	}
	logf("emitting %d function variants with %d workers", report.FinalEntries, options.Jobs)
	var results map[byte][]*emitter.FunctionResult
	var contexts []*codegen.Context
	for prunePass := 0; prunePass < 8; prunePass++ {
		results, contexts, err = repo.emitFunctions(options.Jobs, options.OnlyBanks)
		if err != nil {
			return report, err
		}
		equivalencesAdded := repo.mergeEquivalences(results)
		pruned := repo.pruneDirtyVariants(results)
		if pruned == 0 && equivalencesAdded == 0 {
			break
		}
		logf("emit-truth prune %d: removed %d wrong-width variants; learned %d equivalences; re-emitting", prunePass+1, pruned, equivalencesAdded)
		if prunePass == 7 {
			return report, fmt.Errorf("variant prune did not converge in 8 passes")
		}
	}
	report.FinalEntries, report.Functions = 0, 0
	for _, bank := range repo.banks {
		report.FinalEntries += len(bank.Config.Entries)
	}
	for _, bankResults := range results {
		report.Functions += len(bankResults)
	}
	for _, context := range contexts {
		for demand := range context.Demands {
			if repo.byBank[canonicalBank(repo.byBank, byte(demand.Address>>16))] == nil {
				repo.unresolved[demand] = struct{}{}
			}
		}
	}

	files, changed, unresolved, err := repo.writeOutputs(options, results)
	if err != nil {
		return report, err
	}
	report.Files, report.ChangedFiles = files, changed
	report.UnresolvedIndirects = unresolved
	report.StubHits, err = lintStubs(options.OutputDir)
	report.Elapsed = time.Since(started)
	if err != nil {
		return report, err
	}
	if report.StubHits == 0 {
		stamp := filepath.Join(options.OutputDir, ".v2_regen_stamp")
		if touchErr := os.WriteFile(stamp, nil, 0o644); touchErr != nil {
			return report, touchErr
		}
	} else if !options.AllowStubs {
		return report, fmt.Errorf("stub lint found %d emitted stub marker(s)", report.StubHits)
	}
	return report, nil
}

func loadRepository(romPath, configDir string) (*repository, error) {
	image, err := rom.Load(romPath)
	if err != nil {
		return nil, err
	}
	paths, err := filepath.Glob(filepath.Join(configDir, "bank*.cfg"))
	if err != nil {
		return nil, err
	}
	sort.Strings(paths)
	if len(paths) == 0 {
		return nil, fmt.Errorf("no bank*.cfg under %s", configDir)
	}
	repo := &repository{
		image: image, byBank: make(map[byte]*bankState), names: make(map[uint32]string),
		canonical: make(map[uint32]map[[2]uint8]struct{}), dispatchHelpers: make(map[uint32]string),
		exitMX: make(map[decoder.Variant]decoder.MX), forceVariants: make(map[uint32][2]uint8),
		validVariants: make(map[uint32]map[[2]uint8]struct{}), unresolved: make(map[codegen.Variant]struct{}),
		provenEquivalent: make(map[uint32]map[[2]uint8]map[[2]uint8]struct{}),
		cumulativeDirty:  make(map[codegen.Variant]struct{}), cumulativeEmit: make(map[codegen.Variant]struct{}),
		cumulativePrune: make(map[codegen.Variant]struct{}),
	}
	for _, path := range paths {
		match := bankConfigRE.FindStringSubmatch(filepath.Base(path))
		if match == nil {
			continue
		}
		value, parseErr := strconv.ParseUint(match[1], 16, 8)
		if parseErr != nil {
			return nil, parseErr
		}
		cfg, loadErr := config.Load(path)
		if loadErr != nil {
			return nil, loadErr
		}
		bank := byte(value)
		state := &bankState{ID: bank, Path: path, Config: cfg}
		repo.banks = append(repo.banks, state)
		repo.byBank[bank] = state
		for _, entry := range cfg.Entries {
			address := decoder.Address24(bank, entry.Start)
			if repo.canonical[address] == nil {
				repo.canonical[address] = make(map[[2]uint8]struct{})
			}
			repo.canonical[address][[2]uint8{entry.EntryMX.M & 1, entry.EntryMX.X & 1}] = struct{}{}
		}
		for _, region := range cfg.DataRegions {
			repo.allDataRegions = append(repo.allDataRegions, decoder.DataRegion{Bank: region.Bank, Start: region.Start, End: region.End})
		}
		for site, mx := range cfg.ForceVariantAt {
			if _, found := repo.forceVariants[site&0xffffff]; !found {
				repo.forceVariants[site&0xffffff] = [2]uint8{mx.M & 1, mx.X & 1}
			}
		}
	}
	repo.rebuildNames()
	return repo, nil
}

func (repo *repository) expandAutoVectors(logf func(string, ...any)) {
	bank := repo.byBank[0]
	if bank == nil || !bank.Config.AutoVectors || len(repo.image) < 0x8000 {
		return
	}
	read := func(offset int) uint16 {
		return uint16(repo.image[0x7fe0+offset]) | uint16(repo.image[0x7fe0+offset+1])<<8
	}
	seeds := []struct {
		name string
		pc   uint16
	}{{"I_RESET", read(0x1c)}, {"I_NMI", read(0x0a)}, {"I_IRQ", read(0x0e)}}
	starts, names := map[uint16]struct{}{}, map[string]struct{}{}
	for _, entry := range bank.Config.Entries {
		starts[entry.Start] = struct{}{}
		names[entry.Name] = struct{}{}
	}
	for _, seed := range seeds {
		if seed.pc == 0 || seed.pc == 0xffff {
			continue
		}
		if _, found := starts[seed.pc]; found {
			continue
		}
		if _, found := names[seed.name]; found {
			continue
		}
		bank.Config.Entries = append(bank.Config.Entries, config.Entry{Name: seed.name, Start: seed.pc, EntryMX: config.MX{M: 1, X: 1}})
		logf("auto_vectors: added %s at $00:%04X", seed.name, seed.pc)
	}
}

func (repo *repository) promoteCrossBankNames() {
	claimed := make(map[string]struct{})
	for _, bank := range repo.banks {
		for _, entry := range bank.Config.Entries {
			if entry.Name != "" {
				claimed[entry.Name] = struct{}{}
			}
		}
	}
	for _, declaring := range repo.banks {
		for _, declaration := range declaring.Config.Names {
			target := repo.byBank[byte(declaration.Address>>16)]
			if target == nil {
				continue
			}
			present := false
			for _, entry := range target.Config.Entries {
				if entry.Start == uint16(declaration.Address) || entry.Name == declaration.Name {
					present = true
					break
				}
			}
			if _, used := claimed[declaration.Name]; used {
				present = true
			}
			if !present {
				target.Config.Entries = append(target.Config.Entries, config.Entry{Name: declaration.Name, Start: uint16(declaration.Address), EntryMX: config.MX{M: 1, X: 1}})
				claimed[declaration.Name] = struct{}{}
			}
		}
	}
}

func (repo *repository) rebuildNames() {
	repo.names = make(map[uint32]string)
	for _, bank := range repo.banks {
		for _, entry := range bank.Config.Entries {
			if entry.Name != "" {
				repo.names[decoder.Address24(bank.ID, entry.Start)] = entry.Name
			}
		}
		for _, declaration := range bank.Config.Names {
			if declaration.Name != "" {
				repo.names[declaration.Address&0xffffff] = declaration.Name
			}
		}
	}
	for address, name := range cloneNames(repo.names) {
		bank := byte(address >> 16)
		if bank < 0x40 || (bank >= 0x80 && bank < 0xc0) {
			mirror := address ^ 0x800000
			if _, exists := repo.names[mirror]; !exists {
				repo.names[mirror] = name
			}
		}
	}
}

func cloneNames(source map[uint32]string) map[uint32]string {
	result := make(map[uint32]string, len(source))
	for k, v := range source {
		result[k] = v
	}
	return result
}

func (repo *repository) discoverDispatchHelpers(jobs int, logf func(string, ...any)) {
	targets := make(map[uint32]struct{})
	var lock sync.Mutex
	repo.parallelEntries(jobs, nil, func(bank *bankState, entry config.Entry) {
		options := repo.decodeOptions(bank, entry.Start)
		options.DispatchHelpers = nil
		graph, err := decoder.DecodeFunction(repo.image, bank.ID, entry.Start, entry.EntryMX.M, entry.EntryMX.X, options)
		if err != nil {
			return
		}
		lock.Lock()
		defer lock.Unlock()
		for _, decoded := range graph.Instructions {
			ins := decoded.Instruction
			if ins.Mnemonic == "JSL" || (ins.Mnemonic == "JMP" && ins.Mode == cpu65816.LONG) {
				targets[ins.Operand&0xffffff] = struct{}{}
			}
		}
	})
	short, long := 0, 0
	for target := range targets {
		kind := decoder.ClassifyDispatchHelper(repo.image, byte(target>>16), uint16(target))
		if kind != "" {
			repo.dispatchHelpers[target] = kind
			if kind == "short" {
				short++
			} else {
				long++
			}
		}
	}
	logf("dispatch helpers: %d short, %d long (%d call targets scanned)", short, long, len(targets))
}

func (repo *repository) discoverVariants(jobs int) (int, error) {
	totalAdded := 0
	for round := 0; round < 32; round++ {
		demands := make(map[codegen.Variant]struct{})
		var lock sync.Mutex
		var firstErr error
		repo.parallelEntries(jobs, nil, func(bank *bankState, entry config.Entry) {
			options := repo.decodeOptions(bank, entry.Start)
			graph, err := decoder.DecodeFunction(repo.image, bank.ID, entry.Start, entry.EntryMX.M, entry.EntryMX.X, options)
			if err != nil {
				return
			}
			starts := make(map[uint16]struct{})
			for _, sibling := range bank.Config.Entries {
				starts[sibling.Start] = struct{}{}
			}
			local := discoverGraphDemands(graph, starts)
			lock.Lock()
			for demand := range local {
				demands[demand] = struct{}{}
			}
			if err != nil && firstErr == nil {
				firstErr = err
			}
			lock.Unlock()
		})
		if firstErr != nil {
			return totalAdded, firstErr
		}
		added := repo.applyDemands(demands)
		totalAdded += added
		if added == 0 {
			return totalAdded, nil
		}
	}
	return totalAdded, fmt.Errorf("variant discovery exceeded 32 rounds")
}

func discoverGraphDemands(graph *decoder.Graph, siblingStarts map[uint16]struct{}) map[codegen.Variant]struct{} {
	result := make(map[codegen.Variant]struct{})
	all := func(address uint32) {
		for m := uint8(0); m < 2; m++ {
			for x := uint8(0); x < 2; x++ {
				result[codegen.Variant{Address: address & 0xffffff, M: m, X: x}] = struct{}{}
			}
		}
	}
	for _, decoded := range graph.Instructions {
		ins := decoded.Instruction
		if len(ins.DispatchEntries) > 0 {
			if ins.DispatchKind == "rts_trick" {
				continue
			}
			for _, target := range ins.DispatchEntries {
				if target == 0 {
					continue
				}
				if ins.DispatchKind != "long" {
					target = uint32(byte(ins.Address>>16))<<16 | target&0xffff
				}
				if (ins.Mnemonic == "JSL" || (ins.Mnemonic == "JMP" && ins.Mode == cpu65816.LONG)) && ins.DispatchIndexReg == "" {
					result[codegen.Variant{Address: target, M: 1, X: 1}] = struct{}{}
				} else {
					all(target)
				}
			}
			continue
		}
		switch {
		case ins.Mnemonic == "JSR" && ins.Mode == cpu65816.ABS:
			all(uint32(byte(ins.Address>>16))<<16 | ins.Operand&0xffff)
		case ins.Mnemonic == "JSL":
			all(ins.Operand)
		case ins.Mnemonic == "JMP" && ins.Mode == cpu65816.LONG:
			all(ins.Operand)
		}
		for _, successor := range decoded.Successors {
			if _, known := siblingStarts[uint16(successor.PC)]; !known {
				continue
			}
			if graph.Instructions[successor] != nil {
				continue
			}
			result[codegen.Variant{Address: successor.PC & 0xffffff, M: successor.M & 1, X: successor.X & 1}] = struct{}{}
		}
	}
	return result
}

func (repo *repository) applyDemands(demands map[codegen.Variant]struct{}) int {
	keys := make([]codegen.Variant, 0, len(demands))
	for demand := range demands {
		keys = append(keys, demand)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].Address != keys[j].Address {
			return keys[i].Address < keys[j].Address
		}
		if keys[i].M != keys[j].M {
			return keys[i].M < keys[j].M
		}
		return keys[i].X < keys[j].X
	})
	added := 0
	for _, demand := range keys {
		address := demand.Address & 0xffffff
		pc := uint16(address)
		if pc < 0x8000 || int(byte(address>>16)&0x7f)*0x8000+int(pc-0x8000) >= len(repo.image) {
			continue
		}
		bankID := canonicalBank(repo.byBank, byte(address>>16))
		bank := repo.byBank[bankID]
		if bank == nil {
			repo.unresolved[demand] = struct{}{}
			continue
		}
		if repo.inDataRegion(bankID, pc) {
			continue
		}
		found, base := false, (*config.Entry)(nil)
		for index := range bank.Config.Entries {
			entry := &bank.Config.Entries[index]
			if entry.Start != pc {
				continue
			}
			if base == nil {
				base = entry
			}
			if entry.EntryMX.M&1 == demand.M&1 && entry.EntryMX.X&1 == demand.X&1 {
				found = true
				break
			}
		}
		if found {
			continue
		}
		entry := config.Entry{Name: fmt.Sprintf("bank_%02X_%04X", bankID, pc), Start: pc, EntryMX: config.MX{M: demand.M & 1, X: demand.X & 1}}
		if base != nil {
			entry = *base
			entry.EntryMX = config.MX{M: demand.M & 1, X: demand.X & 1}
		}
		bank.Config.Entries = append(bank.Config.Entries, entry)
		added++
	}
	if added > 0 {
		repo.rebuildNames()
	}
	return added
}

func canonicalBank(banks map[byte]*bankState, bank byte) byte {
	if banks[bank] != nil {
		return bank
	}
	if bank < 0x40 || (bank >= 0x80 && bank < 0xc0) {
		if banks[bank^0x80] != nil {
			return bank ^ 0x80
		}
	}
	return bank
}

func (repo *repository) inDataRegion(bank byte, pc uint16) bool {
	for _, r := range repo.allDataRegions {
		if r.Bank == bank && pc >= r.Start && pc < r.End {
			return true
		}
	}
	return false
}

func (repo *repository) inferExitMX(jobs int) (bool, int) {
	manual := make(map[decoder.Variant]decoder.MX)
	for _, bank := range repo.banks {
		for _, declaration := range bank.Config.ExitMXAt {
			for m := uint8(0); m < 2; m++ {
				for x := uint8(0); x < 2; x++ {
					manual[decoder.Variant{Address: declaration.Address & 0xffffff, M: m, X: x}] = decoder.MX{M: int8(declaration.Exit.M & 1), X: int8(declaration.Exit.X & 1)}
				}
			}
		}
	}
	current := cloneExitMap(repo.exitMX)
	for key, value := range manual {
		current[key] = value
	}
	for iteration := 0; iteration < 12; iteration++ {
		next := cloneExitMap(manual)
		var lock sync.Mutex
		repo.parallelEntries(jobs, nil, func(bank *bankState, entry config.Entry) {
			key := decoder.Variant{Address: decoder.Address24(bank.ID, entry.Start), M: entry.EntryMX.M & 1, X: entry.EntryMX.X & 1}
			if _, fixed := manual[key]; fixed {
				return
			}
			options := repo.decodeOptions(bank, entry.Start)
			options.CalleeExitMX = current
			graph, err := decoder.DecodeFunction(repo.image, bank.ID, entry.Start, entry.EntryMX.M, entry.EntryMX.X, options)
			if err != nil {
				return
			}
			exit := decoder.AnalyzeExitMX(graph, current)
			if exit.M < 0 || exit.X < 0 {
				return
			}
			if uint8(exit.M)&1 == entry.EntryMX.M&1 && uint8(exit.X)&1 == entry.EntryMX.X&1 {
				return
			}
			lock.Lock()
			next[key] = exit
			lock.Unlock()
		})
		if exitMapsEqual(current, next) {
			current = next
			break
		}
		current = next
	}
	changed := !exitMapsEqual(repo.exitMX, current)
	repo.exitMX = current
	return changed, len(current)
}

func cloneExitMap(source map[decoder.Variant]decoder.MX) map[decoder.Variant]decoder.MX {
	result := make(map[decoder.Variant]decoder.MX, len(source))
	for k, v := range source {
		result[k] = v
	}
	return result
}
func exitMapsEqual(a, b map[decoder.Variant]decoder.MX) bool {
	if len(a) != len(b) {
		return false
	}
	for k, v := range a {
		if b[k] != v {
			return false
		}
	}
	return true
}

func (repo *repository) decodeOptions(bank *bankState, start uint16) decoder.Options {
	options := emitter.DecodeOptionsFromConfig(bank.ID, bank.Config)
	options.DispatchHelpers = repo.dispatchHelpers
	options.DataRegions = repo.allDataRegions
	options.CalleeExitMX = repo.exitMX
	options.SiblingEntryPCs = make(map[uint16]struct{})
	for _, entry := range bank.Config.Entries {
		if entry.Start != start {
			options.SiblingEntryPCs[entry.Start] = struct{}{}
		}
	}
	return options
}

type entryTask struct {
	bank  *bankState
	index int
	entry config.Entry
}

func (repo *repository) parallelEntries(jobs int, only map[byte]struct{}, fn func(*bankState, config.Entry)) {
	if jobs < 1 {
		jobs = 1
	}
	tasks := make(chan entryTask)
	var wait sync.WaitGroup
	for worker := 0; worker < jobs; worker++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			for task := range tasks {
				fn(task.bank, task.entry)
			}
		}()
	}
	for _, bank := range repo.banks {
		if only != nil {
			if _, selected := only[bank.ID]; !selected {
				continue
			}
		}
		for index, entry := range bank.Config.Entries {
			tasks <- entryTask{bank, index, entry}
		}
	}
	close(tasks)
	wait.Wait()
}

func (repo *repository) emitFunctions(jobs int, only map[byte]struct{}) (map[byte][]*emitter.FunctionResult, []*codegen.Context, error) {
	results := make(map[byte][]*emitter.FunctionResult)
	for _, bank := range repo.banks {
		if only == nil || containsBank(only, bank.ID) {
			results[bank.ID] = make([]*emitter.FunctionResult, len(bank.Config.Entries))
		}
	}
	contexts := make([]*codegen.Context, 0)
	var lock sync.Mutex
	var firstErr error
	repo.parallelEntries(jobs, only, func(bank *bankState, entry config.Entry) {
		index := findEntryIndex(bank.Config.Entries, entry)
		context := codegen.NewContext()
		context.ROMSize, context.Names = len(repo.image), repo.names
		context.CanonicalVariants, context.ForceVariantAt = repo.canonical, repo.forceVariants
		context.ValidVariants = repo.validVariants
		context.ProvenEquivalent = repo.provenEquivalent
		options := repo.decodeOptions(bank, entry.Start)
		excludes := make([][2]uint16, 0, len(bank.Config.ExcludeRanges))
		for _, r := range bank.Config.ExcludeRanges {
			excludes = append(excludes, [2]uint16{r.Start, r.End})
		}
		_, hleSPC := uint16Set(bank.Config.HLESPCUpload)[entry.Start]
		var exit *decoder.MX
		if value, found := repo.exitMX[decoder.Variant{Address: decoder.Address24(bank.ID, entry.Start), M: entry.EntryMX.M & 1, X: entry.EntryMX.X & 1}]; found {
			copy := value
			exit = &copy
		}
		result, err := emitter.EmitFunction(repo.image, bank.ID, entry.Start, entry.EntryMX.M, entry.EntryMX.X, emitter.FunctionOptions{
			Name: entry.Name, End: entry.End, EntrySOffset: entry.EntrySOffset, Decode: options, Codegen: context,
			ExcludeRanges: excludes, TailCallPC: entry.TailCallPC, HLESPCUpload: hleSPC,
			HLEFunction: bank.Config.HLEFunctions[entry.Start], HLEDispatch: bank.Config.HLEDispatch,
			ExitMX: exit, UnresolvedAllowed: true,
		})
		lock.Lock()
		defer lock.Unlock()
		if err != nil {
			if firstErr == nil {
				firstErr = fmt.Errorf("emit bank $%02X entry $%04X M%dX%d: %w", bank.ID, entry.Start, entry.EntryMX.M&1, entry.EntryMX.X&1, err)
			}
			return
		}
		if index < 0 || index >= len(results[bank.ID]) {
			if firstErr == nil {
				firstErr = fmt.Errorf("internal entry ordering failure at $%02X:%04X", bank.ID, entry.Start)
			}
			return
		}
		results[bank.ID][index] = result
		contexts = append(contexts, context)
	})
	return results, contexts, firstErr
}

func findEntryIndex(entries []config.Entry, target config.Entry) int {
	for index, entry := range entries {
		if entry.Start == target.Start && entry.EntryMX == target.EntryMX {
			return index
		}
	}
	return -1
}
func containsBank(set map[byte]struct{}, bank byte) bool { _, found := set[bank]; return found }
func uint16Set(values []uint16) map[uint16]struct{} {
	result := make(map[uint16]struct{}, len(values))
	for _, value := range values {
		result[value] = struct{}{}
	}
	return result
}

type emittedVariant struct{ result *emitter.FunctionResult }

func (repo *repository) mergeEquivalences(results map[byte][]*emitter.FunctionResult) int {
	added := 0
	for _, bankResults := range results {
		for _, result := range bankResults {
			if result == nil {
				continue
			}
			for _, equivalence := range result.Equivalences {
				address := equivalence.Address & 0xffffff
				if repo.provenEquivalent[address] == nil {
					repo.provenEquivalent[address] = make(map[[2]uint8]map[[2]uint8]struct{})
				}
				if repo.provenEquivalent[address][equivalence.From] == nil {
					repo.provenEquivalent[address][equivalence.From] = make(map[[2]uint8]struct{})
				}
				if _, found := repo.provenEquivalent[address][equivalence.From][equivalence.To]; !found {
					repo.provenEquivalent[address][equivalence.From][equivalence.To] = struct{}{}
					added++
				}
			}
		}
	}
	return added
}

func (repo *repository) pruneDirtyVariants(results map[byte][]*emitter.FunctionResult) int {
	emitted := make(map[codegen.Variant]emittedVariant)
	hardDirty := make(map[codegen.Variant]struct{}, len(repo.cumulativeDirty))
	for key := range repo.cumulativeDirty {
		hardDirty[key] = struct{}{}
	}
	garbageEvidence := make(map[codegen.Variant][]codegen.Variant)
	for _, bank := range repo.banks {
		bankResults := results[bank.ID]
		for index, entry := range bank.Config.Entries {
			if index >= len(bankResults) || bankResults[index] == nil {
				continue
			}
			key := codegen.Variant{Address: decoder.Address24(bank.ID, entry.Start), M: entry.EntryMX.M & 1, X: entry.EntryMX.X & 1}
			emitted[key] = emittedVariant{result: bankResults[index]}
			for _, evidence := range bankResults[index].GarbageEvidence {
				garbageEvidence[key] = append(garbageEvidence[key], codegen.Variant{Address: key.Address, M: evidence.Sibling[0], X: evidence.Sibling[1]})
			}
			for _, marker := range stubMarkers {
				if strings.Contains(bankResults[index].Source, marker) {
					hardDirty[key] = struct{}{}
					break
				}
			}
		}
	}
	for key := range emitted {
		repo.cumulativeEmit[key] = struct{}{}
	}

	dirty := make(map[codegen.Variant]struct{}, len(hardDirty)+len(garbageEvidence))
	for key := range hardDirty {
		dirty[key] = struct{}{}
	}
	for key := range garbageEvidence {
		dirty[key] = struct{}{}
	}
	pruned := repo.computePrunable(dirty, repo.cumulativeEmit, repo.cumulativePrune)
	// A split-immediate proof is usable only when at least one sibling that
	// spans the BRK survives this same prune decision. Solve that dependency
	// before mutating the entry set; otherwise mutually speculative garbage
	// variants can incorrectly validate and prune one another.
	for iteration := 0; iteration < 16; iteration++ {
		effective := make(map[codegen.Variant]struct{}, len(hardDirty)+len(garbageEvidence))
		for key := range hardDirty {
			effective[key] = struct{}{}
		}
		for key, siblings := range garbageEvidence {
			for _, sibling := range siblings {
				if _, exists := emitted[sibling]; !exists {
					continue
				}
				if _, old := repo.cumulativePrune[sibling]; old {
					continue
				}
				if _, pending := pruned[sibling]; pending {
					continue
				}
				effective[key] = struct{}{}
				break
			}
		}
		next := repo.computePrunable(effective, repo.cumulativeEmit, repo.cumulativePrune)
		dirty = effective
		if sameVariantSet(next, pruned) {
			pruned = next
			break
		}
		pruned = next
	}
	for key := range dirty {
		repo.cumulativeDirty[key] = struct{}{}
	}
	if len(pruned) == 0 {
		// Reference taint is deliberately a separate, later phase. Ordinary
		// pruning changes generated call routing; propagating against the stale
		// pre-prune references falsely drops callers whose edges disappear on
		// the required re-emit.
		refs := repo.scanDirectReferences(emitted)
		refPruned := make(map[codegen.Variant]struct{})
		for {
			tainted := make(map[codegen.Variant]struct{}, len(repo.cumulativeDirty))
			for key := range repo.cumulativeDirty {
				tainted[key] = struct{}{}
			}
			emittedNow := make(map[codegen.Variant]struct{}, len(emitted))
			for key := range emitted {
				if _, old := repo.cumulativePrune[key]; old {
					continue
				}
				if _, pending := refPruned[key]; pending {
					continue
				}
				emittedNow[key] = struct{}{}
			}
			for changed := true; changed; {
				changed = false
				for caller, targets := range refs {
					if _, already := tainted[caller]; already {
						continue
					}
					for target := range targets {
						_, targetDirty := tainted[target]
						_, targetEmitted := emittedNow[target]
						targetBankInSet := repo.byBank[byte(target.Address>>16)] != nil
						if targetDirty || (targetBankInSet && !targetEmitted) {
							tainted[caller] = struct{}{}
							changed = true
							break
						}
					}
				}
			}
			excluded := make(map[codegen.Variant]struct{}, len(repo.cumulativePrune)+len(refPruned))
			for key := range repo.cumulativePrune {
				excluded[key] = struct{}{}
			}
			for key := range refPruned {
				excluded[key] = struct{}{}
			}
			next := repo.computePrunable(tainted, emittedNow, excluded)
			if len(next) == 0 {
				break
			}
			for key := range next {
				refPruned[key] = struct{}{}
			}
		}
		pruned = refPruned
	}
	if len(pruned) == 0 {
		repo.rebuildValidVariants()
		return 0
	}
	if os.Getenv("SNESRECOMP_TRACE_PRUNE") != "" {
		keys := make([]codegen.Variant, 0, len(pruned))
		for key := range pruned {
			keys = append(keys, key)
		}
		sort.Slice(keys, func(i, j int) bool {
			if keys[i].Address != keys[j].Address {
				return keys[i].Address < keys[j].Address
			}
			if keys[i].M != keys[j].M {
				return keys[i].M < keys[j].M
			}
			return keys[i].X < keys[j].X
		})
		for _, key := range keys {
			fmt.Fprintf(os.Stderr, "v2regen: prune $%06X M%dX%d\n", key.Address, key.M, key.X)
		}
	}
	for key := range pruned {
		repo.cumulativePrune[key] = struct{}{}
	}
	for _, bank := range repo.banks {
		kept := bank.Config.Entries[:0]
		for _, entry := range bank.Config.Entries {
			key := codegen.Variant{Address: decoder.Address24(bank.ID, entry.Start), M: entry.EntryMX.M & 1, X: entry.EntryMX.X & 1}
			if _, drop := pruned[key]; !drop {
				kept = append(kept, entry)
			}
		}
		bank.Config.Entries = kept
	}
	repo.rebuildValidVariants()
	repo.rebuildNames()
	return len(pruned)
}

func (repo *repository) computePrunable(dirty, emitted, excluded map[codegen.Variant]struct{}) map[codegen.Variant]struct{} {
	pruned := make(map[codegen.Variant]struct{})
	for key := range dirty {
		if _, skip := excluded[key]; skip {
			continue
		}
		canonical := repo.canonical[key.Address]
		if len(canonical) == 0 {
			canonical = map[[2]uint8]struct{}{{1, 1}: {}}
		}
		if _, isCanonical := canonical[[2]uint8{key.M, key.X}]; isCanonical {
			continue
		}
		for pair := range canonical {
			candidate := codegen.Variant{Address: key.Address, M: pair[0], X: pair[1]}
			if _, exists := emitted[candidate]; !exists {
				continue
			}
			if _, isDirty := dirty[candidate]; !isDirty {
				pruned[key] = struct{}{}
				break
			}
		}
	}
	return pruned
}

func sameVariantSet(left, right map[codegen.Variant]struct{}) bool {
	if len(left) != len(right) {
		return false
	}
	for key := range left {
		if _, found := right[key]; !found {
			return false
		}
	}
	return true
}

var directVariantCallRE = regexp.MustCompile(`([A-Za-z_]\w*)_M([01])X([01])\(cpu\)`)

func (repo *repository) scanDirectReferences(emitted map[codegen.Variant]emittedVariant) map[codegen.Variant]map[codegen.Variant]struct{} {
	nameToAddress := make(map[string]uint32)
	for _, bank := range repo.banks {
		for _, entry := range bank.Config.Entries {
			if entry.Name != "" {
				nameToAddress[entry.Name] = decoder.Address24(bank.ID, entry.Start)
			}
		}
	}
	resolve := func(name string) (uint32, bool) {
		if address, found := nameToAddress[name]; found {
			return address, true
		}
		match := syntheticNameRE.FindStringSubmatch(name)
		if match == nil {
			return 0, false
		}
		bank, bankErr := strconv.ParseUint(match[1], 16, 8)
		pc, pcErr := strconv.ParseUint(match[2], 16, 16)
		if bankErr != nil || pcErr != nil {
			return 0, false
		}
		return uint32(bank)<<16 | uint32(pc), true
	}
	refs := make(map[codegen.Variant]map[codegen.Variant]struct{})
	for caller, item := range emitted {
		for _, line := range strings.Split(item.result.Source, "\n") {
			trimmed := strings.TrimSpace(line)
			if strings.HasPrefix(trimmed, "case 0:") || strings.HasPrefix(trimmed, "case 1:") || strings.HasPrefix(trimmed, "case 2:") || strings.HasPrefix(trimmed, "case 3:") || strings.HasPrefix(trimmed, "default:") {
				continue
			}
			for _, match := range directVariantCallRE.FindAllStringSubmatch(line, -1) {
				address, found := resolve(match[1])
				if !found {
					continue
				}
				m, _ := strconv.ParseUint(match[2], 10, 8)
				x, _ := strconv.ParseUint(match[3], 10, 8)
				target := codegen.Variant{Address: address & 0xffffff, M: uint8(m), X: uint8(x)}
				if target == caller {
					continue
				}
				if refs[caller] == nil {
					refs[caller] = make(map[codegen.Variant]struct{})
				}
				refs[caller][target] = struct{}{}
			}
		}
	}
	return refs
}

func (repo *repository) rebuildValidVariants() {
	repo.validVariants = make(map[uint32]map[[2]uint8]struct{})
	for _, bank := range repo.banks {
		for _, entry := range bank.Config.Entries {
			address := decoder.Address24(bank.ID, entry.Start)
			if repo.validVariants[address] == nil {
				repo.validVariants[address] = make(map[[2]uint8]struct{})
			}
			repo.validVariants[address][[2]uint8{entry.EntryMX.M & 1, entry.EntryMX.X & 1}] = struct{}{}
		}
	}
}

var stubMarkers = []string{"IndirectGoto: target", "IndirectGoto: dispatch table", "Call indirect SUPPRESSED", "Call: target unknown", "unresolvable cross-fn goto", "cpu_trace_unresolved_goto_trap", "cpu_trace_unresolved_stub_trap", "Goto with no successor", "unresolvable cross-bank goto", "unresolved IndirectGoto"}

func lintStubs(directory string) (int, error) {
	paths, err := filepath.Glob(filepath.Join(directory, "*_v2.c"))
	if err != nil {
		return 0, err
	}
	hits := 0
	for _, path := range paths {
		data, readErr := os.ReadFile(path)
		if readErr != nil {
			return hits, readErr
		}
		text := string(data)
		for _, marker := range stubMarkers {
			hits += strings.Count(text, marker)
		}
	}
	return hits, nil
}
