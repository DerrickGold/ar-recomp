package tooling

import (
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestColdCommandCoverageKeepsUnexpandedAlternatives(t *testing.T) {
	walks := []ShadowCommandWalk{
		{SelectorPC: 0x8000, Steps: []ShadowCommandWalkStep{{FetchPC: 0x9010}, {FetchPC: 0x9020}}, StopPC: 0x9030, StopReason: "stream_command_budget"},
		{SelectorPC: 0x8000, Steps: []ShadowCommandWalkStep{{FetchPC: 0x9020}, {FetchPC: 0x9030}}, StopPC: 0x9030, StopReason: "stream_path_budget"},
		{SelectorPC: 0x8000, Steps: []ShadowCommandWalkStep{{FetchPC: 0x9040}}, StopPC: 0x9040, StopReason: "callback_dynamic_transfer"},
		{SelectorPC: 0x8100, Steps: []ShadowCommandWalkStep{{FetchPC: 0x9030}}, StopPC: 0x9030, StopReason: "callback_dynamic_transfer"},
	}
	got := coveredColdCommandPositions(walks)
	for _, p := range []coldCommandPosition{{0x8000, 0x9010}, {0x8000, 0x9020}, {0x8000, 0x9040}, {0x8100, 0x9030}} {
		if !got[p] {
			t.Fatalf("lost materialized conditional position %+v", p)
		}
	}
	if got[coldCommandPosition{0x8000, 0x9030}] {
		t.Fatal("budget stop incorrectly suppressed a new query")
	}
	if len(got) != 4 {
		t.Fatal(got)
	}
}

func TestPublishedCursorFixedPointBeyondEightWaves(t *testing.T) {
	image, results := commandWalkFixture(t, nil)
	fetch := results[2].commandStreams[0].FetchPC
	copy(image[0x800:], []byte{0x40, 0x89})
	copy(image[0x940:], []byte{0x98, 0x18, 0x69, 3, 0, 0x95, 0x46, 0x60})
	copy(image[0xb00:], []byte{0xb4, 0x46, 0x88, 0x4c, byte(fetch), byte(fetch >> 8)})
	for n := 0; n < 16; n++ {
		copy(image[0x1201f+n*4:], []byte{0, 0x80, 0, 0})
	}
	copy(image[0x1201f+16*4:], []byte{0, 0x81, 0, 0x93})
	image[0x1300] = 0x60
	var graphs []*decoder.Graph
	for _, r := range results {
		g, err := decoder.DecodeFunction(image, 0, uint16(r.entry.Address), 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	g, err := decoder.DecodeFunction(image, 0, 0x8b00, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	graphs = append(graphs, g)
	got := AnalyzeDecodedCommands(image, nil, graphs, graphs)
	if !slices.Contains(got.Targets, uint32(0x9300)) || got.ResumeRounds <= 8 || got.ResumeTruncated {
		t.Fatalf("depth cutoff or nonconvergent closure: rounds=%d truncated=%v targets=%X", got.ResumeRounds, got.ResumeTruncated, got.Targets)
	}
	// Publication back to the original cursor is not a new root or an
	// invitation to keep consuming native command/scout work forever.
	copy(image[0x940:], []byte{0x98, 0x38, 0xe9, 1, 0, 0x95, 0x46, 0x60})
	graphs = nil
	for _, pc := range []uint16{0x8000, 0x8100, 0x8200, 0x8a00, 0x8b00} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	got = AnalyzeDecodedCommands(image, nil, graphs, graphs)
	if got.ResumeRounds != 0 || got.ResumeTruncated || slices.Contains(got.Targets, uint32(0x9300)) {
		t.Fatalf("cycle did not settle: %+v", got)
	}
}
