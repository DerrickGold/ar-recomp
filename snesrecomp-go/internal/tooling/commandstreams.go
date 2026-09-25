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
)

// ShadowCommandStream describes native command prefixes, NOT a script grammar
// or a closed target set. A selector mask bounds an index, not the valid table
// extent. Only the decoder's existing table entries and instruction ownership
// are inspected. In particular, no stream bytes become code roots here.
type ShadowCommandStream struct {
	SelectorPC       uint32                  `json:"selector_pc"`
	FetchPC          uint32                  `json:"fetch_pc"`
	SignTestPC       uint32                  `json:"sign_test_pc"`
	TablePC          uint32                  `json:"table_pc"`
	SelectorMask     uint16                  `json:"selector_mask"`
	TagByteOffset    int                     `json:"tag_byte_offset"`
	EntryCursorDelta int                     `json:"entry_cursor_delta"`
	Contexts         []analysis.EntryVariant `json:"contexts"`
	Commands         []ShadowCommandPrefix   `json:"commands"`
	DataPaths        []ShadowCommandDataPath `json:"native_data_paths,omitempty"`
	ProofObligations []string                `json:"proof_obligations"`
}

type ShadowCommandPrefix struct {
	Index        int                           `json:"index"`
	EntryPC      uint32                        `json:"entry_pc"`
	CursorDelta  int                           `json:"cursor_delta"`
	StopPC       uint32                        `json:"stop_pc"`
	StopReason   string                        `json:"stop_reason"`
	Callback     *ShadowCommandCallback        `json:"callback_operand,omitempty"`
	Deferred     *ShadowDeferredCommandOperand `json:"deferred_field_operand,omitempty"`
	CursorStores []ShadowCommandCursorStore    `json:"conditional_cursor_stores,omitempty"`
	Contexts     []analysis.EntryVariant       `json:"decoded_in"`
	Advances     []ShadowCommandAdvance        `json:"native_refetch_paths,omitempty"`
}

type ShadowCommandCursorStore struct {
	PC          uint32 `json:"pc"`
	Mode        string `json:"mode"`
	Operand     uint16 `json:"operand"`
	CursorDelta int    `json:"cursor_delta"`
	// A literal cursor is an absolute stream address in the reloading stream's
	// bank, not a delta from the command entry Y. CursorDelta is then zero.
	Literal bool   `json:"literal_cursor,omitempty"`
	Value   uint16 `json:"literal_cursor_value,omitempty"`
}

type ShadowCommandCallback struct {
	LoadPC             uint32              `json:"load_pc"`
	OperandOffset      int                 `json:"offset_from_command_entry_y"`
	Width              int                 `json:"width_bytes"`
	StreamBankSource   string              `json:"stream_bank_source"`
	StreamBank         *uint8              `json:"stream_bank,omitempty"`
	StorePC            uint32              `json:"store_pc"`
	StoreMode          string              `json:"store_mode"`
	StoreOperand       uint16              `json:"store_operand"`
	DispatchPC         uint32              `json:"dispatch_pc"`
	PointerAddress     uint16              `json:"pointer_address"`
	RequiredD          *uint16             `json:"required_d,omitempty"`
	DecodedProgramBank uint8               `json:"decoded_program_bank"` // JMP target uses live PB, which may be a mirror.
	PushedWordPC       uint32              `json:"pushed_word_pc,omitempty"`
	PushedWord         *uint16             `json:"pushed_word,omitempty"`
	Frame              *ShadowCommandFrame `json:"native_frame,omitempty"`
}

const shadowCommandPrefixLimit = 64

func collectShadowCommandStreams(graph *decoder.Graph) []ShadowCommandStream {
	hasSelector := false
	for _, d := range graph.Instructions {
		if d != nil && d.Instruction != nil && d.Key.M == 0 && d.Key.X == 0 && d.Instruction.Opcode == 0x7c && len(d.Instruction.DispatchEntries) != 0 {
			hasSelector = true
			break
		}
	}
	if !hasSelector {
		return nil
	}
	walk := shadowPointerWalk{graph: graph, preds: make(map[decoder.DecodeKey][]decoder.DecodeKey)}
	for _, d := range graph.Instructions {
		for _, next := range d.Successors {
			walk.preds[next] = append(walk.preds[next], d.Key)
		}
	}
	var result []ShadowCommandStream
	for _, key := range graph.Order {
		d := graph.Instructions[key]
		if d == nil || d.Instruction == nil || key.M != 0 || key.X != 0 || d.Instruction.Opcode != 0x7c || len(d.Instruction.DispatchEntries) == 0 {
			continue
		}
		// Word fetch; BMI tagged path; cursor steps; XBA; AND mask; ASL;
		// TAX; JMP (table,X). Check the actual predecessor edges, not adjacent
		// bytes, so a branch around a defining instruction cannot qualify.
		at := key
		take := func(mnemonic string, mode cpu65816.AddressingMode) *decoder.DecodedInstruction {
			p := walk.previous(at)
			if p == nil || p.Instruction == nil || p.Key.M != 0 || p.Key.X != 0 || p.Instruction.Mnemonic != mnemonic || p.Instruction.Mode != mode {
				return nil
			}
			at = p.Key
			return p
		}
		if take("TAX", cpu65816.IMP) == nil || take("ASL", cpu65816.ACC) == nil {
			continue
		}
		mask := take("AND", cpu65816.IMM)
		if mask == nil || mask.Instruction.Operand > 0x7f || mask.Instruction.Operand == 0 || mask.Instruction.Operand&(mask.Instruction.Operand+1) != 0 || take("XBA", cpu65816.IMP) == nil {
			continue
		}
		steps := 0
		for steps < 4 && take("INY", cpu65816.IMP) != nil {
			steps++
		}
		branchTarget := at.PC
		branch := take("BMI", cpu65816.REL)
		if branch == nil || (branch.Key.PC&0xff0000|branch.Instruction.Operand) != branchTarget {
			continue
		}
		fetch := take("LDA", cpu65816.ABSY)
		if fetch == nil {
			continue
		}
		r := ShadowCommandStream{
			SelectorPC: key.PC, FetchPC: fetch.Key.PC, SignTestPC: branch.Key.PC,
			TablePC: key.PC&0xff0000 | d.Instruction.Operand, SelectorMask: uint16(mask.Instruction.Operand),
			TagByteOffset: int(fetch.Instruction.Operand) + 1, EntryCursorDelta: steps,
			Contexts:         []analysis.EntryVariant{{PC: graph.Entry.PC, EntryMX: analysis.MXState{M: graph.Entry.M, X: graph.Entry.X}}},
			ProofObligations: []string{"independently_rooted_streams", "stream_bank_and_ROM_ownership", "valid_command_table_extent", "complete_command_effects_and_successors", "scratch_pointer_alias_and_lifetime", "callback_live_mx_and_return_contract"},
		}
		for n, target := range d.Instruction.DispatchEntries {
			if n > int(r.SelectorMask) {
				break
			}
			entry := decoder.DecodeKey{PC: target, M: key.M, X: key.X, PStack: key.PStack, PDepth: key.PDepth}
			r.Commands = append(r.Commands, summarizeShadowCommandPrefix(graph, entry, n))
		}
		result = append(result, r)
	}
	return result
}

// A local bank byte can be a constant, inherited DB, or unknown. PHK must not
// silently turn the stream bank into the callback bank: reads retain their
// bank provenance at the time of the load.
type shadowCommandByte struct {
	value   int // -1 means unknown
	entryDB bool
	pushPC  uint32
	cursor  bool
	delta   int
	part    int // 0 low byte, 1 high byte
}

func summarizeShadowCommandPrefix(graph *decoder.Graph, entry decoder.DecodeKey, index int) ShadowCommandPrefix {
	r := ShadowCommandPrefix{Index: index, EntryPC: entry.PC, Contexts: []analysis.EntryVariant{{PC: graph.Entry.PC, EntryMX: analysis.MXState{M: graph.Entry.M, X: graph.Entry.X}}}}
	at := entry
	seen := make(map[decoder.DecodeKey]bool)
	var a *ShadowCommandCallback
	var stored *ShadowCommandCallback
	db := shadowCommandByte{value: -1, entryDB: true}
	var stack []shadowCommandByte
	unknownByte := shadowCommandByte{value: -1}
	frameComplete := true
	carry, decimal := "unknown", "unknown"
	pushUnknown := func(n int) {
		for range n {
			stack = append(stack, unknownByte)
		}
	}
	for range shadowCommandPrefixLimit {
		r.StopPC = at.PC
		if seen[at] {
			r.StopReason = "prefix_cycle"
			return r
		}
		seen[at] = true
		d := graph.Instructions[at]
		if d == nil || d.Instruction == nil {
			r.StopReason = "outside_decoded_graph"
			return r
		}
		if at.M != 0 || at.X != 0 {
			r.StopReason = "unknown_or_changed_width"
			return r
		}
		i := d.Instruction
		switch i.Mnemonic {
		case "LDA":
			a = nil
			if i.Mode == cpu65816.ABSY {
				a = &ShadowCommandCallback{LoadPC: at.PC, OperandOffset: r.CursorDelta + int(i.Operand), Width: 2, StreamBankSource: "unknown"}
				if db.entryDB {
					a.StreamBankSource = "command_entry_db"
				}
				if db.value >= 0 {
					b := uint8(db.value)
					a.StreamBankSource, a.StreamBank = "constant", &b
				}
			}
		case "LDX": // X is not the stream cursor in this shape.
		case "INY":
			r.CursorDelta++
		case "DEY":
			r.CursorDelta--
		case "STA":
			if stored == nil && a != nil && (i.Mode == cpu65816.DPX || i.Mode == cpu65816.ABSX) {
				r.Deferred = &ShadowDeferredCommandOperand{
					LoadPC: a.LoadPC, OperandOffset: a.OperandOffset, Width: a.Width,
					StreamBankSource: a.StreamBankSource, StreamBank: a.StreamBank,
					StorePC: at.PC, StoreMode: i.Mode.String(), StoreOperand: uint16(i.Operand),
					ProofObligations: deferredFieldObligations(),
				}
				r.StopReason = "deferred_field_store"
				return r // No claim about the command's following cursor/field effects.
			}
			// Stop at other stores: even a different spelling may alias this
			// scratch slot. Only an initial candidate store is admitted.
			if stored != nil || a == nil || i.Mode != cpu65816.DP {
				r.StopReason = "unmodeled_memory_write"
				return r
			}
			v := *a
			v.StorePC, v.StoreMode, v.StoreOperand = at.PC, i.Mode.String(), uint16(i.Operand)
			stored = &v
		case "STY":
			// Saving the cursor to an indexed object field is structural
			// evidence only. It may alias the scratch slot: retain that store
			// in the report, never claim an unbroken reaching definition.
			if i.Mode != cpu65816.ABSX && i.Mode != cpu65816.DPX {
				r.StopReason = "unmodeled_memory_write"
				return r
			}
			r.CursorStores = append(r.CursorStores, ShadowCommandCursorStore{PC: at.PC, Mode: i.Mode.String(), Operand: uint16(i.Operand), CursorDelta: r.CursorDelta})
		case "PHB":
			b := db
			b.pushPC = 0 // This is a new byte push, not the original PEA word.
			stack = append(stack, b)
		case "PHK":
			stack = append(stack, shadowCommandByte{value: int(at.PC >> 16)})
		case "PEA":
			stack = append(stack, shadowCommandByte{value: int(i.Operand >> 8), pushPC: at.PC, part: 1}, shadowCommandByte{value: int(i.Operand & 255), pushPC: at.PC})
		case "PLB":
			db = unknownByte
			if len(stack) != 0 {
				db = stack[len(stack)-1]
				stack = stack[:len(stack)-1]
			} else {
				frameComplete = false // Borrowed an unowned caller byte.
			}
			db.pushPC = 0
		case "PHY":
			stack = append(stack,
				shadowCommandByte{value: -1, cursor: true, delta: r.CursorDelta, part: 1},
				shadowCommandByte{value: -1, cursor: true, delta: r.CursorDelta})
		case "PHA", "PHX":
			pushUnknown(2)
		case "CLC":
			carry = "clear"
		case "SEC":
			carry = "set"
		case "CLD":
			decimal = "clear"
		case "SED":
			decimal = "set"
		case "NOP":
		case "BRA", "BRL":
		case "JMP":
			if i.Mode == cpu65816.INDIR {
				r.StopReason = "indirect_target_without_operand_provenance"
				if stored != nil {
					v := *stored
					v.DispatchPC, v.PointerAddress, v.DecodedProgramBank = at.PC, uint16(i.Operand), byte(at.PC>>16)
					required := uint16(i.Operand) - v.StoreOperand
					v.RequiredD = &required
					v.Frame = &ShadowCommandFrame{Complete: frameComplete, CursorDelta: r.CursorDelta,
						DB: shadowFrameByte(db), Carry: carry, Decimal: decimal}
					for _, b := range stack {
						v.Frame.Stack = append(v.Frame.Stack, shadowFrameByte(b))
					}
					if n := len(stack); n >= 2 && stack[n-1].pushPC != 0 && stack[n-1].pushPC == stack[n-2].pushPC {
						word := uint16(stack[n-1].value | stack[n-2].value<<8)
						v.PushedWord, v.PushedWordPC = &word, stack[n-1].pushPC
					}
					r.Callback, r.StopReason = &v, "callback_operand_dispatch"
				}
				return r
			}
			if i.Mode != cpu65816.ABS {
				r.StopReason = "unsupported_control_transfer"
				return r
			}
		default:
			r.StopReason = "unsupported_" + i.Mnemonic
			return r
		}
		if len(d.Successors) != 1 {
			r.StopReason = "ambiguous_or_terminal_successor"
			return r
		}
		at = d.Successors[0]
	}
	r.StopPC, r.StopReason = at.PC, "prefix_budget_exhausted"
	return r
}

func mergeShadowCommandStreams(results []shadowDecodeResult) []ShadowCommandStream {
	// Native command bodies often live in sibling graphs after discovery.
	// Join ONLY an independently decoded exact M0X0 entry, starting a fresh
	// symbolic entry-Y/DB query. Never concatenate instruction maps across
	// owners or decode past a data/HLE boundary to fill a missing prefix.
	entries := make(map[uint32][]ShadowCommandPrefix)
	for _, result := range results {
		if result.issue == nil && result.entry.M == 0 && result.entry.X == 0 && len(result.commandPrefix.Contexts) != 0 {
			entries[result.entry.Address] = append(entries[result.entry.Address], result.commandPrefix)
		}
	}
	// Deduplicate contextual repetition but preserve genuinely different
	// summaries. The index remains part of the key: commands sharing a native
	// handler must not collapse into one command.
	byShape := make(map[string]*ShadowCommandStream)
	for _, decoded := range results {
		for _, item := range decoded.commandStreams {
			contexts := item.Contexts
			commands := item.Commands
			item.Contexts = nil
			item.Commands = nil
			key, _ := json.Marshal(item)
			r := byShape[string(key)]
			if r == nil {
				copy := item
				r = &copy
				byShape[string(key)] = r
			}
			r.Contexts = append(r.Contexts, contexts...)
			for _, c := range commands {
				if c.StopReason == "outside_decoded_graph" && c.StopPC == c.EntryPC && len(entries[c.EntryPC]) != 0 {
					for _, p := range entries[c.EntryPC] {
						p.Index = c.Index
						r.Commands = append(r.Commands, p)
					}
				} else {
					r.Commands = append(r.Commands, c)
				}
			}
		}
	}
	keys := make([]string, 0, len(byShape))
	for key := range byShape {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	var out []ShadowCommandStream
	for _, key := range keys {
		r := byShape[key]
		r.Contexts = normalizeShadowCommandContexts(r.Contexts)
		prefixes := make(map[string]*ShadowCommandPrefix)
		for _, c := range r.Commands {
			contexts := c.Contexts
			c.Contexts = nil
			key, _ := json.Marshal(c)
			p := prefixes[string(key)]
			if p == nil {
				copy := c
				p = &copy
				prefixes[string(key)] = p
			}
			p.Contexts = append(p.Contexts, contexts...)
		}
		prefixKeys := make([]string, 0, len(prefixes))
		for k := range prefixes {
			prefixKeys = append(prefixKeys, k)
		}
		sort.Strings(prefixKeys)
		r.Commands = nil
		for _, k := range prefixKeys {
			p := prefixes[k]
			p.Contexts = normalizeShadowCommandContexts(p.Contexts)
			r.Commands = append(r.Commands, *p)
		}
		sort.SliceStable(r.Commands, func(i, j int) bool { return r.Commands[i].Index < r.Commands[j].Index })
		out = append(out, *r)
	}
	return annotateShadowCommandDataPaths(annotateShadowCommandAdvances(linkDeferredCommandOperands(out, mergeShadowDeferredFields(results)), results), results)
}

func normalizeShadowCommandContexts(contexts []analysis.EntryVariant) []analysis.EntryVariant {
	sort.Slice(contexts, func(i, j int) bool {
		a, b := contexts[i], contexts[j]
		if a.PC != b.PC {
			return a.PC < b.PC
		}
		if a.EntryMX.M != b.EntryMX.M {
			return a.EntryMX.M < b.EntryMX.M
		}
		return a.EntryMX.X < b.EntryMX.X
	})
	return slices.Compact(contexts)
}

func writeShadowCommandStreams(out io.Writer, streams []ShadowCommandStream) {
	for _, s := range streams {
		fmt.Fprintf(out, "[COMMAND-STREAM] selector=%s fetch=%s table=%s mask=$%02X command-prefixes=%d (report-only; stream roots unproven)\n", shadowAddress(s.SelectorPC), shadowAddress(s.FetchPC), shadowAddress(s.TablePC), s.SelectorMask, len(s.Commands))
		for _, c := range s.Commands {
			fmt.Fprintf(out, "  command=$%02X entry=%s cursor-delta=%d stop=%s reason=%s\n", c.Index, shadowAddress(c.EntryPC), c.CursorDelta, shadowAddress(c.StopPC), c.StopReason)
			if p := c.Callback; p != nil {
				fmt.Fprintf(out, "    callback load=%s entry-Y%+d bank=%s target-bank=live-PB (decoded=$%02X) slot=$%04X required-D=$%04X dispatch=%s", shadowAddress(p.LoadPC), p.OperandOffset, p.StreamBankSource, p.DecodedProgramBank, p.PointerAddress, *p.RequiredD, shadowAddress(p.DispatchPC))
				if p.StreamBank != nil {
					fmt.Fprintf(out, " stream-bank=$%02X", *p.StreamBank)
				}
				if p.PushedWord != nil {
					fmt.Fprintf(out, " pushed-word=$%04X at=%s (return contract unproven)", *p.PushedWord, shadowAddress(p.PushedWordPC))
				}
				fmt.Fprintln(out)
			}
			for _, st := range c.CursorStores {
				fmt.Fprintf(out, "    cursor-store=%s %s $%04X entry-Y%+d (scratch nonalias unproven)\n", shadowAddress(st.PC), st.Mode, st.Operand, st.CursorDelta)
			}
			if p := c.Deferred; p != nil {
				fmt.Fprintf(out, "    deferred-field load=%s entry-Y%+d bank=%s store=%s %s $%04X consumers=%d (conditional field match; no target values inferred)\n", shadowAddress(p.LoadPC), p.OperandOffset, p.StreamBankSource, shadowAddress(p.StorePC), p.StoreMode, p.StoreOperand, len(p.Consumers))
			}
			for _, a := range c.Advances {
				fmt.Fprintf(out, "    native-refetch=%s entry-Y%+d stop=%s (conditional native register effects)\n", a.Status, a.CursorDelta, shadowAddress(a.StopPC))
			}
		}
		fmt.Fprintf(out, "  obligations=%v\n", s.ProofObligations)
		for _, p := range s.DataPaths {
			fmt.Fprintf(out, "  native-data=%s fetch-Y%+d stop=%s branches=%d cursor-publications=%d (conditional paths; publications do not prove later resumption)\n", p.Status, p.CursorDelta, shadowAddress(p.StopPC), len(p.Branches), len(p.Publications))
		}
	}
}
