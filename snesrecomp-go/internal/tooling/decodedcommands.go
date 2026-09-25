package tooling

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// DecodedCommandInventory reuses the report's conditional native command
// analysis over a caller's current decoded closure. It performs no I/O and
// imports no observations. These are open cold candidates, never proven entry
// facts, closed dispatch domains, or evidence of gameplay reachability.
type DecodedCommandInventory struct {
	ValueQueries    *ShadowCommandValueSummary `json:"shared_value_queries,omitempty"`
	ResumeRounds    int                        `json:"published_cursor_rounds"`
	ResumeTruncated bool                       `json:"published_cursor_truncated"`
	Streams         []ShadowCommandStream      `json:"streams"`
	Roots           []ShadowCommandRoot        `json:"roots"`
	Walks           []ShadowCommandWalk        `json:"walks"`
	Targets         []uint32                   `json:"cold_targets"`
}

// AnalyzeDecodedCommands keeps graph ownership separate from admission:
// graphs supplies decoded ownership, while eligible supplies native bodies
// whose dataflow may participate. The caller must exclude HLE/body/width
// overrides from eligible. All configs still participate in target blockers.
func AnalyzeDecodedCommands(image rom.Image, configs map[byte]*config.Config, graphs, eligible []*decoder.Graph) DecodedCommandInventory {
	ordered := append([]*decoder.Graph(nil), graphs...)
	sort.Slice(ordered, func(i, j int) bool {
		a, b := ordered[i].Entry, ordered[j].Entry
		if a.PC != b.PC {
			return a.PC < b.PC
		}
		if a.M != b.M {
			return a.M < b.M
		}
		return a.X < b.X
	})
	allowed := map[*decoder.Graph]bool{}
	for _, g := range eligible {
		allowed[g] = true
	}
	var banks []shadowBank
	for id, cfg := range configs {
		banks = append(banks, shadowBank{ID: id, Config: cfg})
	}
	sort.Slice(banks, func(i, j int) bool { return banks[i].ID < banks[j].ID })
	policies := newShadowCallbackAnalyzer(image, banks, nil, nil)
	var results []shadowDecodeResult
	var nativeGraphs []*decoder.Graph
	for _, g := range ordered {
		r := shadowDecodeResult{
			entry:        decoder.Variant{Address: g.Entry.PC, M: g.Entry.M, X: g.Entry.X},
			instructions: shadowDecodedInstructions(byte(g.Entry.PC>>16), uint16(g.Entry.PC), g),
		}
		native := allowed[g]
		if len(policies.blocked) != 0 {
			for _, d := range r.instructions {
				for n := 0; n < int(d.Instruction.Length); n++ {
					if off, ok := policies.offset(d.PC + uint32(n)); ok && policies.blocked[off] != "" {
						native = false
					}
				}
			}
		}
		if native {
			nativeGraphs = append(nativeGraphs, g)
			cfg := configs[byte(g.Entry.PC>>16)]
			if cfg == nil {
				cfg = &config.Config{}
			}
			s := collectShadowCommandStreams(g)
			calls := collectShadowDirectCallInputs(g)
			r.commandStreams = s
			r.commandPrefix = summarizeShadowCommandPrefix(g, g.Entry, 0)
			r.commandPaths = collectShadowCommandPaths(g, cfg, s)
			r.commandDataPaths = collectShadowCommandDataPaths(g, cfg, s)
			r.commandRoots = collectShadowCommandRoots(g, s)
			r.streamInputs = collectShadowStreamInputs(g, calls)
			r.statusBody = collectShadowStatusBody(g, cfg)
			r.forwardedFields = decoder.ForwardedIndirectFields(g)
		}
		results = append(results, r)
	}
	r := DecodedCommandInventory{Streams: mergeShadowCommandStreams(results), Roots: resolveShadowCommandRoots(image, results)}
	a := newShadowCallbackAnalyzer(image, banks, results, nil)
	a.nativeCommands = true
	r.Roots = extendColdCommandPointerNeighbors(image, r.Roots, a)
	r.Walks = walkShadowCommandStreamsWithCallbacks(image, r.Streams, r.Roots, a)
	values := newColdCommandValueQueries(image, configs, graphs, nativeGraphs, &r)
	expandColdCommandResumes(image, &r, nativeGraphs, a, values)
	r.ValueQueries = values.summary()
	seen := map[uint32]bool{}
	for _, w := range r.Walks {
		for _, step := range w.Steps {
			for _, operand := range step.Operands {
				for _, target := range operand.Candidates {
					// A cold candidate may conflict with an existing speculative
					// decode, but must never bypass an authored data/HLE boundary.
					off, ok := a.offset(target.PC)
					if !ok || a.blocked[off] != "" {
						continue
					}
					i, err := decodeShadowInstruction(image, byte(target.PC>>16), uint16(target.PC), 0, 0)
					if err != nil || i.Opcode == 0 || i.Opcode == 0xff {
						continue
					}
					valid := true
					for n := 0; n < int(i.Length); n++ {
						p, ok := a.offset(target.PC + uint32(n))
						if !ok || p != off+n || a.blocked[p] != "" {
							valid = false
							break
						}
					}
					if valid {
						seen[target.PC] = true
					}
				}
			}
		}
	}
	for pc := range seen {
		r.Targets = append(r.Targets, pc)
	}
	sort.Slice(r.Targets, func(i, j int) bool { return r.Targets[i] < r.Targets[j] })
	return r
}

// Independently recovered, stride-aligned indices of one pointer table anchor
// an open stream-reference inventory. Anchors pool across contextual roots only
// when the table (constant data bank, base operand, shift stride) and its
// stream interpretation (constant stream bank, selector, fetch and cursor
// arithmetic) are identical, and only ORIGINAL references pool. Between two
// adjacent anchors every aligned slot belongs to the same indexed table; beyond
// the outermost anchors probe at most 32 records, never recursively from a
// probe. A null slot is an unused table entry: it is skipped, never referenced,
// and still counts toward the outer window. Mapping/ownership boundaries, a
// nonzero slot that is not a mapped stream, and the first pointed stream stop
// each ray. This is an open inventory, not an argument value proof or a license
// to scan arbitrary stream bytes for tags.
func extendColdCommandPointerNeighbors(image rom.Image, roots []ShadowCommandRoot, a *shadowCallbackAnalyzer) []ShadowCommandRoot {
	type pointerTable struct {
		bank, streamBank                       byte
		operand, selector, fetch, fetchOperand uint32
		stride, delta                          int
	}
	identity := func(r *ShadowCommandRoot) (pointerTable, bool) {
		bank, ok := shadowStreamConstantBank(r.TableRead.DataBank)
		if !ok {
			return pointerTable{}, false
		}
		streamBank, ok := shadowStreamConstantBank(r.StreamBank)
		if !ok {
			return pointerTable{}, false
		}
		ops := r.TableRead.Index.Operations
		if len(ops) < 1 || len(ops) > 4 {
			return pointerTable{}, false
		}
		stride := 1
		for _, op := range ops {
			if op.Mnemonic != "ASL" {
				return pointerTable{}, false
			}
			stride *= 2
		}
		return pointerTable{bank, streamBank, uint32(uint16(r.TableRead.Operand)), uint32(r.SelectorPC), uint32(r.FetchPC), uint32(r.FetchOperand), stride, int(r.FetchCursorDelta)}, true
	}
	anchors := map[pointerTable]map[int]bool{}
	fetches := map[pointerTable][]uint32{}
	for n := range roots {
		t, ok := identity(&roots[n])
		if !ok {
			continue
		}
		for _, ref := range roots[n].References {
			if ref.FirstFetchPC == nil || ref.StreamPC == nil || ref.Pointer == nil || *ref.Pointer == 0 || ref.TableIndex%uint16(t.stride) != 0 {
				continue
			}
			if anchors[t] == nil {
				anchors[t] = map[int]bool{}
			}
			anchors[t][int(ref.TableIndex)] = true
			fetches[t] = append(fetches[t], *ref.FirstFetchPC)
		}
	}
	out := append([]ShadowCommandRoot(nil), roots...)
	for n := range out {
		r := &out[n]
		t, ok := identity(r)
		if !ok || len(anchors[t]) < 2 {
			continue
		}
		base, err := image.Offset(t.bank, uint16(t.operand))
		if err != nil {
			continue
		}
		end := base + 65536 - int(t.operand)
		for _, pc := range fetches[t] {
			if off, ok := a.offset(pc); ok && off >= base && off < end {
				end = off
			}
		}
		seen := map[uint16]bool{}
		for _, ref := range r.References {
			seen[ref.TableIndex] = true
		}
		var indices []int
		for index := range anchors[t] {
			indices = append(indices, index)
		}
		sort.Ints(indices)
		r.References = append([]ShadowCommandRootReference(nil), r.References...)
		for i, anchor := range indices {
			for _, direction := range []int{-1, 1} {
				steps, status := 32, "open_neighbor_ROM_stream_reference"
				if next := i + direction; next >= 0 && next < len(indices) {
					steps, status = (indices[next]-anchor)*direction/t.stride, "open_interpolated_ROM_stream_reference"
				}
				for step := 1; step <= steps; step++ {
					index := anchor + direction*step*t.stride
					if index < 0 || int(t.operand)+index > 65534 {
						break
					}
					pc := uint32(t.bank)<<16 | uint32(int(t.operand)+index)
					off, ok := a.offset(pc)
					if !ok || off < base || off+2 > end || a.starts[off] || a.interiors[off] || a.starts[off+1] || a.interiors[off+1] {
						break
					}
					ref := ShadowCommandRootReference{TableIndex: uint16(index)}
					resolveShadowStreamPointer(image, *r, &ref)
					if ref.Pointer != nil && *ref.Pointer == 0 {
						continue
					}
					if ref.FirstFetchPC == nil {
						break
					}
					if stream, ok := a.offset(*ref.FirstFetchPC); ok && stream >= base && stream < end {
						end = stream
						if off+2 > end {
							break
						}
					}
					if seen[ref.TableIndex] {
						continue
					}
					if len(r.References) >= 2048 {
						r.Truncated = true
						break
					}
					seen[ref.TableIndex] = true
					ref.Status = status
					r.References = append(r.References, ref)
				}
			}
		}
	}
	return out
}
