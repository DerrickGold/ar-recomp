package tooling

import (
	"encoding/json"
	"reflect"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestCommandValuesReenterInitializerWithoutBorrowingStack(t *testing.T) {
	for _, kind := range []string{"two script replacements", "unrooted", "HLE command", "HLE initializer", "data target", "borrow A", "borrow DB", "known wrong DB"} {
		t.Run(kind, func(t *testing.T) {
			image, results := commandWalkFixture(t, nil)
			// Replace the current stream with a table-selected one. PLB restores
			// an opaque caller-owned byte; this is not a complete refetch proof.
			command := []byte{0xb9, 0, 0, 0xab, 0x4c, 0, 0x82}
			configs := map[byte]*config.Config{}
			switch kind {
			case "unrooted":
				image[0] = 0x60
			case "HLE command":
				configs[0] = &config.Config{HLEFunctions: map[uint16]string{0x8900: "Host"}}
			case "HLE initializer":
				configs[0] = &config.Config{HLEFunctions: map[uint16]string{0x8200: "Host"}}
			case "data target":
				configs[0] = &config.Config{DataRegions: []config.DataRegion{{Bank: 0, Start: 0x9300, End: 0x9300}}}
			case "borrow A":
				command = []byte{0xb9, 0, 0, 0x68, 0x4c, 0, 0x82}
			case "borrow DB":
				command = []byte{0xab, 0xb9, 0, 0, 0x4c, 0, 0x82}
			case "known wrong DB":
				command = []byte{0x4b, 0xab, 0xb9, 0, 0, 0x4c, 0, 0x82}
			}
			copy(image[0x900:], command)
			copy(image[0x11010:], []byte{0x40, 0xa0})
			copy(image[0x11014:], []byte{0x60, 0xa0})
			copy(image[0x1201f:], []byte{0, 0x80, 4, 0})
			copy(image[0x1203f:], []byte{0, 0x80, 5, 0})
			copy(image[0x1205f:], []byte{0, 0x81, 0, 0x93})
			image[0x1300] = 0x60
			var graphs []*decoder.Graph
			options := decoder.Options{SiblingEntryPCs: map[uint16]struct{}{0x8200: {}, 0x8900: {}, 0x8910: {}}}
			for _, r := range results {
				g, err := decoder.DecodeFunction(image, 0, uint16(r.entry.Address), 0, 0, options)
				if err != nil {
					t.Fatal(err)
				}
				graphs = append(graphs, g)
			}
			for _, pc := range []uint16{0x8900, 0x8910} {
				g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, options)
				if err != nil {
					t.Fatal(err)
				}
				graphs = append(graphs, g)
			}
			before, _ := json.Marshal(graphs)
			got := AnalyzeDecodedCommands(image, configs, graphs, graphs)
			want := kind == "two script replacements"
			if slices.Contains(got.Targets, uint32(0x9300)) != want {
				t.Fatalf("targets %X want=%v; queries%+v", got.Targets, want, got.ValueQueries)
			}
			if want {
				found := map[uint32]bool{}
				for _, r := range got.Roots {
					for _, ref := range r.References {
						if ref.ValueInput != nil && ref.FirstFetchPC != nil {
							found[*ref.FirstFetchPC] = true
							if len(ref.ValueInput.CallSites) == 0 || ref.ValueInput.NativeEntryPC != 0x8900 || ref.ValueInput.EntryDB != 2 {
								t.Fatalf("lost command input provenance: %+v", ref.ValueInput)
							}
						}
					}
				}
				if !found[0x02a03f] || !found[0x02a05f] {
					t.Fatalf("did not reach command/value fixed point: %v", found)
				}
				stopped := false
				for _, w := range got.Walks {
					if w.StopReason == "opaque_caller_stack_read" {
						stopped = true
					}
				}
				if !stopped {
					t.Fatal("value evidence erased the unresolved native stack boundary")
				}
			}
			slices.Reverse(graphs)
			if other := AnalyzeDecodedCommands(image, configs, graphs, graphs); !reflect.DeepEqual(got, other) {
				t.Fatal("worker order changed seeded value closure")
			}
			slices.Reverse(graphs)
			after, _ := json.Marshal(graphs)
			if string(before) != string(after) {
				t.Fatal("analysis mutated decoded graphs")
			}
		})
	}
}
