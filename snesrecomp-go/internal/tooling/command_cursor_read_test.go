package tooling

import (
	"reflect"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestRootedROMCursorReplacement(t *testing.T) {
	for _, kind := range []string{"word", "saved word", "unknown Y", "unknown word", "changed bank", "unbalanced stack", "unmapped word", "cursor underflow", "read bank boundary", "pointer cycle", "HLE command", "data target"} {
		t.Run(kind, func(t *testing.T) {
			image, results := commandWalkFixture(t, nil)
			fetch := results[2].commandStreams[0].FetchPC
			prefix := []byte{0xb9, 0, 0}
			switch kind {
			case "saved word":
				prefix = append(prefix, 0x48, 0xa9, 0, 0, 0x68)
			case "unknown Y":
				prefix = []byte{0xa4, 0x20, 0xb9, 0, 0}
			case "unknown word":
				prefix = []byte{0xa5, 0x20}
			case "changed bank":
				prefix = append(prefix, 0x4b, 0xab)
			case "unbalanced stack":
				prefix = append(prefix, 0x48)
			case "read bank boundary":
				prefix = []byte{0xb9, 0xff, 0xff}
			}
			code := append(prefix, 0xa8, 0x88, 0x4c, byte(fetch), byte(fetch>>8))
			copy(image[0x940:], code)
			copy(image[0x800:], []byte{0x40, 0x89})
			copy(image[0x1201f:], []byte{0, 0x80, 1, 0xa1})
			copy(image[0x12100:], []byte{0, 0x81, 0, 0x93})
			image[0x1300] = 0x60
			if kind == "unmapped word" {
				image[0x12022] = 0x10
			}
			if kind == "cursor underflow" {
				image[0x12021], image[0x12022] = 0, 0
			}
			if kind == "pointer cycle" {
				image[0x12021], image[0x12022] = 0x20, 0xa0
			}
			var graphs []*decoder.Graph
			for _, r := range results {
				g, err := decoder.DecodeFunction(image, 0, uint16(r.entry.Address), 0, 0, decoder.Options{})
				if err != nil {
					t.Fatal(err)
				}
				graphs = append(graphs, g)
			}
			cfgs := map[byte]*config.Config{}
			if kind == "HLE command" {
				cfgs[0x80] = &config.Config{HLEFunctions: map[uint16]string{0x8940: "Host"}}
			}
			if kind == "data target" {
				cfgs[0] = &config.Config{DataRegions: []config.DataRegion{{Bank: 0, Start: 0x9300, End: 0x9301}}}
			}
			got := AnalyzeDecodedCommands(image, cfgs, graphs, graphs)
			want := kind == "word" || kind == "saved word"
			if slices.Contains(got.Targets, uint32(0x9300)) != want {
				t.Fatalf("targets %X want=%v", got.Targets, want)
			}
			if kind == "pointer cycle" {
				if len(got.Walks) == 0 {
					t.Fatal("missing cycle diagnostic")
				}
				for _, w := range got.Walks {
					if w.StopReason != "stream_cursor_cycle" {
						t.Fatalf("unbounded or hidden cycle: %+v", w)
					}
				}
			}
			if want {
				found := false
				for _, w := range got.Walks {
					for _, step := range w.Steps {
						if step.NextPC != nil && *step.NextPC == 0x02a100 && step.NativePath != nil && step.NativePath.StreamCursorRead != nil {
							p := step.NativePath.StreamCursorRead
							if p.LoadPC != 0x8940 || p.Offset != 0 || p.Delta != -1 || p.BankSource != "entry_db" {
								t.Fatalf("lost pointer provenance %+v", p)
							}
							if len(step.Operands) != 1 || step.Operands[0].SourcePC != 0x02a021 || step.Operands[0].Value != 0xa101 || len(step.Operands[0].Candidates) != 0 {
								t.Fatalf("pointer became code or lost source: %+v", step)
							}
							found = true
						}
					}
				}
				if !found {
					t.Fatal("missing conditional pointer evidence")
				}
			}
			slices.Reverse(graphs)
			if other := AnalyzeDecodedCommands(image, cfgs, graphs, graphs); !reflect.DeepEqual(got, other) {
				t.Fatal("worker-order-dependent pointer closure")
			}
		})
	}
}
