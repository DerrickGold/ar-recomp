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

// A pair of independently recovered indices anchors a pointer-record window.
// Probe at most 32 records in each direction from the ORIGINAL anchors, never
// recursively from a probe. Nulls, mapping/ownership boundaries and the first
// pointed stream stop each ray. This is an open neighbor inventory, not an
// argument value proof or a license to scan arbitrary stream bytes for tags.
func extendColdCommandPointerNeighbors(image rom.Image, roots []ShadowCommandRoot, a *shadowCallbackAnalyzer) []ShadowCommandRoot {
	out := append([]ShadowCommandRoot(nil), roots...)
	for n := range out {
		r := &out[n]
		bank, ok := shadowStreamConstantBank(r.TableRead.DataBank)
		if !ok {
			continue
		}
		if _, ok := shadowStreamConstantBank(r.StreamBank); !ok {
			continue
		}
		ops := r.TableRead.Index.Operations
		if len(ops) < 1 || len(ops) > 4 {
			continue
		}
		stride := 1
		for _, op := range ops {
			if op.Mnemonic != "ASL" {
				stride = 0
				break
			}
			stride *= 2
		}
		if stride < 2 {
			continue
		}
		anchors := map[int]bool{}
		seen := map[uint16]bool{}
		base, err := image.Offset(bank, uint16(r.TableRead.Operand))
		if err != nil {
			continue
		}
		end := base + 65536 - int(uint16(r.TableRead.Operand))
		for _, ref := range r.References {
			seen[ref.TableIndex] = true
			if ref.FirstFetchPC == nil || ref.StreamPC == nil || ref.Pointer == nil || *ref.Pointer == 0 || ref.TableIndex%uint16(stride) != 0 {
				continue
			}
			anchors[int(ref.TableIndex)] = true
			if off, ok := a.offset(*ref.FirstFetchPC); ok && off >= base && off < end {
				end = off
			}
		}
		if len(anchors) < 2 {
			continue
		}
		var indices []int
		for index := range anchors {
			indices = append(indices, index)
		}
		sort.Ints(indices)
		r.References = append([]ShadowCommandRootReference(nil), r.References...)
		for _, anchor := range indices {
			for _, direction := range []int{-1, 1} {
				for step := 1; step <= 32; step++ {
					index := anchor + direction*step*stride
					if index < 0 || index > 65535 || int(r.TableRead.Operand)+index > 65534 {
						break
					}
					pc := uint32(bank)<<16 | uint32(int(r.TableRead.Operand)+index)
					off, ok := a.offset(pc)
					if !ok || off < base || off+2 > end || a.starts[off] || a.interiors[off] || a.starts[off+1] || a.interiors[off+1] {
						break
					}
					ref := ShadowCommandRootReference{TableIndex: uint16(index)}
					resolveShadowStreamPointer(image, *r, &ref)
					if ref.FirstFetchPC == nil || ref.Pointer == nil || *ref.Pointer == 0 {
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
					ref.Status = "open_neighbor_ROM_stream_reference"
					r.References = append(r.References, ref)
				}
			}
		}
	}
	return out
}
