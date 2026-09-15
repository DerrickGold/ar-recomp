package tooling

import (
	"reflect"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestPublishedCursorIsAConditionalRootNotImmediateRefetch(t *testing.T) {
	for _, tt := range []struct {
		name      string
		slot      byte
		omit, hle bool
		want      bool
	}{
		{"paired saved cursor", 0x46, false, false, true},
		{"different field", 0x47, false, false, false},
		{"no reload", 0x46, true, false, false},
		{"HLE reload", 0x46, false, true, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results := commandWalkFixture(t, nil)
			fetch := results[2].commandStreams[0].FetchPC
			copy(image[0x800:], []byte{0x40, 0x89})
			copy(image[0x940:], []byte{0x98, 0x18, 0x69, 3, 0, 0x95, 0x46, 0x60})
			copy(image[0xb00:], []byte{0xb4, tt.slot, 0x88, 0x4c, byte(fetch), byte(fetch >> 8)})
			copy(image[0x1201f:], []byte{0, 0x80, 0, 0, 0, 0x81, 0, 0x93})
			image[0x1300] = 0x60
			var graphs []*decoder.Graph
			for _, r := range results {
				g, err := decoder.DecodeFunction(image, 0, uint16(r.entry.Address), 0, 0, decoder.Options{})
				if err != nil {
					t.Fatal(err)
				}
				graphs = append(graphs, g)
			}
			if !tt.omit {
				g, err := decoder.DecodeFunction(image, 0, 0x8b00, 0, 0, decoder.Options{})
				if err != nil {
					t.Fatal(err)
				}
				graphs = append(graphs, g)
			}
			configs := map[byte]*config.Config{}
			if tt.hle {
				configs[0] = &config.Config{HLEFunctions: map[uint16]string{0x8b00: "HostReload"}}
			}
			got := AnalyzeDecodedCommands(image, configs, graphs, graphs)
			if found := slices.Contains(got.Targets, uint32(0x9300)); found != tt.want {
				t.Fatalf("target recovered=%v want=%v, roots=%+v", found, tt.want, got.Roots)
			}
			if tt.want {
				found := false
				for _, r := range got.Roots {
					for _, ref := range r.References {
						if ref.FirstFetchPC != nil && *ref.FirstFetchPC == 0x02a023 && ref.PublishedResume != nil {
							p := ref.PublishedResume
							if p.Publication.PC != 0x8945 || p.Reload.PC != 0x8b00 || p.Reload.FetchDelta != -1 || p.EntryDecimalCondition != "clear" || p.NativeStop == "owned_native_refetch" {
								t.Fatalf("lost separate lifetime condition: %+v", p)
							}
							found = true
						}
					}
				}
				if !found {
					t.Fatal("no published-cursor provenance")
				}
				for _, w := range got.Walks {
					for _, s := range w.Steps {
						if s.NativePath != nil && len(s.NativePath.CursorPublications) > 0 && s.NextPC != nil {
							t.Fatal("publication mislabeled immediate refetch")
						}
					}
				}
			}
			slices.Reverse(graphs)
			if other := AnalyzeDecodedCommands(image, configs, graphs, graphs); !reflect.DeepEqual(got, other) {
				t.Fatal("worker order changes published-root closure")
			}
		})
	}
}
