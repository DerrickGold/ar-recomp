package tooling

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

type ShadowCommandCursorReload struct {
	PC            uint32 `json:"pc"`
	Mode          string `json:"mode"`
	Operand       uint16 `json:"operand"`
	FetchDelta    int    `json:"cursor_delta_to_fetch"`
	IndexRestored bool   `json:"index_restored_before_fetch,omitempty"`
}

// A stored cursor is NOT a control-flow continuation. The field alias and
// lifetime are explicit conditions for a separate cold stream-root query.
type ShadowCommandPublishedResume struct {
	ParentFetchPC         uint32                    `json:"parent_fetch_pc"`
	Publication           ShadowCommandCursorStore  `json:"publication"`
	Reload                ShadowCommandCursorReload `json:"reload"`
	EntryDecimalCondition string                    `json:"conditional_entry_decimal,omitempty"`
	NativeStop            string                    `json:"native_path_stop"`
	Obligations           []string                  `json:"proof_obligations"`
	FieldInput            *ShadowCommandFieldInput  `json:"conditional_rebased_field,omitempty"`
	ConsumerDispatchPC    uint32                    `json:"deferred_consumer_dispatch_pc,omitempty"`
}

type coldCommandPosition struct{ selector, fetch uint32 }

// A later publication does not require replaying an already materialized
// suffix. All command/path effects at a fixed ROM cursor are conditional on
// the same entry state. A path-budget stop is different: it did not enumerate
// that command's alternatives and must remain eligible for a fresh query.
func coveredColdCommandPositions(walks []ShadowCommandWalk) map[coldCommandPosition]bool {
	covered, incomplete := map[coldCommandPosition]bool{}, map[coldCommandPosition]bool{}
	for _, w := range walks {
		for _, step := range w.Steps {
			covered[coldCommandPosition{w.SelectorPC, step.FetchPC}] = true
		}
		if w.StopReason == "stream_path_budget" || w.StopReason == "stream_command_budget" {
			incomplete[coldCommandPosition{w.SelectorPC, w.StopPC}] = true
		}
	}
	for p := range incomplete {
		delete(covered, p)
	}
	return covered
}

func collectColdCommandReloads(graphs []*decoder.Graph, streams []ShadowCommandStream) map[uint32][]ShadowCommandCursorReload {
	// Read-only instruction index; only existing decoded edges can cross an
	// ownership boundary. No missing block is decoded by this query.
	index := map[decoder.DecodeKey]*decoder.DecodedInstruction{}
	ambiguous := map[decoder.DecodeKey]bool{}
	for _, g := range graphs {
		for k, d := range g.Instructions {
			if k.M == 0 && k.X == 0 && k.PDepth == 0 {
				if ambiguous[k] {
					continue
				}
				if old := index[k]; old != nil {
					same := old.Instruction.Opcode == d.Instruction.Opcode && old.Instruction.Operand == d.Instruction.Operand && len(old.Successors) == len(d.Successors)
					if same {
						for n := range old.Successors {
							if old.Successors[n] != d.Successors[n] {
								same = false
								break
							}
						}
					}
					if !same {
						delete(index, k)
						ambiguous[k] = true
						continue
					}
				}
				index[k] = d
			}
		}
	}
	out := map[uint32][]ShadowCommandCursorReload{}
	for key, d := range index {
		i := d.Instruction
		if i.Mnemonic != "LDY" || (i.Mode != cpu65816.DPX && i.Mode != cpu65816.ABSX) {
			continue
		}
		for _, stream := range streams {
			at, delta, restored := d, 0, false
			for step := 0; step < 16; step++ {
				if len(at.Successors) != 1 {
					break
				}
				next := at.Successors[0]
				if next.M != 0 || next.X != 0 || next.PDepth != 0 {
					break
				}
				if next.PC == stream.FetchPC {
					out[stream.SelectorPC] = append(out[stream.SelectorPC], ShadowCommandCursorReload{key.PC, i.Mode.String(), uint16(i.Operand), delta, restored})
					break
				}
				at = index[next]
				if at == nil {
					break
				}
				switch at.Instruction.Mnemonic {
				case "INY":
					delta++
				case "DEY":
					delta--
				case "LDX":
					// The value has already been loaded into Y. Restoring the
					// object's X does not change it, but this new spelling needs
					// independently rooted field-index evidence before pairing.
					restored = true
				case "NOP", "BRA", "BRL":
				case "JMP":
					if at.Instruction.Mode != cpu65816.ABS {
						goto nextStream
					}
				default:
					goto nextStream
				}
			}
		nextStream:
		}
	}
	for key := range out {
		sort.Slice(out[key], func(i, j int) bool {
			a, b := out[key][i], out[key][j]
			if a.PC != b.PC {
				return a.PC < b.PC
			}
			return a.FetchDelta < b.FetchDelta
		})
	}
	return out
}

func expandColdCommandResumes(image rom.Image, inventory *DecodedCommandInventory, graphs []*decoder.Graph, a *shadowCallbackAnalyzer, values *coldCommandValueQueries) {
	reloads := collectColdCommandReloads(graphs, inventory.Streams)
	seen := map[coldCommandPosition]bool{}
	covered := map[coldCommandPosition]bool{}
	for _, r := range inventory.Roots {
		for _, ref := range r.References {
			if ref.FirstFetchPC != nil {
				seen[coldCommandPosition{r.SelectorPC, *ref.FirstFetchPC}] = true
			}
		}
	}
	wave := inventory.Walks
	// Iterate to quiescence, not an arbitrary script depth. Every nonempty
	// wave admits at least one previously unseen query position, and the
	// existing 4096-position budget bounds the entire fixed point.
	for round := 0; len(wave) != 0; round++ {
		for p := range coveredColdCommandPositions(wave) {
			covered[p] = true
		}
		var roots []ShadowCommandRoot
		var valueRoots []ShadowCommandRoot
		if values != nil {
			valueRoots = values.discover(wave)
			valueRoots = append(valueRoots, values.rebasedCursorRoots(wave, reloads)...)
		}
		for _, w := range wave {
			for _, s := range inventory.Streams {
				if s.SelectorPC != w.SelectorPC {
					continue
				}
				for _, step := range w.Steps {
					type write struct {
						p             ShadowCommandCursorStore
						base          int
						decimal, stop string
						consumer      uint32
					}
					var writes []write
					// Callback scouts begin at the callback, not at its command
					// prefix. Preserve stores already summarized before that edge;
					// discovering a more complete callback must not lose them.
					for _, command := range s.Commands {
						if command.Index != step.Index || command.EntryPC != step.Handler {
							continue
						}
						for _, p := range command.CursorStores {
							writes = append(writes, write{p, int(uint16(step.FetchPC)) - s.TagByteOffset + 1 + s.EntryCursorDelta, "", "command_prefix:" + command.StopReason, 0})
						}
					}
					if step.DataPath != nil {
						for _, p := range step.DataPath.Publications {
							writes = append(writes, write{p, int(uint16(step.FetchPC)) - s.TagByteOffset + 1, "", step.DataPath.Status, 0})
						}
					}
					for _, path := range []*ShadowCommandCallbackPath{step.NativePath, step.CallbackPath} {
						if path != nil {
							for _, p := range path.CursorPublications {
								writes = append(writes, write{p, int(uint16(step.FetchPC)) - s.TagByteOffset + 1 + s.EntryCursorDelta, path.EntryDecimalCondition, path.Status, 0})
							}
						}
					}
					// A deferred consumer's Y is not this command's cursor. Only a
					// literal cursor from its callback names a stream position.
					for _, consumer := range step.ConsumerPaths {
						for _, p := range consumer.Path.CursorPublications {
							if p.Literal {
								writes = append(writes, write{p, 0, "", consumer.Path.Status, consumer.DispatchPC})
							}
						}
					}
					for _, wr := range writes {
						for _, load := range reloads[s.SelectorPC] {
							if load.IndexRestored || wr.p.Mode != load.Mode || wr.p.Operand != load.Operand {
								continue
							}
							cursor := wr.base + wr.p.CursorDelta
							if wr.p.Literal {
								cursor = int(wr.p.Value)
							}
							fetch := cursor + load.FetchDelta + s.TagByteOffset - 1
							if cursor < 0 || cursor > 65535 || fetch < 0 || fetch > 65534 {
								continue
							}
							pc := step.FetchPC&0xff0000 | uint32(fetch)
							key := coldCommandPosition{s.SelectorPC, pc}
							if seen[key] || covered[key] {
								continue
							}
							if len(seen) >= 4096 {
								inventory.ResumeTruncated = true
								continue
							}
							if _, ok := shadowStreamROMWord(image, byte(pc>>16), uint32(fetch)); !ok {
								continue
							}
							seen[key] = true
							pointer := uint16(cursor)
							streamPC := pc&0xff0000 | uint32(pointer)
							status, obligations := "conditional_published_cursor_reload", []string{"conditional_native_publication_not_a_return", "D_DB_X_field_alias_and_lifetime_unproven", "publication_must_reach_reload_without_overwrite", "reload_stream_bank_must_match_parent_stream", "no_frame_skip_or_closed_entry_promotion"}
							if wr.consumer != 0 {
								status = "conditional_consumer_published_cursor_reload"
								obligations = append(obligations, "deferred_field_must_reach_consumer_invocation", "consumer_required_branches_not_proven_feasible")
							}
							evidence := &ShadowCommandPublishedResume{ParentFetchPC: step.FetchPC, Publication: wr.p, Reload: load, EntryDecimalCondition: wr.decimal, NativeStop: wr.stop, Obligations: obligations, ConsumerDispatchPC: wr.consumer}
							roots = append(roots, ShadowCommandRoot{SelectorPC: s.SelectorPC, FetchPC: s.FetchPC, FetchOperand: uint16(s.TagByteOffset - 1), FetchCursorDelta: load.FetchDelta, References: []ShadowCommandRootReference{{Pointer: &pointer, StreamPC: &streamPC, FirstFetchPC: &pc, Status: status, PublishedResume: evidence}}, ProofObligations: evidence.Obligations})
						}
					}
				}
			}
		}
		if values != nil {
			for _, r := range valueRoots {
				pc := *r.References[0].FirstFetchPC
				key := coldCommandPosition{r.SelectorPC, pc}
				if seen[key] || covered[key] {
					continue
				}
				if len(seen) >= 4096 {
					inventory.ResumeTruncated = true
					continue
				}
				seen[key] = true
				roots = append(roots, r)
			}
		}
		if len(roots) == 0 {
			break
		}
		inventory.ResumeRounds = round + 1
		sort.Slice(roots, func(i, j int) bool {
			if roots[i].SelectorPC != roots[j].SelectorPC {
				return roots[i].SelectorPC < roots[j].SelectorPC
			}
			return *roots[i].References[0].FirstFetchPC < *roots[j].References[0].FirstFetchPC
		})
		base := len(inventory.Roots)
		wave = walkShadowCommandStreamsWithCallbacks(image, inventory.Streams, roots, a)
		for n := range wave {
			for m := range wave[n].Origins {
				wave[n].Origins[m].Root += base
			}
		}
		inventory.Roots = append(inventory.Roots, roots...)
		inventory.Walks = append(inventory.Walks, wave...)
	}
}
