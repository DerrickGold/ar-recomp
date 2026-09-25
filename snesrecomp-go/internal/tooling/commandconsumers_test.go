package tooling

import (
	"reflect"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// A stream command stores a ROM word into an object field. A separately
// decoded consumer later calls that field through its own frame, and the
// callback publishes a literal cursor to the field a stream reload reads. The
// literal position is a conditional root in the parent stream's bank; it is
// never the storing command's continuation.
func TestDeferredConsumerLiteralCursorIsAConditionalRoot(t *testing.T) {
	// LDA $38,X; PHB; PHK; PLB; PHX; STA $72; PEA $8A0D; JMP ($0072); PLX; PLB; RTS
	owned := []byte{0xb5, 0x38, 0x8b, 0x4b, 0xab, 0xda, 0x85, 0x72, 0xf4, 0x0d, 0x8a, 0x6c, 0x72, 0, 0xfa, 0xab, 0x60}
	// LDA $38,X; PLB; STA $72; PEA $8A0A; JMP ($0072); RTS: DB is a caller byte.
	borrowed := []byte{0xb5, 0x38, 0xab, 0x85, 0x72, 0xf4, 0x0a, 0x8a, 0x6c, 0x72, 0, 0x60}
	// LDY #$A030; LDX $12; STY $5C,X; RTS
	literal := []byte{0xa0, 0x30, 0xa0, 0xa6, 0x12, 0x94, 0x5c, 0x60}
	for _, tt := range []struct {
		name               string
		consumer, callback []byte
		native, omit, hle  bool
		want               bool
	}{
		{"literal cursor through consumer frame", owned, literal, false, false, false, true},
		{"different field", owned, []byte{0xa0, 0x30, 0xa0, 0xa6, 0x12, 0x94, 0x5d, 0x60}, false, false, false, false},
		{"consumer Y is not the command cursor", owned, []byte{0xc8, 0xa6, 0x12, 0x94, 0x5c, 0x60}, false, false, false, false},
		{"consumer borrows caller DB", borrowed, literal, false, false, false, false},
		{"no reload", owned, literal, false, true, false, false},
		{"HLE callback", owned, literal, false, false, true, false},
		{"native literal goto", owned, literal, true, false, false, true},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results := commandWalkFixture(t, nil)
			fetch := results[2].commandStreams[0].FetchPC
			copy(image[0xa00:], tt.consumer)
			copy(image[0x1500:], tt.callback)
			if tt.native {
				// Command 0 becomes LDA #$A030; STA $5C,X; RTS.
				copy(image[0x800:], []byte{0x40, 0x89})
				copy(image[0x940:], []byte{0xa9, 0x30, 0xa0, 0x95, 0x5c, 0x60})
			}
			copy(image[0xb00:], []byte{0xb4, 0x5c, 0x88, 0x4c, byte(fetch), byte(fetch >> 8)})
			// Command 0 stores $9500. Zero data stops the linear walk before the
			// command 1 at $A02F, whose callback operand is $9300.
			copy(image[0x1201f:], []byte{0, 0x80, 0, 0x95, 0, 0, 0, 0, 0, 0, 0, 0})
			copy(image[0x1202f:], []byte{0, 0x81, 0, 0x93})
			image[0x1300] = 0x60
			entries := []uint16{0x8000, 0x8100, 0x8200, 0x8a00}
			if !tt.omit {
				entries = append(entries, 0x8b00)
			}
			var graphs []*decoder.Graph
			for _, pc := range entries {
				g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
				if err != nil {
					t.Fatal(err)
				}
				graphs = append(graphs, g)
			}
			configs := map[byte]*config.Config{}
			if tt.hle {
				configs[0] = &config.Config{HLEFunctions: map[uint16]string{0x9500: "HostCallback"}}
			}
			got := AnalyzeDecodedCommands(image, configs, graphs, graphs)
			if found := slices.Contains(got.Targets, uint32(0x9300)); found != tt.want {
				t.Fatalf("target recovered=%v want=%v roots=%+v", found, tt.want, got.Roots)
			}
			if tt.want {
				via, status := uint32(0x8a0b), "conditional_consumer_published_cursor_reload"
				if tt.native {
					via, status = 0, "conditional_published_cursor_reload"
				}
				found := false
				for _, r := range got.Roots {
					for _, ref := range r.References {
						p := ref.PublishedResume
						if ref.FirstFetchPC == nil || *ref.FirstFetchPC != 0x02a02f || p == nil {
							continue
						}
						if ref.Status != status || !p.Publication.Literal || p.Publication.Value != 0xa030 || p.Publication.CursorDelta != 0 ||
							p.ParentFetchPC != 0x02a01f || p.Reload.PC != 0x8b00 || p.ConsumerDispatchPC != via {
							t.Fatalf("lost literal publication provenance: %+v %+v", ref, p)
						}
						found = true
					}
				}
				if !found {
					t.Fatal("no literal-cursor root")
				}
			}
			for _, w := range got.Walks {
				for _, s := range w.Steps {
					for _, c := range s.ConsumerPaths {
						if tt.native || c.DispatchPC != 0x8a0b || c.Path.TargetPC != 0x9500 || len(c.Path.Returns) != 1 ||
							c.Path.Returns[0].Source != "consumer_pea" || c.Path.Returns[0].TargetPC != 0x8a0e {
							t.Fatalf("consumer path evidence: %+v", c)
						}
						if s.NextPC == nil || *s.NextPC != 0x02a023 {
							t.Fatal("consumer publication became the command's continuation")
						}
					}
				}
			}
			slices.Reverse(graphs)
			if other := AnalyzeDecodedCommands(image, configs, graphs, graphs); !reflect.DeepEqual(got, other) {
				t.Fatal("worker order changes consumer publication closure")
			}
		})
	}
}
