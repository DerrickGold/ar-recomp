package tooling

import (
	"fmt"
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// These summaries are conditional on the decoded entry M/X and ordinary call
// stack contract. They never become production facts, roots, or HLE contracts.
// A memory write poisons every locally saved byte: different spellings do not
// prove that a saved DB/status byte is safe from aliases.
const (
	shadowDBUnknown    = int16(-1)
	shadowDBEntry      = int16(256)
	shadowDBStatus     = int16(512)
	shadowDBStackLimit = 64
	shadowDBWorkLimit  = 4096
)

type shadowDBNode struct {
	key     decoder.DecodeKey
	op      byte
	mode    cpu65816.AddressingMode
	operand uint32
	next    []int
	call    *shadowDBCall
	blocked string
}

type shadowDBCall struct {
	targets []decoder.Variant
	closed  bool
	reason  string
	frame   int
}

type shadowDBProgram struct {
	entry decoder.Variant
	nodes []shadowDBNode
	start int
}

type ShadowBankBlocker struct {
	PC       uint32            `json:"pc"`
	Reason   string            `json:"reason"`
	TargetPC uint32            `json:"target_pc,omitempty"`
	TargetMX *analysis.MXState `json:"target_mx,omitempty"`
}

type ShadowBankCallCheck struct {
	PC                uint32              `json:"call_pc"`
	TargetSetClosed   bool                `json:"target_superset_closed"`
	TargetCount       int                 `json:"target_count"`
	PreservingCount   int                 `json:"preserving_target_count"`
	Status            string              `json:"status"`
	Blockers          []ShadowBankBlocker `json:"blockers,omitempty"`
	BlockersTruncated bool                `json:"blockers_truncated,omitempty"`
}

type shadowDBSummary struct {
	valid     bool
	preserves bool
	constant  *byte // one exact DB on every modeled normal return
	writes    bool
	returns   uint8 // RTS=1, RTL=2
	modes     []analysis.MXState
	peak      int
	blockers  []ShadowBankBlocker
}

type shadowDBEval struct {
	summary shadowDBSummary
	values  []int16
	checks  []ShadowBankCallCheck
}

func collectShadowDBProgram(image romimage.Image, graph *decoder.Graph) *shadowDBProgram {
	p := &shadowDBProgram{entry: decoder.Variant{Address: graph.Entry.PC, M: graph.Entry.M, X: graph.Entry.X}}
	indices := make(map[decoder.DecodeKey]int)
	for i, k := range graph.Order {
		indices[k] = i
	}
	var found bool
	p.start, found = indices[graph.Entry]
	if !found {
		p.start = -1
	}
	walk := shadowPointerWalk{graph: graph, preds: shadowPredecessors(graph)}
	for _, k := range graph.Order {
		d := graph.Instructions[k]
		n := shadowDBNode{key: k}
		if d == nil || d.Instruction == nil {
			n.blocked = "missing_instruction"
			p.nodes = append(p.nodes, n)
			continue
		}
		i := d.Instruction
		n.op, n.mode, n.operand = i.Opcode, i.Mode, i.Operand
		isCall := i.Mnemonic == "JSR" || i.Mnemonic == "JSL"
		if isCall {
			c := &shadowDBCall{frame: 2}
			n.call = c
			target := func(pc uint32) { c.targets = append(c.targets, decoder.Variant{Address: pc, M: k.M, X: k.X}) }
			switch {
			case i.Opcode == 0x20 && i.Mode == cpu65816.ABS && i.DispatchKind == "" && i.DispatchEntries == nil:
				target(k.PC&0xff0000 | i.Operand&0xffff)
				c.closed = true
			case i.Opcode == 0x22 && i.Mode == cpu65816.LONG && i.DispatchKind == "" && i.DispatchEntries == nil:
				target(i.Operand & 0xffffff)
				c.closed = true
				c.frame = 3
			case i.Opcode == 0xfc && i.Mode == cpu65816.INDIRX:
				// Use a finite local X superset, never the decoder's heuristic
				// table prefix. Every possible word read must be immutable ROM.
				index := walk.indexExpression(k, "X", true)
				c.closed = len(index.DomainValues) > 0
				c.reason = "open_indirect_index_domain"
				if byte(k.PC>>16) == 0x7e || byte(k.PC>>16) == 0x7f {
					c.closed = false
					c.reason = "indirect_pointer_not_ROM"
					break
				}
				for _, x := range index.DomainValues {
					addr := uint16(i.Operand) + x
					if addr == 0xffff {
						c.closed = false
						c.reason = "indirect_pointer_bank_boundary"
						break
					}
					data, err := image.Slice(byte(k.PC>>16), addr, 2)
					if err != nil {
						c.closed = false
						c.reason = "indirect_pointer_not_ROM"
						break
					}
					target(k.PC&0xff0000 | uint32(data[0]) | uint32(data[1])<<8)
				}
			default:
				c.reason = "unsupported_or_collapsed_call"
			}
			if c.closed {
				c.reason = ""
			} else {
				c.targets = nil
			}
			sort.Slice(c.targets, func(a, b int) bool { return c.targets[a].Address < c.targets[b].Address })
			c.targets = slices.Compact(c.targets)
		} else if i.DispatchKind != "" || i.DispatchEntries != nil {
			n.blocked = "collapsed_control_flow"
		}
		for _, successor := range d.Successors {
			// Auto-dispatch successors may also include handler bodies. A
			// normal call resumes only at the real opcode's fallthrough PC.
			if isCall {
				length := uint16(3)
				if i.Opcode == 0x22 {
					length = 4
				}
				if successor.PC != k.PC&0xff0000|uint32(uint16(k.PC)+length) {
					continue
				}
			}
			if at, ok := indices[successor]; ok {
				n.next = append(n.next, at)
			} else {
				n.next = append(n.next, -1)
			}
		}
		p.nodes = append(p.nodes, n)
	}
	return p
}

type shadowDBState struct {
	at    int
	db    int16
	stack []int16
}

func evaluateShadowDB(p *shadowDBProgram, summaries map[decoder.Variant]shadowDBSummary, hle map[uint32]bool, query *decoder.Variant) shadowDBEval {
	return evaluateShadowDBWithStackPolicy(p, summaries, hle, query, false)
}

func evaluateShadowDBWithStackPolicy(p *shadowDBProgram, summaries map[decoder.Variant]shadowDBSummary, hle map[uint32]bool, query *decoder.Variant, conditionalStack bool) shadowDBEval {
	r := shadowDBEval{}
	block := func(pc uint32, reason string) {
		r.summary.blockers = append(r.summary.blockers, ShadowBankBlocker{PC: pc, Reason: reason})
	}
	if p == nil || len(p.nodes) == 0 {
		block(0, "missing_program")
		return r
	}
	if hle[p.entry.Address] {
		block(p.entry.Address, "HLE_entry_contract")
		return r
	}
	// A prefix query concerns only decoded paths that can reach its exact
	// PC/M/X. An unrelated branch's later call/return is not unresolved debt
	// for this read. Opaque entries and non-local returns remain obligations.
	var relevant []bool
	if query != nil {
		relevant = make([]bool, len(p.nodes))
		preds := make([][]int, len(p.nodes))
		var work []int
		for i, n := range p.nodes {
			if n.key.PC == query.Address && n.key.M == query.M && n.key.X == query.X {
				relevant[i] = true
				work = append(work, i)
			}
			for _, j := range n.next {
				if j >= 0 && j < len(p.nodes) {
					preds[j] = append(preds[j], i)
				}
			}
		}
		for len(work) > 0 {
			i := work[0]
			work = work[1:]
			for _, j := range preds[i] {
				if !relevant[j] {
					relevant[j] = true
					work = append(work, j)
				}
			}
		}
	}
	queue := []shadowDBState{{at: p.start, db: shadowDBEntry}}
	seen := make(map[string]bool)
	for len(queue) > 0 {
		state := queue[0]
		queue = queue[1:]
		if len(seen) >= shadowDBWorkLimit {
			block(p.entry.Address, "bank_state_budget")
			break
		}
		if state.at < 0 || state.at >= len(p.nodes) {
			block(p.entry.Address, "incomplete_CFG_or_external_tail")
			continue
		}
		if relevant != nil && !relevant[state.at] {
			continue
		}
		n := p.nodes[state.at]
		id := fmt.Sprintf("%d/%d/%v", state.at, state.db, state.stack)
		if seen[id] {
			continue
		}
		seen[id] = true
		if hle[n.key.PC] {
			block(n.key.PC, "HLE_site_contract")
			continue
		}
		if query != nil && n.key.PC == query.Address && n.key.M == query.M && n.key.X == query.X {
			r.values = append(r.values, state.db)
			continue
		}
		if n.blocked != "" {
			block(n.key.PC, n.blocked)
			continue
		}
		state.stack = slices.Clone(state.stack)
		push := func(v int16) { state.stack = append(state.stack, v) }
		pop := func() (int16, bool) {
			if len(state.stack) == 0 {
				return shadowDBUnknown, false
			}
			i := len(state.stack) - 1
			v := state.stack[i]
			state.stack = state.stack[:i]
			return v, true
		}
		poison := func() {
			r.summary.writes = true
			// Cold target inventory may ask what follows IF native writes
			// leave the local saved bytes intact. This is not a preservation
			// proof and is never enabled for published bank summaries.
			if conditionalStack {
				return
			}
			for i := range state.stack {
				state.stack[i] = shadowDBUnknown
			}
		}
		postM, postX := n.key.M, n.key.X
		var callModes []analysis.MXState
		if n.call != nil {
			c := n.call
			check := ShadowBankCallCheck{PC: n.key.PC, TargetSetClosed: c.closed, TargetCount: len(c.targets), Status: "blocked"}
			if !c.closed {
				check.Blockers = []ShadowBankBlocker{{PC: n.key.PC, Reason: c.reason}}
			} else {
				preserves := true
				for _, target := range c.targets {
					s, ok := summaries[target]
					reason := ""
					switch {
					case hle[target.Address]:
						reason = "HLE_target_contract"
					case !ok:
						reason = "missing_exact_entry_variant"
					case !s.valid:
						reason = "callee_summary_unresolved"
					case (c.frame == 2 && s.returns != 1) || (c.frame == 3 && s.returns != 2):
						reason = "callee_return_kind_mismatch"
					}
					if reason != "" {
						mx := analysis.MXState{M: target.M, X: target.X}
						check.Blockers = append(check.Blockers, ShadowBankBlocker{PC: n.key.PC, Reason: reason, TargetPC: target.Address, TargetMX: &mx})
						continue
					}
					if s.preserves {
						check.PreservingCount++
					} else {
						preserves = false
						if s.constant != nil && len(c.targets) == 1 {
							state.db = int16(*s.constant)
						}
					}
					if s.writes {
						poison()
					}
					r.summary.peak = max(r.summary.peak, len(state.stack)+c.frame+s.peak)
					callModes = append(callModes, s.modes...)
				}
				if len(check.Blockers) == 0 && len(c.targets) > 0 {
					check.Status = "preserves_DB_on_normal_return"
					if !preserves {
						if len(c.targets) != 1 || summaries[c.targets[0]].constant == nil {
							state.db = shadowDBUnknown
						}
						check.Status = "may_change_DB"
					}
				}
			}
			if len(check.Blockers) > 8 {
				check.Blockers = check.Blockers[:8]
				check.BlockersTruncated = true
			}
			r.checks = append(r.checks, check)
			if check.Status == "blocked" {
				block(n.key.PC, "call_bank_contract_unresolved")
				continue
			}
		} else {
			switch n.op {
			case 0x8b:
				push(state.db) // PHB
			case 0x4b:
				push(int16(n.key.PC >> 16)) // PHK
			case 0xab:
				v, ok := pop()
				if !ok {
					block(n.key.PC, "PLB_outside_local_stack")
					continue
				}
				state.db = v
				if v >= shadowDBStatus {
					state.db = shadowDBUnknown
				}
			case 0x08:
				push(shadowDBStatus + int16(n.key.M*2+n.key.X))
			case 0x28:
				v, ok := pop()
				if !ok || v < shadowDBStatus {
					block(n.key.PC, "PLP_status_not_proven")
					continue
				}
				postM, postX = uint8(v-shadowDBStatus)>>1, uint8(v-shadowDBStatus)&1
			case 0x48, 0xda, 0x5a, 0x0b, 0xf4, 0x62, 0xd4:
				count := 2
				if n.op == 0x48 && n.key.M == 1 || (n.op == 0xda || n.op == 0x5a) && n.key.X == 1 {
					count = 1
				}
				for range count {
					push(shadowDBUnknown)
				}
			case 0x68, 0xfa, 0x7a, 0x2b:
				count := 2
				if n.op == 0x68 && n.key.M == 1 || (n.op == 0xfa || n.op == 0x7a) && n.key.X == 1 {
					count = 1
				}
				if len(state.stack) < count {
					block(n.key.PC, "pull_outside_local_stack")
					continue
				}
				state.stack = state.stack[:len(state.stack)-count]
			case 0xc2:
				if n.operand&0x20 != 0 {
					postM = 0
				}
				if n.operand&0x10 != 0 {
					postX = 0
				}
			case 0xe2:
				if n.operand&0x20 != 0 {
					postM = 1
				}
				if n.operand&0x10 != 0 {
					postX = 1
				}
			case 0x60, 0x6b:
				if query != nil {
					continue
				}
				if len(state.stack) != 0 {
					block(n.key.PC, "unbalanced_normal_return")
					continue
				}
				r.values = append(r.values, state.db)
				if n.op == 0x60 {
					r.summary.returns |= 1
				} else {
					r.summary.returns |= 2
				}
				r.summary.modes = append(r.summary.modes, analysis.MXState{M: n.key.M, X: n.key.X})
				continue
			case 0x00, 0x02, 0x40, 0xcb, 0xdb, 0xfb, 0x9a, 0x1b, 0x44, 0x54, 0x42:
				block(n.key.PC, "unsupported_stack_interrupt_or_block_move")
				continue
			case 0x6c, 0x7c, 0xdc, 0x5c:
				block(n.key.PC, "indirect_or_long_tail")
				continue
			default:
				if shadowDBMemoryWrite(n.op, n.mode) {
					poison()
				}
			}
		}
		r.summary.peak = max(r.summary.peak, len(state.stack))
		if r.summary.peak > shadowDBStackLimit {
			block(n.key.PC, "stack_depth_budget")
			continue
		}
		if len(n.next) == 0 {
			block(n.key.PC, "missing_fallthrough")
			continue
		}
		if n.call == nil {
			callModes = []analysis.MXState{{M: postM, X: postX}}
		}
		// Never use a summary to choose a canonical decode width. Every
		// proven exit mode needs a matching existing successor interpretation.
		for _, mode := range callModes {
			matched := false
			for _, at := range n.next {
				if at < 0 {
					block(n.key.PC, "incomplete_CFG_or_external_tail")
					continue
				}
				next := p.nodes[at].key
				if next.M == mode.M && next.X == mode.X {
					matched = true
					queue = append(queue, shadowDBState{at: at, db: state.db, stack: state.stack})
				}
			}
			if !matched {
				block(n.key.PC, "exit_MX_not_in_decoded_successors")
			}
		}
	}
	slices.Sort(r.values)
	r.values = slices.Compact(r.values)
	if len(r.values) == 0 && len(r.summary.blockers) == 0 {
		reason := "no_normal_return"
		if query != nil {
			reason = "no_decoded_query_path"
		}
		block(p.entry.Address, reason)
	}
	sort.Slice(r.summary.modes, func(i, j int) bool { a, b := r.summary.modes[i], r.summary.modes[j]; return a.M*2+a.X < b.M*2+b.X })
	r.summary.modes = slices.Compact(r.summary.modes)
	r.summary.valid = len(r.summary.blockers) == 0 && len(r.values) > 0
	r.summary.preserves = r.summary.valid && len(r.values) == 1 && r.values[0] == shadowDBEntry
	if r.summary.valid && len(r.values) == 1 && r.values[0] >= 0 && r.values[0] <= 255 {
		b := byte(r.values[0])
		r.summary.constant = &b
	}
	return r
}

func shadowDBMemoryWrite(op byte, mode cpu65816.AddressingMode) bool {
	// Store and memory read-modify-write opcodes. Accumulator shifts do not
	// write stack memory; all explicit memory writes conservatively might.
	if mode == cpu65816.ACC {
		return false
	}
	switch op {
	case 0x81, 0x83, 0x85, 0x87, 0x8d, 0x8f, 0x91, 0x92, 0x93, 0x95, 0x97, 0x99, 0x9d, 0x9f,
		0x86, 0x8e, 0x96, 0x84, 0x8c, 0x94, 0x64, 0x74, 0x9c, 0x9e,
		0x04, 0x0c, 0x14, 0x1c, 0x06, 0x0e, 0x16, 0x1e, 0x26, 0x2e, 0x36, 0x3e,
		0x46, 0x4e, 0x56, 0x5e, 0x66, 0x6e, 0x76, 0x7e, 0xc6, 0xce, 0xd6, 0xde, 0xe6, 0xee, 0xf6, 0xfe:
		return true
	}
	return false
}

func buildShadowDBSummaries(programs map[decoder.Variant]*shadowDBProgram, hle map[uint32]bool) map[decoder.Variant]shadowDBSummary {
	return buildShadowDBSummariesWithStackPolicy(programs, hle, false)
}

func buildShadowDBSummariesWithStackPolicy(programs map[decoder.Variant]*shadowDBProgram, hle map[uint32]bool, conditionalStack bool) map[decoder.Variant]shadowDBSummary {
	keys := make([]decoder.Variant, 0, len(programs))
	for k := range programs {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool {
		a, b := keys[i], keys[j]
		if a.Address != b.Address {
			return a.Address < b.Address
		}
		return a.M*2+a.X < b.M*2+b.X
	})
	summaries := make(map[decoder.Variant]shadowDBSummary, len(programs))
	for _, k := range keys {
		summaries[k] = shadowDBSummary{}
	}
	// Least fixed point: mutually recursive unproven groups remain unknown;
	// no cycle bootstraps its own preservation claim.
	for range 32 {
		changed := false
		for _, k := range keys {
			if summaries[k].valid {
				continue
			}
			s := evaluateShadowDBWithStackPolicy(programs[k], summaries, hle, nil, conditionalStack).summary
			summaries[k] = s
			if s.valid {
				changed = true
			}
		}
		if !changed {
			break
		}
	}
	return summaries
}
