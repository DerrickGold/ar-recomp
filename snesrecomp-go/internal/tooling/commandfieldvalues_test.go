package tooling

import (
	"encoding/json"
	"reflect"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestCommandFieldValuesResumeRootedScript(t *testing.T) {
	for _, kind := range []string{"same field", "different field", "different object slot", "unknown carry", "decimal", "unknown operand", "unrooted reload", "HLE store", "HLE reload", "data callback"} {
		t.Run(kind, func(t *testing.T) {
			image, results := commandWalkFixture(t, nil)
			fetch := results[2].commandStreams[0].FetchPC
			// Store a continuation at object + a script-selected field offset,
			// then follow a second script. The reload independently selects the
			// same field and restores X before entering the common fetch.
			store := []byte{0xb9, 0, 0, 0x29, 0xff, 0, 0x18, 0x65, 0x70, 0xaa, 0xc8, 0xb9, 0, 0, 0xc8, 0xc8, 0x94, 0, 0xa8, 0xa6, 0x70, 0x88, 0x4c, byte(fetch), byte(fetch >> 8)}
			reload := []byte{0xb9, 0, 0, 0x29, 0xff, 0, 0x18, 0x65, 0x70, 0xaa, 0xb4, 0, 0xa6, 0x70, 0x88, 0x4c, byte(fetch), byte(fetch >> 8)}
			copy(image[0x800:], []byte{0x40, 0x89, 0x10, 0x89, 0x80, 0x89})
			copy(image[0x1201f:], []byte{0, 0x80, 0x2c, 0x40, 0xa0, 0x81, 0, 0x93})
			copy(image[0x1203f:], []byte{0, 0x82, 0x2c, 0})
			image[0x1300] = 0x60
			configs := map[byte]*config.Config{}
			switch kind {
			case "different field":
				image[0x12041] = 0x2d
			case "different object slot":
				reload[8] = 0x71
			case "unknown carry":
				store[6] = 0xea
			case "decimal":
				store = append([]byte{0xf8}, store...)
			case "unknown operand":
				store[0] = 0xbd // X is not an input seed
			case "unrooted reload":
				image[0x12040] = 1
			case "HLE store":
				configs[0] = &config.Config{HLEFunctions: map[uint16]string{0x8940: "HostStore"}}
			case "HLE reload":
				configs[0] = &config.Config{HLEFunctions: map[uint16]string{0x8980: "HostReload"}}
			case "data callback":
				configs[0] = &config.Config{DataRegions: []config.DataRegion{{Bank: 0, Start: 0x9300, End: 0x9300}}}
			}
			copy(image[0x940:], store)
			copy(image[0x980:], reload)
			var graphs []*decoder.Graph
			for _, pc := range []uint16{0x8000, 0x8100, 0x8200, 0x8910, 0x8940, 0x8980} {
				g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
				if err != nil {
					t.Fatal(err)
				}
				graphs = append(graphs, g)
			}
			before, _ := json.Marshal(graphs)
			got := AnalyzeDecodedCommands(image, configs, graphs, graphs)
			want := kind == "same field"
			if slices.Contains(got.Targets, uint32(0x9300)) != want {
				t.Fatalf("targets %X want %v; queries %+v", got.Targets, want, got.ValueQueries)
			}
			if want {
				found := false
				for _, r := range got.Roots {
					for _, ref := range r.References {
						if ref.PublishedResume == nil || ref.PublishedResume.FieldInput == nil {
							continue
						}
						f := ref.PublishedResume.FieldInput
						if f.BaseSlot != 0x70 || f.Offset != 0x2c || f.StoreInput.NativeEntryPC != 0x8940 || f.ReloadInput.NativeEntryPC != 0x8980 || ref.FirstFetchPC == nil || *ref.FirstFetchPC != 0x02a023 {
							t.Fatalf("lost field/input provenance: %+v %+v", ref, f)
						}
						found = true
					}
				}
				if !found {
					t.Fatal("no rebased field evidence")
				}
			}
			slices.Reverse(graphs)
			if other := AnalyzeDecodedCommands(image, configs, graphs, graphs); !reflect.DeepEqual(got, other) {
				t.Fatal("worker order changed field closure")
			}
			slices.Reverse(graphs)
			after, _ := json.Marshal(graphs)
			if string(before) != string(after) {
				t.Fatal("mutated inputs")
			}
		})
	}
}
