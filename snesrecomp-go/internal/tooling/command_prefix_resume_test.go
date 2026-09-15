package tooling

import (
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestCallbackPrefixPublicationSurvivesCompleteCallback(t *testing.T) {
	for _, kind := range []string{"complete callback", "no reload", "different field", "HLE publisher", "HLE reload"} {
		t.Run(kind, func(t *testing.T) {
			image, results := commandWalkFixture(t, nil)
			fetch := results[2].commandStreams[0].FetchPC
			copy(image[0x800:], []byte{0x40, 0x89})
			// Publish the cursor before entering an unresolved callback. No claim
			// about its return is necessary for the later conditional reload root.
			copy(image[0x940:], []byte{0xb9, 0, 0, 0x85, 0x72, 0xc8, 0xc8, 0x94, 0x46, 0x5a, 0x8b, 0x4b, 0xab, 0xf4, 0x55, 0x89, 0x6c, 0x72, 0})
			copy(image[0x1200:], []byte{0xdc, 0x98, 0})
			copy(image[0xb00:], []byte{0xb4, 0x46, 0x88, 0x4c, byte(fetch), byte(fetch >> 8)})
			copy(image[0x1201f:], []byte{0, 0x80, 0, 0x92, 0x81, 0, 0x93})
			image[0x1300] = 0x60
			if kind == "different field" {
				image[0xb01] = 0x47
			}
			var graphs []*decoder.Graph
			for _, r := range results {
				g, err := decoder.DecodeFunction(image, 0, uint16(r.entry.Address), 0, 0, decoder.Options{})
				if err != nil {
					t.Fatal(err)
				}
				graphs = append(graphs, g)
			}
			if kind != "no reload" {
				g, err := decoder.DecodeFunction(image, 0, 0x8b00, 0, 0, decoder.Options{})
				if err != nil {
					t.Fatal(err)
				}
				graphs = append(graphs, g)
			}
			cfgs := map[byte]*config.Config{}
			if kind == "HLE publisher" {
				cfgs[0x80] = &config.Config{HLEFunctions: map[uint16]string{0x8940: "Host"}}
			}
			if kind == "HLE reload" {
				cfgs[0] = &config.Config{HLEFunctions: map[uint16]string{0x8b00: "Host"}}
			}
			got := AnalyzeDecodedCommands(image, cfgs, graphs, graphs)
			want := kind == "complete callback"
			if slices.Contains(got.Targets, uint32(0x9300)) != want {
				t.Fatalf("targets=%X want=%v", got.Targets, want)
			}
			if want {
				found := false
				for _, s := range got.Streams {
					for _, c := range s.Commands {
						if c.EntryPC == 0x8940 && c.Callback != nil && len(c.CursorStores) == 1 {
							found = true
						}
					}
				}
				if !found {
					t.Fatal("fixture did not exercise a complete callback prefix")
				}
				found = false
				for _, root := range got.Roots {
					for _, ref := range root.References {
						if p := ref.PublishedResume; p != nil && p.NativeStop == "command_prefix:callback_operand_dispatch" && p.Publication.PC == 0x8947 && p.Reload.PC == 0x8b00 && ref.FirstFetchPC != nil && *ref.FirstFetchPC == 0x02a022 {
							found = true
						}
					}
				}
				if !found {
					t.Fatal("lost prefix publication provenance")
				}
				for _, w := range got.Walks {
					for _, step := range w.Steps {
						if step.Handler == 0x8940 && step.NextPC != nil {
							t.Fatal("invented callback return")
						}
					}
				}
			}
		})
	}
}
