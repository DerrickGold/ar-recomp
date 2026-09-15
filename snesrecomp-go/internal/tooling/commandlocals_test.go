package tooling

import (
	"bytes"
	"encoding/json"
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestCommandLocalEntryBackedgesAreNotInitializers(t *testing.T) {
	for _, tt := range []struct {
		name   string
		code   []byte
		load   uint32
		reason string
	}{
		{"load at external entry", []byte{0xaf, 0x40, 0x10, 0x7e, 0xa9, 3, 0, 0x8f, 0x40, 0x10, 0x7e, 0x80, 0xf3}, 0x8000, "local_input_entry_or_ambiguous_predecessor"},
		{"entry store value from later iteration", []byte{0x8f, 0x40, 0x10, 0x7e, 0xaf, 0x40, 0x10, 0x7e, 0xa9, 3, 0, 0x80, 0xf3}, 0x8004, "local_input_value_crosses_entry_or_join"},
		{"entry index from later iteration", []byte{0xbf, 0, 0x10, 0x7e, 0xa2, 0x40, 0, 0xa9, 3, 0, 0x9f, 0, 0x10, 0x7e, 0x80, 0xf0}, 0x8000, "local_input_index_unproven"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, _ := commandRootFixture(t, []byte{0x60}, []byte{0x60}, false)
			copy(image, tt.code)
			g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
			if err != nil {
				t.Fatal(err)
			}
			walk := shadowPointerWalk{graph: g, preds: shadowPredecessors(g)}
			key := decoder.DecodeKey{PC: tt.load}
			r, ok := collectShadowStreamLocalRead(walk, ShadowInitializerIndex{Source: ShadowStoredOrigin{Kind: "load", Register: "A", PC: tt.load}, sourceKey: key}, 0)
			if !ok || r.Issue != tt.reason {
				t.Fatalf("backedge escaped: %+v want %s", r, tt.reason)
			}
		})
	}
}

func TestCommandLocalQueryBudgetsAndCallerValues(t *testing.T) {
	for _, count := range []int{shadowStreamLocalStoreLimit - 1, shadowStreamLocalStoreLimit} {
		code := []byte{0xa9, 3, 0, 0x8f, 0x40, 0x10, 0x7e}
		code = append(code, bytes.Repeat([]byte{0xea}, count)...)
		code = append(code, 0xaf, 0x40, 0x10, 0x7e, 0xaa, 0x20, 0, 0x81, 0x60)
		image, results := commandRootFixture(t, code, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
		got := resolveShadowCommandRoots(image, results)[0]
		if count == shadowStreamLocalStoreLimit-1 {
			if len(got.References) != 1 || got.Truncated {
				t.Fatalf("lost budget edge: %+v", got)
			}
		} else if len(got.References) != 0 || !got.Truncated || !slices.ContainsFunc(got.Unresolved, func(i ShadowCommandRootIssue) bool { return i.Reason == "local_input_store_budget" }) {
			t.Fatalf("store search unbounded: %+v", got)
		}
	}
	for _, count := range []int{shadowStreamInputReadDepth, shadowStreamInputReadDepth + 1} {
		code := []byte{0xa9, 3, 0}
		for n := range count {
			code = append(code, 0x8f, byte(0x40+2*n), 0x10, 0x7e, 0xaf, byte(0x40+2*n), 0x10, 0x7e)
		}
		code = append(code, 0xaa, 0x20, 0, 0x81, 0x60)
		image, results := commandRootFixture(t, code, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
		got := resolveShadowCommandRoots(image, results)[0]
		if count == shadowStreamInputReadDepth {
			if len(got.References) != 1 || len(got.References[0].LocalInputs) != count || got.Truncated {
				t.Fatalf("lost nested chain: %+v", got)
			}
		} else if len(got.References) != 0 || !got.Truncated {
			t.Fatalf("nested chain unbounded: %+v", got)
		}
	}
	// The word, not an object address, may come from the exact direct caller.
	image, results := commandRootFixture(t, []byte{0xa9, 3, 0, 0x20, 0, 0x81, 0x60}, []byte{0x8f, 0x40, 0x10, 0x7e, 0xa9, 0xff, 0xff, 0xaf, 0x40, 0x10, 0x7e, 0x22, 0, 0x82, 0x80, 0x60}, false)
	got := resolveShadowCommandRoots(image, results)[0]
	if len(got.References) != 1 || len(got.References[0].LocalInputs) != 1 || len(got.References[0].Calls) != 2 || got.References[0].TableIndex != 12 {
		t.Fatalf("lost caller value through local store: %+v", got)
	}
}

func TestCommandLocalWRAMInputs(t *testing.T) {
	setup := []byte{0xa9, 0, 0, 0x5b, 0xa2, 0, 0x10} // D=0; X=$1000 (native word index)
	for _, tt := range []struct {
		name            string
		body            []byte
		count, disjoint int
		rom             bool
		value           uint16
	}{
		{"indexed object", []byte{0xa9, 3, 0, 0x95, 0x40, 0xa9, 0xff, 0xff, 0xb5, 0x40}, 1, 0, false, 3},
		{"disjoint field", []byte{0xa9, 3, 0, 0x95, 0x40, 0x74, 0x50, 0xa9, 0xff, 0xff, 0xb5, 0x40}, 1, 1, false, 3},
		{"other WRAM bank is disjoint", []byte{0xa9, 3, 0, 0x95, 0x40, 0xa9, 5, 0, 0x8f, 0x40, 0x10, 0x7f, 0xb5, 0x40}, 1, 1, false, 3},
		{"WRAM mirror alias", []byte{0xa9, 3, 0, 0x8f, 0x40, 0x10, 0x7e, 0xb5, 0x40}, 1, 0, false, 3},
		{"different D and X same address", []byte{0xa9, 3, 0, 0x95, 0x40, 0xa9, 0, 1, 0x5b, 0xa2, 0, 0x0f, 0xb5, 0x40}, 1, 0, false, 3},
		{"local field chain", []byte{0xa9, 3, 0, 0x95, 0x40, 0xb5, 0x40, 0x95, 0x44, 0xb5, 0x44}, 2, 0, false, 3},
		{"ROM loaded then stored", []byte{0xaf, 0, 0xb0, 3, 0x95, 0x40, 0xb5, 0x40}, 1, 0, true, 3},
		{"zero overwrites old word", []byte{0xa9, 3, 0, 0x95, 0x40, 0x74, 0x40, 0xb5, 0x40}, 1, 0, false, 0},
		{"nearest full overwrite", []byte{0xa9, 5, 0, 0x95, 0x40, 0xa9, 3, 0, 0x95, 0x40, 0xb5, 0x40}, 1, 0, false, 3},
	} {
		t.Run(tt.name, func(t *testing.T) {
			caller := append(slices.Clone(setup), tt.body...)
			caller = append(caller, 0xaa, 0x20, 0, 0x81, 0x60)
			image, results := commandRootFixture(t, caller, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
			copy(image[3*0x8000+0x3000:], []byte{3, 0})
			copy(image[2*0x8000+0x1000:], []byte{0x20, 0xa0}) // zero ID is a valid synthetic stream
			got := resolveShadowCommandRoots(image, results)
			if len(got) != 1 || len(got[0].References) != 1 || len(got[0].Unresolved) != 0 {
				t.Fatalf("lost local input: %+v", got)
			}
			p := got[0].References[0]
			if p.Status != "local_WRAM_call_path_stream_reference" || p.TableIndex != tt.value*4 || p.StreamPC == nil || *p.StreamPC != 0x02a020 || len(p.LocalInputs) != tt.count {
				t.Fatalf("wrong value/provenance: %+v", p)
			}
			if (len(p.ROMReads) != 0) != tt.rom {
				t.Fatalf("lost ROM origin: %+v", p)
			}
			if tt.value == 0 && p.LiteralValue != nil {
				t.Fatal("STZ invented a literal load")
			}
			first := p.LocalInputs[0]
			if first.Word != tt.value || first.LoadAddress.WRAMOffset != 0x1040 || first.StoreAddress.WRAMOffset != 0x1040 || len(first.DisjointWrites) != tt.disjoint {
				t.Fatalf("bad exact-address witness: %+v", first)
			}
			if tt.name == "WRAM mirror alias" && first.StoreAddress.BusPC != 0x7e1040 {
				t.Fatal("lost distinct bus spelling")
			}
			slices.Reverse(results)
			if !reflect.DeepEqual(got, resolveShadowCommandRoots(image, results)) {
				t.Fatal("worker order changes local query")
			}
			report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandStreamRoots: got}
			facts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
			if len(facts) != 0 {
				t.Fatal("conditional local memory entered generation facts")
			}
			b, _ := json.Marshal(report)
			var round ShadowReport
			if err := json.Unmarshal(b, &round); err != nil {
				t.Fatal(err)
			}
			b2, _ := json.Marshal(round)
			if !bytes.Equal(b, b2) {
				t.Fatal("local evidence lost in JSON")
			}
			var out bytes.Buffer
			writeShadowCommandRoots(&out, got)
			if !strings.Contains(out.String(), "local-WRAM store=") || !strings.Contains(out.String(), "no_interrupt_HLE_or_external_memory_interference") {
				t.Fatal(out.String())
			}
		})
	}
}

func TestCommandLocalAliasAndLifetimeBarriers(t *testing.T) {
	for _, tt := range []struct {
		name   string
		body   []byte
		reason string
	}{
		{"different object", []byte{0xa2, 0, 0x11}, "local_input_entry_or_ambiguous_predecessor"},
		{"unknown object", []byte{0xa6, 0x70}, "local_input_index_unproven"},
		{"different D", []byte{0xa9, 0, 1, 0x5b, 0xa2, 0, 0x10}, "local_input_entry_or_ambiguous_predecessor"},
		{"unknown D", []byte{0xa5, 0x20, 0x5b}, "local_input_D_unproven"},
		{"partial overlap upper byte", []byte{0x74, 0x41}, "local_input_partial_alias_write"},
		{"partial overlap lower byte", []byte{0x74, 0x3f}, "local_input_partial_alias_write"},
		{"read modify write", []byte{0xf6, 0x40, 0xa2, 0, 0x10}, "local_input_RMW_alias_write"},
		{"unknown alias", []byte{0x91, 0x10}, "local_input_possible_alias_write:local_input_address_mode_unproven"},
		{"DMA register write", []byte{0x8f, 0x0b, 0x42, 0}, "local_input_possible_alias_write:local_input_not_WRAM"},
		{"native call", []byte{0x20, 0, 0x89}, "local_input_D_unproven"},
		{"stack write", []byte{0x48}, "local_input_memory_barrier_PHA"},
		{"stack pull", []byte{0x68, 0xa2, 0, 0x10}, "local_input_memory_barrier_PLA"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			caller := []byte{0xa9, 0, 0, 0x5b, 0xa2, 0, 0x10, 0xa9, 3, 0, 0x95, 0x40}
			caller = append(caller, tt.body...)
			caller = append(caller, 0xb5, 0x40, 0xaa, 0x20, 0, 0x81, 0x60)
			image, results := commandRootFixture(t, caller, []byte{0x8a, 0x22, 0, 0x82, 0x80, 0x60}, false)
			got := resolveShadowCommandRoots(image, results)
			if len(got) != 1 || len(got[0].References) != 0 || !slices.ContainsFunc(got[0].Unresolved, func(i ShadowCommandRootIssue) bool { return i.Reason == tt.reason }) {
				t.Fatalf("lost %s: %+v", tt.reason, got)
			}
		})
	}
}

func TestCommandLocalConcreteMappingAndBarriers(t *testing.T) {
	for _, tt := range []struct {
		name   string
		code   []byte
		mxM    uint8
		reason string
	}{
		{"hardware is not a slot", []byte{0xa9, 0, 0, 0x5b, 0xa2, 0, 0x21, 0xa9, 3, 0, 0x95, 0, 0xb5, 0}, 0, "local_input_not_WRAM"},
		{"mirror word straddles hardware", []byte{0xa9, 0, 0, 0x5b, 0xa2, 0xff, 0x1f, 0xa9, 3, 0, 0x95, 0, 0xb5, 0}, 0, "local_input_not_WRAM"},
		{"bank wrap", []byte{0xa9, 0, 0, 0x5b, 0xa2, 0xff, 0xff, 0xa9, 3, 0, 0x95, 1, 0xb5, 1}, 0, "local_input_bank_boundary"},
		{"byte overwrite", []byte{0xf4, 0, 0, 0xab, 0xab, 0xa2, 3, 0, 0x8e, 0x40, 0x10, 0xa9, 5, 0x8d, 0x40, 0x10, 0xae, 0x40, 0x10}, 1, "local_input_partial_alias_write"},
		{"call through fixed long slot", []byte{0xa9, 3, 0, 0x8f, 0x40, 0x10, 0x7e, 0x20, 0, 0x89, 0xaf, 0x40, 0x10, 0x7e}, 0, "local_input_memory_barrier_JSR"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, _ := commandRootFixture(t, []byte{0x60}, []byte{0x60}, false)
			copy(image, append(slices.Clone(tt.code), 0xea, 0x60))
			g, err := decoder.DecodeFunction(image, 0, 0x8000, tt.mxM, 0, decoder.Options{})
			if err != nil {
				t.Fatal(err)
			}
			walk := shadowPointerWalk{graph: g, preds: shadowPredecessors(g)}
			key := decoder.DecodeKey{PC: 0x8000 + uint32(len(tt.code)), M: tt.mxM}
			reg := "A"
			if tt.mxM == 1 {
				reg = "X"
			}
			expr := walk.indexExpression(key, reg, true)
			r, attempt := collectShadowStreamLocalRead(walk, expr, 0)
			if !attempt {
				_, reason := shadowStreamWRAMAddress(walk, expr.sourceKey, 2)
				if reason != tt.reason {
					t.Fatalf("address reason=%s expr=%+v", reason, expr)
				}
			} else if r.Issue != tt.reason {
				t.Fatalf("issue=%s want=%s", r.Issue, tt.reason)
			}
		})
	}
}

func TestCommandLocalDoesNotJoinMemoryAcrossCalls(t *testing.T) {
	image, results := commandRootFixture(t, []byte{0xa9, 3, 0, 0x8f, 0x40, 0x10, 0x7e, 0x20, 0, 0x81, 0x60}, []byte{0xaf, 0x40, 0x10, 0x7e, 0x22, 0, 0x82, 0x80, 0x60}, false)
	got := resolveShadowCommandRoots(image, results)[0]
	if len(got.References) != 0 || !slices.ContainsFunc(got.Unresolved, func(i ShadowCommandRootIssue) bool { return i.Reason == "local_input_entry_or_ambiguous_predecessor" }) {
		t.Fatalf("cross-call memory lifetime guessed: %+v", got)
	}
}
