package tooling

import (
	"fmt"
	"io"
	"slices"
	"sort"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const shadowStatusBodyLimit = 256
const shadowStatusWorkLimit = 4096
const shadowStatusCallDepth = 8

// Conditional on native execution and an ordinary matched return. These are
// NOT termination, reachability, stack-alias, HLE, or production-code proofs.
type ShadowNativeStatusSummary struct {
	Context      analysis.EntryVariant    `json:"context"`
	ReturnKind   string                   `json:"return_kind"`
	Carry        string                   `json:"carry_effect"`
	Decimal      string                   `json:"decimal_effect"`
	Instructions int                      `json:"instructions"`
	Returns      []analysis.EntryVariant  `json:"returns,omitempty"`
	Calls        []ShadowNativeStatusCall `json:"calls,omitempty"`
	Reason       string                   `json:"reason,omitempty"`
}

type ShadowNativeStatusCall struct {
	SitePC   uint32                     `json:"site_pc"`
	TargetPC uint32                     `json:"target_pc"`
	MX       analysis.MXState           `json:"entry_mx"`
	Summary  *ShadowNativeStatusSummary `json:"summary"`
	Transfer string                     `json:"transfer,omitempty"`
}

type shadowStatusBody struct {
	graph  *decoder.Graph
	reason string
}

func collectShadowStatusBody(g *decoder.Graph, cfg *config.Config) *shadowStatusBody {
	b := &shadowStatusBody{}
	if len(g.Instructions) > shadowStatusBodyLimit {
		b.reason = "native_status_body_budget"
		return b
	}
	if len(g.UnresolvedIndirects) != 0 || len(g.SuppressedIndirectCalls) != 0 || len(g.DispatchTargetsSuppressed) != 0 || len(g.ConstantZFolds) != 0 {
		b.reason = "native_status_pruned_or_dynamic_graph"
		return b
	}
	if cfg != nil {
		for _, k := range g.Order {
			pc := uint16(k.PC)
			if cfg.HLEFunctions[pc] != "" || cfg.HLEFunctionsIf[pc].Function != "" || cfg.HLEDispatch[pc] != "" || slices.Contains(cfg.HLESPCUpload, pc) {
				b.reason = "native_status_HLE_boundary"
				return b
			}
		}
	}
	b.graph = g // Immutable, bounded graph; never re-decode through owned boundaries.
	return b
}

type shadowStatusKey struct {
	entry shadowStreamEntry
	kind  string
}
type shadowStatusAnalyzer struct {
	image  romimage.Image
	bodies map[shadowStreamEntry][]*shadowStatusBody
	cache  map[shadowStatusKey]*ShadowNativeStatusSummary
}

func newShadowStatusAnalyzer(image romimage.Image, results []shadowDecodeResult) *shadowStatusAnalyzer {
	a := &shadowStatusAnalyzer{image: image, bodies: make(map[shadowStreamEntry][]*shadowStatusBody), cache: make(map[shadowStatusKey]*ShadowNativeStatusSummary)}
	for _, r := range results {
		body := r.statusBody
		if r.issue != nil {
			body = &shadowStatusBody{reason: "native_status_decode_issue"}
		}
		if k, ok := a.entryKey(r.entry.Address, analysis.MXState{M: r.entry.M, X: r.entry.X}); ok && body != nil {
			a.bodies[k] = append(a.bodies[k], body)
		}
	}
	return a
}

func (a *shadowStatusAnalyzer) entryKey(pc uint32, mx analysis.MXState) (shadowStreamEntry, bool) {
	if !a.image.IsROM(byte(pc>>16), uint16(pc)) {
		return shadowStreamEntry{}, false
	}
	off, err := a.image.Offset(byte(pc>>16), uint16(pc))
	return shadowStreamEntry{offset: off, mx: mx}, err == nil && off >= 0 && off < len(a.image)
}

// A finite symbolic relation: entry bit, zero, one, or unknown. Join never
// selects a canonical branch. Constant definitions may kill earlier uncertainty.
const (
	statusEntry byte = 1 << iota
	statusZero
	statusOne
	statusUnknown
)

func shadowStatusEffect(v byte) string {
	switch v {
	case statusEntry:
		return "preserve"
	case statusZero:
		return "clear"
	case statusOne:
		return "set"
	default:
		return "unknown"
	}
}
func shadowStatusCompose(v byte, effect string) byte {
	switch effect {
	case "preserve":
		return v
	case "clear":
		return statusZero
	case "set":
		return statusOne
	default:
		return statusUnknown
	}
}

func (a *shadowStatusAnalyzer) summarize(pc uint32, mx analysis.MXState, kind string) *ShadowNativeStatusSummary {
	k, ok := a.entryKey(pc, mx)
	if !ok {
		return &ShadowNativeStatusSummary{Reason: "native_status_target_not_ROM"}
	}
	key := shadowStatusKey{k, kind}
	if s := a.cache[key]; s != nil {
		return s
	}
	budget := shadowStatusWorkLimit
	s := a.summarizeKey(key, make(map[shadowStatusKey]bool), 0, &budget)
	a.cache[key] = s
	return s
}

func (a *shadowStatusAnalyzer) summarizeKey(key shadowStatusKey, active map[shadowStatusKey]bool, depth int, budget *int) *ShadowNativeStatusSummary {
	fail := func(reason string) *ShadowNativeStatusSummary {
		return &ShadowNativeStatusSummary{ReturnKind: key.kind, Carry: "unknown", Decimal: "unknown", Reason: reason}
	}
	if depth == shadowStatusCallDepth {
		return fail("native_status_call_depth")
	}
	if active[key] {
		return fail("native_status_recursive_group")
	}
	bodies := a.bodies[key.entry]
	if len(bodies) == 0 {
		return fail("native_status_body_missing")
	}
	// Multiple physical aliases may carry different ownership or HLE contracts.
	// Until that equivalence is proven, do not select one as canonical.
	if len(bodies) != 1 {
		return fail("native_status_ambiguous_body")
	}
	b := bodies[0]
	if b.reason != "" {
		return fail(b.reason)
	}
	g := b.graph
	active[key] = true
	defer delete(active, key)
	type state struct {
		c, d  byte
		stack int
	}
	states := map[decoder.DecodeKey]state{g.Entry: {statusEntry, statusEntry, 0}}
	queue := []decoder.DecodeKey{g.Entry}
	returns := make(map[analysis.EntryVariant]bool)
	calls := make(map[string]ShadowNativeStatusCall)
	visited := make(map[decoder.DecodeKey]bool)
	var exitC, exitD byte
	tail := func(site decoder.DecodeKey, next decoder.DecodeKey, s state, transfer string) string {
		// This is the existing caller's frame, not a fresh JSR/JSL frame.
		// Saved locals crossing ownership boundaries need a richer contract.
		if s.stack != 0 {
			return "native_status_live_stack_tail"
		}
		if next.PStack != 0 || next.PDepth != 0 {
			return "native_status_saved_status_tail"
		}
		if key.kind == "RTS" && byte(site.PC>>16) != byte(next.PC>>16) {
			return "native_status_short_return_bank_change"
		}
		entry, ok := a.entryKey(next.PC, analysis.MXState{M: next.M, X: next.X})
		if !ok {
			return "native_status_tail_not_ROM"
		}
		summary := a.summarizeKey(shadowStatusKey{entry, key.kind}, active, depth+1, budget)
		if summary.Reason != "" {
			return summary.Reason
		}
		exitC |= shadowStatusCompose(s.c, summary.Carry)
		exitD |= shadowStatusCompose(s.d, summary.Decimal)
		for _, r := range summary.Returns {
			returns[r] = true
		}
		call := ShadowNativeStatusCall{SitePC: site.PC, TargetPC: next.PC, MX: analysis.MXState{M: next.M, X: next.X}, Summary: summary, Transfer: transfer}
		calls[shadowStoredJSONKey(call)] = call
		return ""
	}
	for len(queue) != 0 {
		if *budget == 0 {
			return fail("native_status_work_budget")
		}
		*budget--
		at := queue[0]
		queue = queue[1:]
		d := g.Instructions[at]
		if d == nil || d.Instruction == nil {
			return fail("native_status_external_boundary")
		}
		visited[at] = true
		s := states[at]
		i := d.Instruction
		if i.DispatchKind != "" || i.DispatchEntries != nil {
			return fail("native_status_computed_dispatch")
		}
		if i.Opcode == 0x5c && i.Mode == cpu65816.LONG {
			next := decoder.DecodeKey{PC: i.Operand & 0xffffff, M: at.M, X: at.X, PStack: at.PStack, PDepth: at.PDepth}
			if reason := tail(at, next, s, "JML"); reason != "" {
				return fail(reason)
			}
			continue // A long jump has no native fallthrough, whatever the CFG's legacy successor list says.
		}
		if i.Mnemonic == "JMP" && i.Mode != cpu65816.ABS {
			return fail("native_status_computed_transfer")
		}
		if i.Mnemonic == "RTS" || i.Mnemonic == "RTL" {
			if i.Mnemonic != key.kind {
				return fail("native_status_return_kind_mismatch")
			}
			if s.stack != 0 {
				return fail("native_status_unbalanced_return")
			}
			exitC |= s.c
			exitD |= s.d
			returns[analysis.EntryVariant{PC: at.PC, EntryMX: analysis.MXState{M: at.M, X: at.X}}] = true
			continue
		}
		switch i.Mnemonic {
		case "PHP", "PLP", "RTI", "BRK", "COP", "XCE", "TCS", "TXS", "WAI", "STP", "JML":
			return fail("native_status_barrier_" + i.Mnemonic)
		case "PHA":
			s.stack += 2 - int(at.M)
		case "PHX", "PHY":
			s.stack += 2 - int(at.X)
		case "PHB", "PHK":
			s.stack++
		case "PHD", "PEA", "PEI", "PER":
			s.stack += 2
		case "PLA":
			s.stack -= 2 - int(at.M)
		case "PLX", "PLY":
			s.stack -= 2 - int(at.X)
		case "PLB":
			s.stack--
		case "PLD":
			s.stack -= 2
		}
		if s.stack < 0 || s.stack > 32 {
			return fail("native_status_stack_boundary")
		}
		if i.Mnemonic == "JSR" || i.Mnemonic == "JSL" {
			target, kind, ok := shadowStatusDirectCall(d)
			if !ok {
				return fail("native_status_indirect_call")
			}
			calleeKey, ok := a.entryKey(target, analysis.MXState{M: at.M, X: at.X})
			if !ok {
				return fail("native_status_target_not_ROM")
			}
			summary := a.summarizeKey(shadowStatusKey{calleeKey, kind}, active, depth+1, budget)
			if summary.Reason != "" {
				return fail(summary.Reason)
			}
			if !shadowStatusReturnMXCovered(summary, d.Successors) {
				return fail("native_status_return_MX_mismatch")
			}
			s.c, s.d = shadowStatusCompose(s.c, summary.Carry), shadowStatusCompose(s.d, summary.Decimal)
			call := ShadowNativeStatusCall{SitePC: at.PC, TargetPC: target, MX: analysis.MXState{M: at.M, X: at.X}, Summary: summary}
			calls[shadowStoredJSONKey(call)] = call
		} else {
			for n, mask := range []byte{1, 8} {
				flag, stop := shadowStoredFlagEffect(d, mask)
				if !stop {
					continue
				}
				v := statusUnknown
				if flag.Value != nil {
					v = statusZero
					if *flag.Value == 1 {
						v = statusOne
					}
				} else if !strings.HasPrefix(flag.Reason, "carry_clobber_") {
					return fail("native_status_" + flag.Reason)
				}
				if n == 0 {
					s.c = v
				} else {
					s.d = v
				}
			}
		}
		if len(d.Successors) == 0 {
			return fail("native_status_missing_successor")
		}
		for _, next := range d.Successors {
			if g.Instructions[next] == nil {
				if reason := tail(at, next, s, "decoded_boundary"); reason != "" {
					return fail(reason)
				}
				continue
			}
			old, exists := states[next]
			joined := s
			if exists {
				if old.stack != s.stack {
					return fail("native_status_stack_join")
				}
				joined.c |= old.c
				joined.d |= old.d
				if joined == old {
					continue
				}
			}
			states[next] = joined
			queue = append(queue, next)
		}
	}
	if len(returns) == 0 {
		return fail("native_status_no_return")
	}
	summary := &ShadowNativeStatusSummary{Context: analysis.EntryVariant{PC: g.Entry.PC, EntryMX: analysis.MXState{M: g.Entry.M, X: g.Entry.X}}, ReturnKind: key.kind, Carry: shadowStatusEffect(exitC), Decimal: shadowStatusEffect(exitD), Instructions: len(visited)}
	for r := range returns {
		summary.Returns = append(summary.Returns, r)
	}
	sort.Slice(summary.Returns, func(i, j int) bool {
		return shadowStoredJSONKey(summary.Returns[i]) < shadowStoredJSONKey(summary.Returns[j])
	})
	for _, c := range calls {
		summary.Calls = append(summary.Calls, c)
	}
	sort.Slice(summary.Calls, func(i, j int) bool {
		return shadowStoredJSONKey(summary.Calls[i]) < shadowStoredJSONKey(summary.Calls[j])
	})
	return summary
}

func shadowStatusDirectCall(d *decoder.DecodedInstruction) (uint32, string, bool) {
	i := d.Instruction
	if i.Opcode == 0x20 && i.Mode == cpu65816.ABS {
		return d.Key.PC&0xff0000 | i.Operand&0xffff, "RTS", true
	}
	if i.Opcode == 0x22 && i.Mode == cpu65816.LONG {
		return i.Operand & 0xffffff, "RTL", true
	}
	return 0, "", false
}

func shadowStatusReturnMXCovered(s *ShadowNativeStatusSummary, successors []decoder.DecodeKey) bool {
	if len(s.Returns) == 0 {
		return false
	}
	for _, r := range s.Returns {
		if !slices.ContainsFunc(successors, func(k decoder.DecodeKey) bool { return k.M == r.EntryMX.M && k.X == r.EntryMX.X }) {
			return false
		}
	}
	return true
}

func (s *ShadowNativeStatusSummary) String() string {
	return fmt.Sprintf("%06X %s C=%s D=%s %s", s.Context.PC, s.ReturnKind, s.Carry, s.Decimal, s.Reason)
}

func writeShadowNativeFlag(out io.Writer, name string, f *ShadowStoredFlag, depth int) {
	if f == nil || depth == shadowStatusCallDepth {
		return
	}
	if dep := f.NativeCall; dep != nil {
		fmt.Fprintf(out, "      native-status %s call=%s -> %s %s reason=%s", name, shadowAddress(dep.Call.SitePC), shadowAddress(dep.Call.TargetPC), dep.ReturnKind, f.Reason)
		if s := dep.Call.Summary; s != nil {
			fmt.Fprintf(out, " C=%s D=%s returns=%d summary-blocker=%s", s.Carry, s.Decimal, len(s.Returns), s.Reason)
		}
		fmt.Fprintln(out, " (conditional native return)")
		writeShadowNativeFlag(out, name, dep.Before, depth+1)
	}
	writeShadowNativeFlag(out, name, f.EntrySource, depth+1)
}
