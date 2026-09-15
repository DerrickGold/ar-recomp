package tooling

import (
	"encoding/json"
	"fmt"
	"slices"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const shadowCallbackInstructionLimit = 256
const shadowCallbackPathLimit = 8
const shadowCallbackCallLimit = 8
const shadowCallbackStackLimit = 64
const shadowCallbackWorkLimit = 65536

// Bytes are in push order (bottom to top). A saved cursor is NOT a native
// return address. An opaque caller stack lies below these command-owned bytes.
type ShadowCommandStackByte struct {
	Source string                   `json:"source"`
	Value  byte                     `json:"value,omitempty"`
	Delta  int                      `json:"cursor_delta,omitempty"`
	Part   int                      `json:"word_byte,omitempty"`
	PushPC uint32                   `json:"push_pc,omitempty"`
	Read   *ShadowCommandCursorRead `json:"conditional_stream_read,omitempty"`
}

// Symbolic ROM operand, resolved only against an independently rooted stream.
// Delta applies after loading the word; Offset applies to the old entry Y.
type ShadowCommandCursorRead struct {
	LoadPC     uint32 `json:"load_pc"`
	Offset     int    `json:"offset_from_command_entry_y"`
	BankSource string `json:"bank_source"`
	Bank       byte   `json:"bank,omitempty"`
	Delta      int    `json:"loaded_cursor_delta"`
}

type ShadowCommandFrame struct {
	Complete    bool                     `json:"no_caller_stack_borrow"`
	CursorDelta int                      `json:"live_cursor_delta"`
	DB          ShadowCommandStackByte   `json:"live_db"`
	Stack       []ShadowCommandStackByte `json:"bytes_in_push_order,omitempty"`
	Carry       string                   `json:"carry"`
	Decimal     string                   `json:"decimal"`
}

func shadowFrameByte(b shadowCommandByte) ShadowCommandStackByte {
	r := ShadowCommandStackByte{Source: "unknown", PushPC: b.pushPC}
	switch {
	case b.entryDB:
		r.Source = "entry_db"
	case b.cursor:
		r.Source, r.Delta, r.Part = "entry_y", b.delta, b.part
	case b.value >= 0:
		r.Source, r.Value, r.Part = "constant", byte(b.value), b.part
	}
	return r
}

// A bounded conditional native decode, rooted ONLY by a ROM operand already
// read by the command walker. Neither it nor its successors acquire code
// ownership or generation facts. Calls are followed with actual owned return
// bytes, not assumed to preserve the stack, Y, DB, or arithmetic state.
type ShadowCommandCallbackPath struct {
	StreamCursorRead      *ShadowCommandCursorRead      `json:"conditional_stream_cursor_read,omitempty"`
	EntryDecimalCondition string                        `json:"conditional_entry_decimal,omitempty"`
	CursorPublications    []ShadowCommandCursorStore    `json:"conditional_cursor_publications,omitempty"`
	TargetPC              uint32                        `json:"target_pc"`
	Status                string                        `json:"status"`
	StopPC                uint32                        `json:"stop_pc"`
	CursorDelta           int                           `json:"cursor_delta_from_command_entry"`
	CursorKnown           bool                          `json:"cursor_known"`
	StackBytes            int                           `json:"remaining_owned_stack_bytes"`
	DBSource              string                        `json:"db_source"`
	Path                  []uint32                      `json:"conditional_native_path,omitempty"`
	Branches              []ShadowCommandBranch         `json:"required_branches,omitempty"`
	Returns               []ShadowCommandCallbackReturn `json:"owned_returns,omitempty"`
	Obligations           []string                      `json:"proof_obligations"`
}

type ShadowCommandCallbackReturn struct {
	PC       uint32 `json:"pc"`
	TargetPC uint32 `json:"target_pc"`
	Source   string `json:"source"` // matched_jsr, matched_jsl, or command_pea
	PushPC   uint32 `json:"push_pc"`
}

type shadowCallbackWord struct {
	lo, hi ShadowCommandStackByte
}

func shadowCallbackUnknown() shadowCallbackWord {
	return shadowCallbackWord{ShadowCommandStackByte{Source: "unknown"}, ShadowCommandStackByte{Source: "unknown", Part: 1}}
}

func shadowCallbackCursor(delta int) shadowCallbackWord {
	return shadowCallbackWord{ShadowCommandStackByte{Source: "entry_y", Delta: delta}, ShadowCommandStackByte{Source: "entry_y", Delta: delta, Part: 1}}
}
func shadowCallbackConstant(v uint16) shadowCallbackWord {
	return shadowCallbackWord{ShadowCommandStackByte{Source: "constant", Value: byte(v)}, ShadowCommandStackByte{Source: "constant", Value: byte(v >> 8), Part: 1}}
}
func (w shadowCallbackWord) cursor() (int, bool) {
	return w.lo.Delta, w.lo.Source == "entry_y" && w.hi.Source == "entry_y" && w.lo.Part == 0 && w.hi.Part == 1 && w.lo.Delta == w.hi.Delta
}

func (w shadowCallbackWord) streamRead() *ShadowCommandCursorRead {
	if w.lo.Source != "stream_word" || w.hi.Source != "stream_word" || w.lo.Part != 0 || w.hi.Part != 1 || w.lo.Read == nil || w.hi.Read == nil || *w.lo.Read != *w.hi.Read || w.lo.Delta != w.hi.Delta {
		return nil
	}
	r := *w.lo.Read
	r.Delta += w.lo.Delta
	return &r
}

func (w shadowCallbackWord) advanced(delta int) shadowCallbackWord {
	if n, ok := w.cursor(); ok {
		return shadowCallbackCursor(n + delta)
	}
	if w.streamRead() != nil {
		w.lo.Delta += delta
		w.hi.Delta += delta
		return w
	}
	return shadowCallbackUnknown()
}

type shadowCallbackCall struct {
	pc, next uint32
	kind     string
	base     int
}
type shadowCallbackState struct {
	pc             uint32
	a, y           shadowCallbackWord
	db             ShadowCommandStackByte
	stack          []ShadowCommandStackByte
	calls          []shadowCallbackCall
	carry, decimal string
	path           ShadowCommandCallbackPath
	seen           map[uint32]bool
	owned          map[int]int // Physical byte -> start of this scout instruction.
}

func (s shadowCallbackState) clone() shadowCallbackState {
	s.stack = slices.Clone(s.stack)
	s.calls = slices.Clone(s.calls)
	s.path.Path = slices.Clone(s.path.Path)
	s.path.Branches = slices.Clone(s.path.Branches)
	s.path.Returns = slices.Clone(s.path.Returns)
	s.path.CursorPublications = slices.Clone(s.path.CursorPublications)
	seen := map[uint32]bool{}
	for k, v := range s.seen {
		seen[k] = v
	}
	s.seen = seen
	owned := map[int]int{}
	for k, v := range s.owned {
		owned[k] = v
	}
	s.owned = owned
	return s
}
func (s *shadowCallbackState) push(w shadowCallbackWord) { s.stack = append(s.stack, w.hi, w.lo) }
func (s *shadowCallbackState) pop() (shadowCallbackWord, bool) {
	n := len(s.stack)
	if n < 2 {
		return shadowCallbackWord{}, false
	}
	w := shadowCallbackWord{s.stack[n-1], s.stack[n-2]}
	s.stack = s.stack[:n-2]
	return w, true
}

type shadowCallbackAnalyzer struct {
	nativeCommands bool // Experimental conditional cold inventory, not ordinary reports.
	image          romimage.Image
	blocked        map[int]string // Physical ROM offsets: overrides apply to mirrors too.
	interiors      map[int]bool   // Existing M0X0 instruction interiors, never fresh roots.
	starts         map[int]bool
	cache          map[string][]ShadowCommandCallbackPath
	remaining      int
}

func newShadowCallbackAnalyzer(image romimage.Image, banks []shadowBank, results []shadowDecodeResult, tables []ShadowTableSpan) *shadowCallbackAnalyzer {
	a := &shadowCallbackAnalyzer{image: image, blocked: map[int]string{}, interiors: map[int]bool{}, starts: map[int]bool{}, cache: map[string][]ShadowCommandCallbackPath{}, remaining: shadowCallbackWorkLimit}
	block := func(pc uint32, size int, reason string) {
		for n := 0; n < size && int(uint16(pc))+n <= 0xffff; n++ {
			if off, ok := a.offset(pc + uint32(n)); ok {
				// Deterministic even when aliases supply several override types.
				if old := a.blocked[off]; old == "" || reason < old {
					a.blocked[off] = reason
				}
			}
		}
	}
	for _, bank := range banks {
		cfg := bank.Config
		if cfg == nil {
			continue
		}
		for pc, reason := range shadowCommandNativeBlockers(cfg) {
			block(uint32(bank.ID)<<16|uint32(pc), 1, reason)
		}
		for _, e := range cfg.Entries {
			if e.End != nil {
				block(uint32(bank.ID)<<16|uint32(e.Start), int(*e.End)-int(e.Start)+1, "authored_body_override")
			}
		}
		for _, r := range cfg.ExcludeRanges {
			block(uint32(bank.ID)<<16|uint32(r.Start), int(r.End)-int(r.Start)+1, "authored_exclude_boundary")
		}
		for _, r := range cfg.DataRegions {
			block(uint32(r.Bank)<<16|uint32(r.Start), int(r.End)-int(r.Start)+1, "authored_data_boundary")
		}
		for pc := range cfg.ForceVariantAt {
			block(pc, 1, "authored_width_override")
		}
		for _, route := range cfg.ExitMXAt {
			block(route.Address, 1, "authored_width_override")
		}
	}
	for _, table := range tables {
		block(table.StartPC, int(table.EndExclusive-table.StartPC), "dispatch_table_boundary")
	}
	// Whole-body HLE/override policies must not be bypassed via an internal
	// branch into a decoded fallback. Freeze the blockers before propagation.
	entries := map[int]string{}
	for off, reason := range a.blocked {
		entries[off] = reason
	}
	for _, r := range results {
		off, ok := a.offset(r.entry.Address)
		reason := entries[off]
		for _, d := range r.instructions {
			if ok && reason != "" {
				block(d.PC, int(d.Instruction.Length), reason)
			}
			if d.M != 0 || d.X != 0 {
				continue
			}
			if start, ok := a.offset(d.PC); ok {
				a.starts[start] = true
			}
			for n := 1; n < int(d.Instruction.Length); n++ {
				if off, ok := a.offset(d.PC + uint32(n)); ok {
					a.interiors[off] = true
				}
			}
		}
	}
	return a
}

func (a *shadowCallbackAnalyzer) offset(pc uint32) (int, bool) {
	if !a.image.IsROM(byte(pc>>16), uint16(pc)) {
		return 0, false
	}
	off, err := a.image.Offset(byte(pc>>16), uint16(pc))
	return off, err == nil && off >= 0 && off < len(a.image)
}
func (a *shadowCallbackAnalyzer) instruction(pc uint32) (*cpu65816.Instruction, string) {
	off, ok := a.offset(pc)
	if !ok {
		return nil, "callback_not_ROM"
	}
	if reason := a.blocked[off]; reason != "" {
		return nil, reason
	}
	if a.interiors[off] {
		return nil, "conflicting_decoded_byte_ownership"
	}
	i, err := decodeShadowInstruction(a.image, byte(pc>>16), uint16(pc), 0, 0)
	if err != nil {
		return nil, "callback_decode_error"
	}
	if int(uint16(pc))+int(i.Length) > 0xffff {
		return nil, "callback_bank_boundary"
	}
	for n := 0; n < int(i.Length); n++ {
		p, ok := a.offset(pc + uint32(n))
		if !ok || p != off+n {
			return nil, "callback_mapping_boundary"
		}
		if reason := a.blocked[p]; reason != "" {
			return nil, reason
		}
		if n != 0 && a.starts[p] {
			return nil, "conflicting_decoded_byte_ownership"
		}
	}
	return i, ""
}

func (a *shadowCallbackAnalyzer) paths(stream ShadowCommandStream, c ShadowCommandPrefix, target uint32) []ShadowCommandCallbackPath {
	key, _ := json.Marshal([]any{stream.SelectorPC, stream.FetchPC, c.EntryPC, c.Callback, target})
	if paths, ok := a.cache[string(key)]; ok {
		return paths
	}
	paths := a.query(stream, c, target)
	a.cache[string(key)] = paths
	return paths
}

func (a *shadowCallbackAnalyzer) query(stream ShadowCommandStream, c ShadowCommandPrefix, target uint32) []ShadowCommandCallbackPath {
	if c.Callback == nil {
		return a.queryFrame(stream, target, nil, nil)
	}
	return a.queryFrame(stream, target, c.Callback.Frame, []uint32{c.EntryPC, c.Callback.DispatchPC, stream.SelectorPC})
}

// Native command entries own no new return frame. Enumerate both possible
// entry-D conditions explicitly; a decimal-mode cursor effect remains an
// unresolved alternative, never silently replaced with binary arithmetic.
func (a *shadowCallbackAnalyzer) nativePaths(stream ShadowCommandStream, c ShadowCommandPrefix) []ShadowCommandCallbackPath {
	key, _ := json.Marshal([]any{"native_command", stream.SelectorPC, stream.FetchPC, c.EntryPC})
	if paths, ok := a.cache[string(key)]; ok {
		return paths
	}
	var paths []ShadowCommandCallbackPath
	for _, decimal := range []string{"clear", "set"} {
		f := &ShadowCommandFrame{Complete: true, DB: ShadowCommandStackByte{Source: "entry_db"}, Carry: "unknown", Decimal: decimal}
		for _, p := range a.queryFrame(stream, c.EntryPC, f, []uint32{c.EntryPC, stream.SelectorPC}) {
			p.EntryDecimalCondition = decimal
			p.Obligations = append(p.Obligations, "conditional_entry_decimal_"+decimal+"_not_proven", "native_command_entry_not_callback_or_new_return_frame")
			paths = append(paths, p)
		}
	}
	a.cache[string(key)] = paths
	return paths
}

func (a *shadowCallbackAnalyzer) queryFrame(stream ShadowCommandStream, target uint32, f *ShadowCommandFrame, prefixes []uint32) []ShadowCommandCallbackPath {
	base := ShadowCommandCallbackPath{TargetPC: target, StopPC: target, Obligations: []string{
		"conditional_operand_not_proven_code_or_reachability", "native_M0X0_and_decoded_PB_context",
		"native_memory_stack_ROM_mapping_and_HLE_noninterference", "required_branches_not_proven_feasible",
		"opaque_caller_stack_never_assumed_to_be_a_return_frame", "no_generation_fact_promotion"}}
	if f == nil || !f.Complete {
		base.Status = "callback_frame_unknown"
		return []ShadowCommandCallbackPath{base}
	}
	for _, pc := range prefixes {
		if off, ok := a.offset(pc); !ok {
			base.Status = "callback_prefix_not_ROM"
			return []ShadowCommandCallbackPath{base}
		} else if reason := a.blocked[off]; reason != "" {
			base.Status = reason
			return []ShadowCommandCallbackPath{base}
		}
	}
	queue := []shadowCallbackState{{pc: target, a: shadowCallbackUnknown(), y: shadowCallbackCursor(f.CursorDelta), db: f.DB, stack: slices.Clone(f.Stack), carry: f.Carry, decimal: f.Decimal, path: base, seen: map[uint32]bool{}, owned: map[int]int{}}}
	var out []ShadowCommandCallbackPath
	for len(queue) > 0 {
		s := queue[0]
		queue = queue[1:]
		for s.path.Status == "" {
			s.path.StopPC = s.pc
			if len(s.stack) > shadowCallbackStackLimit {
				s.path.Status = "callback_stack_budget"
				break
			}
			if a.remaining == 0 {
				s.path.Status = "callback_total_work_budget"
				break
			}
			a.remaining--
			i, reason := a.instruction(s.pc)
			if reason != "" {
				s.path.Status = reason
				break
			}
			off, _ := a.offset(s.pc)
			for n := 0; n < int(i.Length); n++ {
				if owner, exists := s.owned[off+n]; exists && owner != off {
					s.path.Status = "conflicting_scout_byte_ownership"
				}
			}
			if s.path.Status != "" {
				break
			}
			for n := 0; n < int(i.Length); n++ {
				s.owned[off+n] = off
			}
			if s.pc == stream.FetchPC {
				delta, known := s.y.cursor()
				if known && s.db.Source == "entry_db" && len(s.stack) == 0 && len(s.calls) == 0 {
					s.path.CursorDelta, s.path.Status = delta, "owned_native_refetch"
				} else if read := s.y.streamRead(); a.nativeCommands && read != nil && s.db.Source == "entry_db" && len(s.stack) == 0 && len(s.calls) == 0 {
					s.path.StreamCursorRead, s.path.Status = read, "owned_native_stream_refetch"
				} else {
					s.path.Status = "refetch_frame_or_cursor_unproven"
				}
				break
			}
			if len(s.path.Path) >= shadowCallbackInstructionLimit {
				s.path.Status = "callback_instruction_budget"
				break
			}
			if s.seen[s.pc] {
				s.path.Status = "callback_cycle_or_repeated_native_site"
				break
			}
			s.seen[s.pc] = true
			s.path.Path = append(s.path.Path, s.pc)
			next := s.pc + uint32(i.Length)
			unknown := shadowCallbackUnknown()
			switch i.Mnemonic {
			case "LDA":
				s.a = unknown
				if i.Mode == cpu65816.IMM {
					s.a = shadowCallbackConstant(uint16(i.Operand))
				}
				if delta, known := s.y.cursor(); a.nativeCommands && i.Mode == cpu65816.ABSY && known && (s.db.Source == "entry_db" || s.db.Source == "constant") {
					read := &ShadowCommandCursorRead{LoadPC: s.pc, Offset: delta + int(i.Operand), BankSource: s.db.Source, Bank: s.db.Value}
					s.a = shadowCallbackWord{ShadowCommandStackByte{Source: "stream_word", Read: read}, ShadowCommandStackByte{Source: "stream_word", Read: read, Part: 1}}
				}
			case "LDY":
				s.y = unknown
			case "TYA":
				s.a = s.y
			case "TAY":
				s.y = s.a
			case "INY", "DEY":
				delta := 1
				if i.Mnemonic == "DEY" {
					delta = -1
				}
				s.y = s.y.advanced(delta)
			case "PHY":
				s.push(s.y)
			case "PHA":
				s.push(s.a)
			case "PHX":
				s.push(unknown)
			case "PLA", "PLY", "PLX":
				w, ok := s.pop()
				if !ok {
					s.path.Status = "opaque_caller_stack_read"
					break
				}
				if i.Mnemonic == "PLA" {
					s.a = w
				}
				if i.Mnemonic == "PLY" {
					s.y = w
				}
			case "PHB":
				s.stack = append(s.stack, s.db)
			case "PHK":
				s.stack = append(s.stack, ShadowCommandStackByte{Source: "constant", Value: byte(s.pc >> 16)})
			case "PLB":
				if len(s.stack) == 0 {
					s.path.Status = "opaque_caller_stack_read"
					break
				}
				s.db = s.stack[len(s.stack)-1]
				s.stack = s.stack[:len(s.stack)-1]
			case "PEA":
				w := shadowCallbackConstant(uint16(i.Operand))
				w.lo.PushPC, w.hi.PushPC = s.pc, s.pc
				s.push(w)
			case "CLC":
				s.carry = "clear"
			case "SEC":
				s.carry = "set"
			case "CLD":
				s.decimal = "clear"
			case "SED":
				s.decimal = "set"
			case "REP", "SEP":
				if i.Operand&0x30 != 0 {
					s.path.Status = "callback_width_change"
					break
				}
				value := "clear"
				if i.Mnemonic == "SEP" {
					value = "set"
				}
				if i.Operand&1 != 0 {
					s.carry = value
				}
				if i.Operand&8 != 0 {
					s.decimal = value
				}
			case "ADC", "SBC":
				if delta, ok := s.a.cursor(); ok && i.Mode == cpu65816.IMM {
					if s.decimal != "clear" {
						s.path.Status = "saved_cursor_decimal_unproven"
						break
					}
					if s.carry != "clear" && s.carry != "set" {
						s.path.Status = "saved_cursor_carry_unproven"
						break
					}
					carry := 0
					if s.carry == "set" {
						carry = 1
					}
					if i.Mnemonic == "ADC" {
						delta += int(i.Operand) + carry
					} else {
						delta -= int(i.Operand) + 1 - carry
					}
					s.a = shadowCallbackCursor(delta)
				} else {
					s.a = unknown
				}
				s.carry = "unknown"
			case "INC", "DEC":
				if i.Mode == cpu65816.ACC {
					if delta, ok := s.a.cursor(); ok {
						if i.Mnemonic == "INC" {
							delta++
						} else {
							delta--
						}
						s.a = shadowCallbackCursor(delta)
					} else {
						s.a = unknown
					}
				} // Memory mutation is conditional on stack/code non-aliasing.
			case "ASL", "LSR", "ROL", "ROR":
				if i.Mode == cpu65816.ACC {
					s.a = unknown
				}
				s.carry = "unknown"
			case "CMP", "CPX", "CPY":
				s.carry = "unknown"
			case "AND", "ORA", "EOR", "XBA", "TXA", "TSC", "TDC":
				s.a = unknown
			case "TXY":
				s.y = unknown
			case "STA", "STY":
				if a.nativeCommands && (i.Mode == cpu65816.DPX || i.Mode == cpu65816.ABSX) {
					value := s.a
					if i.Mnemonic == "STY" {
						value = s.y
					}
					if delta, known := value.cursor(); known {
						s.path.CursorPublications = append(s.path.CursorPublications, ShadowCommandCursorStore{PC: s.pc, Mode: i.Mode.String(), Operand: uint16(i.Operand), CursorDelta: delta})
					}
				}
			case "LDX", "TAX", "TYX", "INX", "DEX", "BIT", "NOP", "CLV", "STX", "STZ", "TRB", "TSB":
			case "BRA", "BRL":
				next = s.pc&0xff0000 | i.Operand
			case "JMP", "JML":
				if i.Mode == cpu65816.ABS {
					next = s.pc&0xff0000 | i.Operand
				} else if i.Mode == cpu65816.LONG {
					next = i.Operand
				} else {
					s.path.Status = "callback_dynamic_transfer"
				}
			case "JSR", "JSL":
				if i.Mode != cpu65816.ABS && i.Mode != cpu65816.LONG {
					s.path.Status = "callback_dynamic_call"
					break
				}
				if len(s.calls) >= shadowCallbackCallLimit {
					s.path.Status = "callback_call_depth"
					break
				}
				call := shadowCallbackCall{s.pc, next, i.Mnemonic, len(s.stack)}
				s.calls = append(s.calls, call)
				count := 2
				if i.Mnemonic == "JSL" {
					count = 3
				}
				for n := count - 1; n >= 0; n-- {
					s.stack = append(s.stack, ShadowCommandStackByte{Source: "call_return", Part: n, PushPC: s.pc})
				}
				if i.Mode == cpu65816.LONG {
					next = i.Operand
				} else {
					next = s.pc&0xff0000 | i.Operand
				}
			case "RTS", "RTL":
				if len(s.calls) != 0 {
					call := s.calls[len(s.calls)-1]
					count, kind := 2, "RTS"
					if call.kind == "JSL" {
						count, kind = 3, "RTL"
					}
					if i.Mnemonic != kind || len(s.stack) != call.base+count || kind == "RTS" && s.pc>>16 != call.pc>>16 {
						s.path.Status = "mismatched_native_return"
						break
					}
					for n := 0; n < count; n++ {
						b := s.stack[len(s.stack)-1-n]
						if b.Source != "call_return" || b.Part != n || b.PushPC != call.pc {
							s.path.Status = "modified_native_return_frame"
						}
					}
					if s.path.Status != "" {
						break
					}
					s.stack = s.stack[:call.base]
					s.calls = s.calls[:len(s.calls)-1]
					next = call.next
					s.path.Returns = append(s.path.Returns, ShadowCommandCallbackReturn{s.pc, next, "matched_" + strings.ToLower(call.kind), call.pc})
				} else {
					stack := slices.Clone(s.stack)
					w, ok := s.pop()
					if i.Mnemonic != "RTS" || !ok || w.lo.Source != "constant" || w.hi.Source != "constant" || w.lo.Part != 0 || w.hi.Part != 1 || w.lo.PushPC == 0 || w.lo.PushPC != w.hi.PushPC {
						s.stack = stack
						s.path.Status = "saved_data_is_not_return_frame"
						break
					}
					next = s.pc&0xff0000 | uint32(uint16(uint16(w.hi.Value)<<8|uint16(w.lo.Value))+1)
					s.path.Returns = append(s.path.Returns, ShadowCommandCallbackReturn{s.pc, next, "command_pea", w.lo.PushPC})
				}
			case "BEQ", "BNE", "BCC", "BCS", "BMI", "BPL", "BVC", "BVS":
				target := s.pc&0xff0000 | i.Operand
				if target == next {
					break
				}
				if (i.Mnemonic == "BCC" || i.Mnemonic == "BCS") && (s.carry == "clear" || s.carry == "set") {
					taken := (s.carry == "set") == (i.Mnemonic == "BCS")
					if taken {
						next = target
					}
					s.path.Branches = append(s.path.Branches, ShadowCommandBranch{s.pc, i.Mnemonic, next, taken})
					break // Do not manufacture a path contradicting a known flag.
				}
				if len(out)+len(queue)+2 > shadowCallbackPathLimit {
					s.path.Status = "callback_path_budget"
					break
				}
				for _, taken := range []bool{false, true} {
					child := s.clone()
					child.pc = next
					if taken {
						child.pc = target
					}
					child.path.Branches = append(child.path.Branches, ShadowCommandBranch{s.pc, i.Mnemonic, child.pc, taken})
					if i.Mnemonic == "BCC" || i.Mnemonic == "BCS" {
						child.carry = "clear"
						if taken == (i.Mnemonic == "BCS") {
							child.carry = "set"
						}
					}
					queue = append(queue, child)
				}
				s.path.Status = "forked"
			default:
				s.path.Status = "callback_unsupported_" + i.Mnemonic
			}
			// Stack-relative memory is NOT covered by the ordinary memory
			// non-alias condition: it directly observes/modifies tracked bytes.
			if i.Mode == cpu65816.STK || i.Mode == cpu65816.STKIY {
				s.path.Status = "callback_stack_relative_access"
			}
			if s.path.Status == "" {
				s.pc = next
			}
		}
		if s.path.Status != "forked" {
			s.path.CursorDelta, s.path.CursorKnown = s.y.cursor()
			s.path.StackBytes, s.path.DBSource = len(s.stack), s.db.Source
			out = append(out, s.path)
		}
	}
	return out
}

func shadowCallbackSummary(p ShadowCommandCallbackPath) string {
	cursor := "unknown"
	if p.CursorKnown {
		cursor = fmt.Sprintf("%+d", p.CursorDelta)
	}
	return fmt.Sprintf("%s -> %s %s Y=%s stack=%d DB=%s branches=%d owned-returns=%d", shadowAddress(p.TargetPC), shadowAddress(p.StopPC), p.Status, cursor, p.StackBytes, p.DBSource, len(p.Branches), len(p.Returns))
}
