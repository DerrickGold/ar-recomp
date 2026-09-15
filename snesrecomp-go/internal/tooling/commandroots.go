package tooling

import (
	"encoding/json"
	"fmt"
	"io"
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// A ROM pointer load that directly supplies Y to a recognized command fetch.
// References below are conditional on a decoded call path, NOT evidence of
// all callers, reachable animation IDs, complete streams, or new code roots.
type ShadowCommandRoot struct {
	Context          analysis.EntryVariant        `json:"context"`
	SelectorPC       uint32                       `json:"selector_pc"`
	FetchPC          uint32                       `json:"fetch_pc"`
	CursorSetPC      uint32                       `json:"cursor_set_pc"`
	FetchCursorDelta int                          `json:"fetch_cursor_delta"`
	FetchOperand     uint16                       `json:"fetch_operand"`
	TableRead        ShadowInitializerRead        `json:"pointer_table_read"`
	StreamBank       ShadowRegisterEvidence       `json:"stream_bank"`
	InputPaths       []ShadowCommandInputPath     `json:"conditional_input_paths,omitempty"`
	References       []ShadowCommandRootReference `json:"references,omitempty"`
	Unresolved       []ShadowCommandRootIssue     `json:"unresolved_inputs,omitempty"`
	Truncated        bool                         `json:"truncated"`
	ProofObligations []string                     `json:"proof_obligations"`
	inputRead        *shadowStreamInputRead
}

type ShadowCommandRootReference struct {
	ValueInput      *ShadowCommandValueInput      `json:"conditional_command_value_input,omitempty"`
	PublishedResume *ShadowCommandPublishedResume `json:"conditional_published_cursor_reload,omitempty"`
	TableIndex      uint16                        `json:"table_index"`
	TableReadPC     uint32                        `json:"table_read_pc,omitempty"`
	Pointer         *uint16                       `json:"pointer,omitempty"`
	StreamPC        *uint32                       `json:"stream_pc,omitempty"`
	FirstFetchPC    *uint32                       `json:"first_fetch_pc,omitempty"`
	DefinitionPC    uint32                        `json:"literal_definition_pc,omitempty"`
	LiteralValue    *uint16                       `json:"literal_value,omitempty"`
	ROMReads        []ShadowCommandInputRead      `json:"rom_input_reads,omitempty"`
	LocalInputs     []ShadowCommandLocalInput     `json:"local_wram_inputs,omitempty"`
	InputEdges      []ShadowCommandInputEdge      `json:"required_input_edges,omitempty"`
	Arithmetic      []ShadowCommandArithmetic     `json:"arithmetic_evidence,omitempty"`
	Calls           []ShadowCommandRootCall       `json:"calls,omitempty"` // outermost to innermost
	Status          string                        `json:"status"`
}

type ShadowCommandRootCall struct {
	Caller     analysis.EntryVariant    `json:"caller"`
	SitePC     uint32                   `json:"site_pc"`
	TargetPC   uint32                   `json:"target_pc"`
	Transfer   string                   `json:"transfer"`
	Register   string                   `json:"register"`
	Value      uint16                   `json:"argument_value"`
	MX         analysis.MXState         `json:"call_mx"`
	Flags      *ShadowCommandInputFlags `json:"native_flags_before_transfer,omitempty"`
	InputEdges []ShadowCommandInputEdge `json:"required_input_edges,omitempty"`
}

type ShadowCommandRootIssue struct {
	Context  analysis.EntryVariant `json:"context"`
	SourcePC uint32                `json:"source_pc"`
	Register string                `json:"register"`
	Origin   ShadowStoredOrigin    `json:"value_origin"`
	Reason   string                `json:"reason"`
}

type shadowStreamInput struct {
	call      ShadowCommandRootCall
	mx        analysis.MXState
	values    [3]ShadowInitializerIndex
	reads     [3]*shadowStreamInputRead
	paths     [3][]ShadowCommandInputPath
	truncated [3]bool
	at        decoder.DecodeKey
}

const shadowStreamCallerDepth = 4
const shadowStreamInputBudget = 128
const shadowStreamReferenceLimit = 128

// The register remains a live value, not a same-spelling memory-field join.
// Only one straight-line path (possibly over direct jumps) from TAY to the
// fetch is admitted. Memory writes do not establish a saved-cursor alias.
func collectShadowCommandRoots(graph *decoder.Graph, streams []ShadowCommandStream) []ShadowCommandRoot {
	if len(streams) == 0 {
		return nil
	}
	walk := shadowPointerWalk{graph: graph, preds: shadowPredecessors(graph)}
	byFetch := make(map[uint32][]ShadowCommandStream)
	for _, s := range streams {
		byFetch[s.FetchPC] = append(byFetch[s.FetchPC], s)
	}
	var out []ShadowCommandRoot
	for _, key := range graph.Order {
		d := graph.Instructions[key]
		if d == nil || d.Instruction == nil || d.Instruction.Mnemonic != "TAY" || key.M != 0 || key.X != 0 {
			continue
		}
		origin := walk.indexExpression(key, "A", true)
		if origin.Source.Kind != "load" || len(origin.Operations) != 0 {
			continue
		}
		read := walk.initializerRead(origin.sourceKey, false)
		if read == nil || (read.Mode != "abs,x" && read.Mode != "abs,y" && read.Mode != "long,x") {
			continue
		}
		// The argument expression may cross transparent stack/bank setup; the
		// ordinary table-initializer report keeps its existing narrower scope.
		read.Index = walk.indexExpression(origin.sourceKey, read.IndexRegister, true)
		read.Index.LocalSource = nil // no substitution through mutable slots
		inputRead := collectShadowStreamInputRead(walk, read.Index, 0)
		inputPaths, pathsTruncated := collectShadowCommandInputPaths(walk, origin.sourceKey, read.IndexRegister)
		bank := walk.initializerBank(key) // DB at cursor use, not necessarily at the table load.
		at, delta := key, 0
		seen := make(map[decoder.DecodeKey]bool)
		for range shadowPointerWalkLimit {
			previous := graph.Instructions[at]
			if previous == nil || len(previous.Successors) != 1 {
				break
			}
			at = previous.Successors[0]
			if seen[at] || at.M != 0 || at.X != 0 {
				break
			}
			seen[at] = true
			next := graph.Instructions[at]
			if next == nil || next.Instruction == nil {
				break
			}
			if matches := byFetch[at.PC]; len(matches) != 0 {
				for _, s := range matches {
					out = append(out, ShadowCommandRoot{
						Context:    analysis.EntryVariant{PC: graph.Entry.PC, EntryMX: analysis.MXState{M: graph.Entry.M, X: graph.Entry.X}},
						SelectorPC: s.SelectorPC, FetchPC: s.FetchPC, CursorSetPC: key.PC, FetchCursorDelta: delta, FetchOperand: uint16(next.Instruction.Operand),
						TableRead: *read, StreamBank: bank,
						inputRead:        inputRead,
						InputPaths:       inputPaths,
						Truncated:        pathsTruncated,
						ProofObligations: []string{"decoded_paths_not_reachability", "direct_call_contexts_not_all_inputs", "ROM_ownership_and_cartridge_write_effects", "entry_HLE_and_live_MX_contracts", "complete_command_effects_and_successors", "no_callback_root_or_closed_target_promotion"},
					})
				}
				break
			}
			i := next.Instruction
			switch i.Mnemonic {
			case "INY":
				delta++
			case "DEY":
				delta--
			case "LDA", "LDX", "STA", "STX", "STY", "STZ", "NOP", "CLC", "SEC", "CLD", "SED", "BRA", "BRL":
			case "JMP":
				if i.Mode != cpu65816.ABS {
					goto nextCursor
				}
			default:
				goto nextCursor
			}
		}
	nextCursor:
	}
	return out
}

// Include direct tail transfers and sibling fallthroughs without changing the
// previously published call-input inventory. A fallthrough query starts AFTER
// its defining instruction; otherwise LDA #id at a boundary loses the ID.
func collectShadowStreamInputs(graph *decoder.Graph, cached ...[]shadowDirectCallInputs) []shadowStreamInput {
	walk := shadowPointerWalk{graph: graph, preds: shadowPredecessors(graph)}
	var out []shadowStreamInput
	var calls []shadowDirectCallInputs
	if len(cached) != 0 {
		calls = cached[0]
	} else {
		calls = collectShadowDirectCallInputs(graph)
	}
	for _, c := range calls {
		transfer := "JSL"
		if len(c.call.InstructionBytes) >= 2 && c.call.InstructionBytes[:2] == "20" {
			transfer = "JSR"
		}
		out = append(out, shadowStreamInput{call: ShadowCommandRootCall{Caller: analysis.EntryVariant{PC: c.call.CallerEntryPC, EntryMX: c.call.CallerEntryMX}, SitePC: c.call.CallPC, TargetPC: c.call.TargetPC, Transfer: transfer}, mx: c.call.CallMX, values: c.values, at: decoder.DecodeKey{PC: c.call.CallPC, M: c.call.CallMX.M, X: c.call.CallMX.X}})
	}
	add := func(at decoder.DecodeKey, site, target uint32, kind string) {
		v := shadowStreamInput{call: ShadowCommandRootCall{Caller: analysis.EntryVariant{PC: graph.Entry.PC, EntryMX: analysis.MXState{M: graph.Entry.M, X: graph.Entry.X}}, SitePC: site, TargetPC: target, Transfer: kind}, mx: analysis.MXState{M: at.M, X: at.X}}
		v.at = at
		for n, reg := range []string{"A", "X", "Y"} {
			v.values[n] = walk.indexExpression(at, reg, true)
		}
		out = append(out, v)
	}
	for _, key := range graph.Order {
		d := graph.Instructions[key]
		if d == nil || d.Instruction == nil {
			continue
		}
		i := d.Instruction
		if i.DispatchKind != "" || i.DispatchEntries != nil {
			continue
		}
		switch i.Opcode {
		case 0x20, 0x22: // Already captured by the ordinary call-input query.
			continue
		case 0x4c:
			add(key, key.PC, key.PC&0xff0000|i.Operand, "JMP")
			continue
		case 0x5c:
			add(key, key.PC, i.Operand, "JML")
			continue
		}
		for _, next := range d.Successors {
			if graph.Instructions[next] == nil {
				add(next, key.PC, next.PC, "decoded_boundary_edge")
			}
		}
	}
	for n := range out {
		out[n].call.MX = out[n].mx
		out[n].call.Flags = shadowCommandFlags(walk, out[n].at)
		for reg, expr := range out[n].values {
			out[n].reads[reg] = collectShadowStreamInputRead(walk, expr, 0)
			out[n].paths[reg], out[n].truncated[reg] = collectShadowCommandInputPaths(walk, out[n].at, []string{"A", "X", "Y"}[reg])
		}
	}
	return out
}

type shadowStreamEntry struct {
	offset int
	mx     analysis.MXState
}

func resolveShadowCommandRoots(image romimage.Image, results []shadowDecodeResult) []ShadowCommandRoot {
	hasRoots := false
	for _, r := range results {
		if len(r.commandRoots) != 0 {
			hasRoots = true
			break
		}
	}
	if !hasRoots {
		return nil
	}
	status := newShadowStatusAnalyzer(image, results)
	// Mapper equivalence joins call destinations to decoded entries. It never
	// substitutes canonical PB for live PB or invents DB from a program bank.
	entryKey := func(pc uint32, mx analysis.MXState) (shadowStreamEntry, bool) {
		if byte(pc>>16) == 0x7e || byte(pc>>16) == 0x7f {
			return shadowStreamEntry{}, false
		}
		off, err := image.Offset(byte(pc>>16), uint16(pc))
		return shadowStreamEntry{off, mx}, err == nil && off >= 0 && off < len(image)
	}
	inputs := make(map[shadowStreamEntry][]shadowStreamInput)
	for _, result := range results {
		if result.issue != nil {
			continue
		}
		for _, in := range result.streamInputs {
			if key, ok := entryKey(in.call.TargetPC, in.mx); ok {
				inputs[key] = append(inputs[key], in)
			}
		}
	}
	// Traversal and budget exhaustion must not depend on worker/map order.
	for k, calls := range inputs {
		byKey := make(map[string]shadowStreamInput)
		for _, c := range calls {
			byKey[shadowStreamInputKey(c)] = c
		}
		keys := make([]string, 0, len(byKey))
		for key := range byKey {
			keys = append(keys, key)
		}
		sort.Strings(keys)
		inputs[k] = nil
		for _, key := range keys {
			inputs[k] = append(inputs[k], byKey[key])
		}
	}
	var out []ShadowCommandRoot
	for _, result := range results {
		for _, raw := range result.commandRoots {
			r := raw
			budget := shadowStreamInputBudget
			active := make(map[struct {
				entry shadowStreamEntry
				reg   string
			}]bool)
			issue := func(ctx analysis.EntryVariant, expr ShadowInitializerIndex, reason string) {
				r.Unresolved = append(r.Unresolved, ShadowCommandRootIssue{Context: ctx, SourcePC: expr.Source.PC, Register: expr.Source.Register, Origin: expr.Source, Reason: reason})
			}
			type value struct {
				word       uint16
				literal    *uint16
				definition uint32
				calls      []ShadowCommandRootCall
				reads      []ShadowCommandInputRead
				locals     []ShadowCommandLocalInput
				arithmetic []ShadowCommandArithmetic
			}
			var values func(analysis.EntryVariant, ShadowInitializerIndex, *shadowStreamInputRead, int) []value
			values = func(ctx analysis.EntryVariant, expr ShadowInitializerIndex, read *shadowStreamInputRead, depth int) []value {
				if budget == 0 {
					r.Truncated = true
					issue(ctx, expr, "caller_query_budget")
					return nil
				}
				budget--
				var found []value
				switch {
				case expr.Source.Kind == "load" && expr.Source.Mode == "imm":
					literal := uint16(expr.Source.Operand)
					found = []value{{word: literal, literal: &literal, definition: expr.Source.PC}}
				case read != nil:
					if read.Issue != "" {
						issue(ctx, expr, read.Issue)
						if read.Issue == "ROM_input_read_depth_limit" || read.Issue == "local_input_store_budget" {
							r.Truncated = true
						}
						return nil
					}
					if local := read.Local; local != nil {
						found = []value{{}}
						if !local.Zero {
							found = values(ctx, local.Value, local.InputRead, depth)
						}
						for n := range found {
							evidence := local.Evidence
							evidence.Word = found[n].word
							found[n].locals = append(append([]ShadowCommandLocalInput(nil), found[n].locals...), evidence)
						}
						break
					}
					indices := []value{{}}
					if read.Index != nil {
						indices = values(ctx, *read.Index, read.InputRead, depth)
					}
					for _, v := range indices {
						evidence, reason := resolveShadowStreamInputRead(image, ctx, read, v.word)
						if reason != "" {
							issue(ctx, expr, reason)
							continue
						}
						v.word = evidence.Word
						v.reads = append(append([]ShadowCommandInputRead(nil), v.reads...), evidence)
						found = append(found, v)
					}
				case expr.Source.Kind == "entry_register":
					key, ok := entryKey(ctx.PC, ctx.EntryMX)
					if !ok {
						issue(ctx, expr, "entry_not_ROM_mapped")
						return nil
					}
					reg := slices.Index([]string{"A", "X", "Y"}, expr.Source.Register)
					if reg < 0 {
						issue(ctx, expr, "unsupported_entry_register")
						return nil
					}
					cycle := struct {
						entry shadowStreamEntry
						reg   string
					}{key, expr.Source.Register}
					if active[cycle] {
						issue(ctx, expr, "recursive_input_cycle")
						return nil
					}
					if depth == shadowStreamCallerDepth {
						r.Truncated = true
						issue(ctx, expr, "caller_depth_limit")
						return nil
					}
					active[cycle] = true
					calls := inputs[key]
					if len(calls) == 0 {
						issue(ctx, expr, "no_matching_decoded_direct_inputs")
					}
					for _, in := range calls {
						if budget == 0 {
							r.Truncated = true
							issue(ctx, expr, "caller_query_budget")
							break
						}
						paths := in.paths[reg]
						if in.truncated[reg] {
							r.Truncated = true
							issue(in.call.Caller, in.values[reg], "caller_input_path_budget")
						}
						if len(paths) == 0 && !in.truncated[reg] {
							paths = []ShadowCommandInputPath{{Value: in.values[reg], read: in.reads[reg], flags: in.call.Flags}}
						}
						for _, path := range paths {
							for _, v := range values(in.call.Caller, path.Value, path.read, depth+1) {
								step := in.call
								step.Flags = status.resolveFlags(path.flags)
								step.InputEdges = slices.Clone(path.Edges)
								step.Register = expr.Source.Register
								step.Value = v.word
								v.calls = append(append([]ShadowCommandRootCall(nil), v.calls...), step)
								found = append(found, v)
							}
						}
					}
					delete(active, cycle)
				default:
					issue(ctx, expr, "nonliteral_input:"+expr.Source.Kind+":"+expr.Source.Reason)
					return nil
				}
				validValues := found[:0]
				for n := range found {
					valid := true
					for _, op := range expr.Operations {
						if op.Mnemonic == "ADC" || op.Mnemonic == "SBC" {
							if op.Carry != nil {
								flag := status.resolveFlag(*op.Carry, 1)
								op.Carry = &flag
							}
							if op.Decimal != nil {
								flag := status.resolveFlag(*op.Decimal, 8)
								op.Decimal = &flag
							}
							same := func(pc uint32, mx analysis.MXState, ctx analysis.EntryVariant) bool {
								a, ok := entryKey(pc, mx)
								b, ok2 := entryKey(ctx.PC, ctx.EntryMX)
								return ok && ok2 && a == b
							}
							op.Carry = resolveShadowCommandFlag(ctx, op.Carry, 1, found[n].calls, same)
							op.Decimal = resolveShadowCommandFlag(ctx, op.Decimal, 8, found[n].calls, same)
						}
						word, ok := shadowCommandInputWord(found[n].word, op)
						if !ok {
							origin := ShadowInitializerIndex{Source: ShadowStoredOrigin{Kind: "unknown", PC: op.PC, Register: expr.Source.Register, Reason: "operation_" + op.Mnemonic}}
							issue(ctx, origin, "input_operation_or_arithmetic_flags_unproven")
							valid = false
							break
						}
						found[n].word = word
						if op.Mnemonic == "ADC" || op.Mnemonic == "SBC" {
							found[n].arithmetic = append(slices.Clone(found[n].arithmetic), ShadowCommandArithmetic{Context: ctx, Operation: op})
						}
					}
					if valid {
						validValues = append(validValues, found[n])
					}
				}
				return validValues
			}
			paths := r.InputPaths
			if len(paths) == 0 {
				paths = []ShadowCommandInputPath{{Value: r.TableRead.Index, read: r.inputRead}}
			}
			for _, path := range paths {
				for _, v := range values(r.Context, path.Value, path.read, 0) {
					if len(r.References) == shadowStreamReferenceLimit {
						r.Truncated = true
						break
					}
					ref := ShadowCommandRootReference{TableIndex: v.word, LiteralValue: v.literal, DefinitionPC: v.definition, Calls: v.calls, ROMReads: v.reads, LocalInputs: v.locals, Arithmetic: v.arithmetic}
					ref.InputEdges = slices.Clone(path.Edges)
					resolveShadowStreamPointer(image, r, &ref)
					r.References = append(r.References, ref)
				}
			}
			r.Unresolved = uniqueShadowStreamIssues(r.Unresolved)
			sort.Slice(r.References, func(i, j int) bool {
				a, _ := json.Marshal(r.References[i])
				b, _ := json.Marshal(r.References[j])
				return string(a) < string(b)
			})
			out = append(out, r)
		}
	}
	// Raw graph contexts are retained, including differing entry width/history
	// interpretations. Remove only byte-identical evidence records.
	sort.Slice(out, func(i, j int) bool {
		a, _ := json.Marshal(out[i])
		b, _ := json.Marshal(out[j])
		return string(a) < string(b)
	})
	return slices.CompactFunc(out, func(a, b ShadowCommandRoot) bool {
		x, _ := json.Marshal(a)
		y, _ := json.Marshal(b)
		return string(x) == string(y)
	})
}

func shadowStreamInputKey(in shadowStreamInput) string {
	// Equal call PCs can have different decoded predecessor histories. Include
	// the expressions so budget-limited queries retain deterministic evidence.
	type pathKey struct {
		Value ShadowInitializerIndex
		Edges []ShadowCommandInputEdge
		Read  *shadowStreamInputRead
		Flags *ShadowCommandInputFlags
	}
	var paths [3][]pathKey
	for reg, alternatives := range in.paths {
		for _, p := range alternatives {
			paths[reg] = append(paths[reg], pathKey{p.Value, p.Edges, p.read, p.flags})
		}
	}
	b, _ := json.Marshal(struct {
		Call      ShadowCommandRootCall
		MX        analysis.MXState
		Values    [3]ShadowInitializerIndex
		Reads     [3]*shadowStreamInputRead
		Paths     [3][]pathKey
		Truncated [3]bool
	}{in.call, in.mx, in.values, in.reads, paths, in.truncated})
	return string(b)
}

func uniqueShadowStreamIssues(in []ShadowCommandRootIssue) []ShadowCommandRootIssue {
	sort.Slice(in, func(i, j int) bool {
		a, _ := json.Marshal(in[i])
		b, _ := json.Marshal(in[j])
		return string(a) < string(b)
	})
	return slices.Compact(in)
}

func shadowStreamConstantBank(bank ShadowRegisterEvidence) (byte, bool) {
	if bank.UnknownPaths || len(bank.Constants) != 1 || bank.Constants[0].Value > 255 {
		return 0, false
	}
	return byte(bank.Constants[0].Value), true
}

func shadowStreamROMWord(image romimage.Image, bank byte, address uint32) (uint16, bool) {
	if bank == 0x7e || bank == 0x7f || address > 0xfffe {
		return 0, false
	}
	a, err := image.Offset(bank, uint16(address))
	b, err2 := image.Offset(bank, uint16(address+1))
	if err != nil || err2 != nil || a < 0 || b < 0 || a >= len(image) || b >= len(image) {
		return 0, false
	}
	return uint16(image[a]) | uint16(image[b])<<8, true
}

func resolveShadowStreamPointer(image romimage.Image, r ShadowCommandRoot, ref *ShadowCommandRootReference) {
	bank, ok := shadowStreamConstantBank(r.TableRead.DataBank)
	if !ok {
		ref.Status = "unknown_pointer_table_bank"
		return
	}
	address := uint32(uint16(r.TableRead.Operand)) + uint32(ref.TableIndex)
	// Deliberately stop at bank boundaries; don't silently truncate an indexed
	// effective address or mistake physical-ROM adjacency for CPU mapping.
	if address > 0xfffe {
		ref.Status = "pointer_table_bank_boundary"
		return
	}
	ref.TableReadPC = uint32(bank)<<16 | address
	word, ok := shadowStreamROMWord(image, bank, address)
	if !ok {
		ref.Status = "pointer_table_not_ROM_mapped"
		return
	}
	ref.Pointer = &word
	streamBank, ok := shadowStreamConstantBank(r.StreamBank)
	if !ok {
		ref.Status = "unknown_stream_bank"
		return
	}
	if streamBank == 0x7e || streamBank == 0x7f || !image.IsROM(streamBank, word) {
		ref.Status = "stream_not_ROM_mapped"
		return
	}
	pc := uint32(streamBank)<<16 | uint32(word)
	ref.StreamPC = &pc
	cursor := int(word) + r.FetchCursorDelta
	if cursor < 0 || cursor > 0xffff || cursor+int(r.FetchOperand) > 0xfffe {
		ref.Status = "first_fetch_bank_boundary"
		return
	}
	fetch := uint32(cursor) + uint32(r.FetchOperand)
	if _, ok := shadowStreamROMWord(image, streamBank, fetch); !ok {
		ref.Status = "first_fetch_not_ROM_mapped"
		return
	}
	pc2 := uint32(streamBank)<<16 | fetch
	ref.FirstFetchPC = &pc2
	ref.Status = "literal_call_path_ROM_stream_reference"
	if len(ref.ROMReads) != 0 {
		ref.Status = "ROM_data_call_path_stream_reference"
	}
	if len(ref.LocalInputs) != 0 {
		ref.Status = "local_WRAM_call_path_stream_reference"
	}
}

func writeShadowCommandRoots(out io.Writer, roots []ShadowCommandRoot) {
	for _, r := range roots {
		fmt.Fprintf(out, "[COMMAND-ROOT-INPUT] selector=%s pointer-load=%s cursor-set=%s references=%d unresolved-inputs=%d truncated=%t (report-only; not all inputs or code roots)\n", shadowAddress(r.SelectorPC), shadowAddress(r.TableRead.LoadPC), shadowAddress(r.CursorSetPC), len(r.References), len(r.Unresolved), r.Truncated)
		for _, path := range r.InputPaths {
			fmt.Fprintf(out, "  conditional-input source=%s %s at=%s operations=%d predecessor-choices=%d (branch feasibility unproven)\n", path.Value.Source.Kind, path.Value.Source.Register, shadowAddress(path.Value.Source.PC), len(path.Value.Operations), len(path.Edges))
			for _, edge := range path.Edges {
				fmt.Fprintf(out, "    requires-edge %s M%dX%d -> %s M%dX%d opcode=$%02X operand=$%X\n", shadowAddress(edge.FromPC), edge.FromMX.M, edge.FromMX.X, shadowAddress(edge.ToPC), edge.ToMX.M, edge.ToMX.X, edge.Opcode, edge.Operand)
			}
		}
		for _, p := range r.References {
			fmt.Fprintf(out, "  index=$%04X table-read=%s status=%s", p.TableIndex, shadowAddress(p.TableReadPC), p.Status)
			if p.LiteralValue != nil {
				fmt.Fprintf(out, " literal=%s:$%04X", shadowAddress(p.DefinitionPC), *p.LiteralValue)
			}
			if p.StreamPC != nil {
				fmt.Fprintf(out, " stream=%s", shadowAddress(*p.StreamPC))
			}
			fmt.Fprintln(out)
			for _, read := range p.ROMReads {
				fmt.Fprintf(out, "    ROM-input load=%s mode=%s index=$%04X read=%s word=$%04X\n", shadowAddress(read.LoadPC), read.Mode, read.Index, shadowAddress(read.ReadPC), read.Word)
			}
			for _, local := range p.LocalInputs {
				fmt.Fprintf(out, "    local-WRAM store=%s load=%s WRAM+$%05X word=$%04X disjoint-writes=%d obligations=%v\n", shadowAddress(local.StorePC), shadowAddress(local.LoadPC), local.LoadAddress.WRAMOffset, local.Word, len(local.DisjointWrites), local.Obligations)
			}
			for _, c := range p.Calls {
				fmt.Fprintf(out, "    %s %s -> %s register=%s value=$%04X\n", c.Transfer, shadowAddress(c.SitePC), shadowAddress(c.TargetPC), c.Register, c.Value)
				if c.Flags != nil {
					writeShadowNativeFlag(out, "C", &c.Flags.Carry, 0)
					writeShadowNativeFlag(out, "D", &c.Flags.Decimal, 0)
				}
				for _, e := range c.InputEdges {
					fmt.Fprintf(out, "      caller-requires-edge %s M%dX%d -> %s M%dX%d\n", shadowAddress(e.FromPC), e.FromMX.M, e.FromMX.X, shadowAddress(e.ToPC), e.ToMX.M, e.ToMX.X)
				}
			}
			for _, a := range p.Arithmetic {
				fmt.Fprintf(out, "    arithmetic %s %s carry=%s decimal=%s (same native call path)\n", shadowAddress(a.Operation.PC), a.Operation.Mnemonic, a.Operation.Carry.Reason, a.Operation.Decimal.Reason)
				writeShadowNativeFlag(out, "C", a.Operation.Carry, 0)
				writeShadowNativeFlag(out, "D", a.Operation.Decimal, 0)
			}
		}
		for _, i := range r.Unresolved {
			fmt.Fprintf(out, "  unresolved-input=%s %s origin=%s %s $%X reason=%s\n", shadowAddress(i.SourcePC), i.Register, i.Origin.Kind, i.Origin.Mode, i.Origin.Operand, i.Reason)
		}
		fmt.Fprintf(out, "  obligations=%v\n", r.ProofObligations)
	}
}
