package tooling

import (
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestNativeCommandBranchesKeepDecimalAlternatives(t *testing.T) {
	image, results := commandWalkFixture(t, nil)
	fetch := results[2].commandStreams[0].FetchPC
	// C is known set on the taken BCS edge. ADC #2 therefore advances
	// the saved cursor by three, not two. D remains an explicit condition.
	copy(image[0x800:], []byte{0x40, 0x89})
	copy(image[0x940:], []byte{0xa5, 0x10, 0xc9, 1, 0, 0xb0, 3, 0x4c, byte(fetch), byte(fetch >> 8),
		0x98, 0x69, 2, 0, 0xa8, 0x88, 0x4c, byte(fetch), byte(fetch >> 8)})
	image[0x1300] = 0x60
	stream := results[2].commandStreams[0]
	command := ShadowCommandPrefix{EntryPC: 0x8940}
	a := newShadowCallbackAnalyzer(image, nil, nil, nil)
	paths := a.nativePaths(stream, command)
	clear, unresolved, unchanged := false, false, false
	for _, p := range paths {
		if p.EntryDecimalCondition == "clear" && p.Status == "owned_native_refetch" && p.CursorDelta == 2 {
			clear = true
			if len(p.Branches) != 1 || p.Branches[0].Mnemonic != "BCS" || !p.Branches[0].Taken {
				t.Fatalf("lost carry edge: %+v", p)
			}
		}
		if p.EntryDecimalCondition == "set" && p.Status == "saved_cursor_decimal_unproven" {
			unresolved = true
		}
		if p.EntryDecimalCondition == "set" && p.Status == "owned_native_refetch" && p.CursorDelta == 0 {
			unchanged = true
		}
	}
	if !clear || !unresolved || !unchanged {
		t.Fatalf("lost native alternatives: %+v", paths)
	}
	// Walk the same command from a rooted ROM stream, reaching its later
	// callback only through a documented conditional native path.
	copy(image[0x1201f:], []byte{0, 0x80, 0, 0, 0, 0x81, 0, 0x93})
	var graphs []*decoder.Graph
	for _, r := range results {
		g, err := decoder.DecodeFunction(image, 0, uint16(r.entry.Address), 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	got := AnalyzeDecodedCommands(image, nil, graphs, graphs)
	if !slices.Contains(got.Targets, uint32(0x9300)) {
		t.Fatalf("later callback not recovered: %X", got.Targets)
	}
	found := false
	for _, w := range got.Walks {
		for _, step := range w.Steps {
			if step.NativePath != nil && step.NativePath.EntryDecimalCondition == "clear" && step.NextPC != nil && *step.NextPC == 0x02a023 {
				found = true
			}
		}
	}
	if !found {
		t.Fatal("invented next cursor without native/decimal provenance")
	}
}

func TestNativeCommandDoesNotBorrowCallerFrame(t *testing.T) {
	image, results := commandWalkFixture(t, nil)
	image[0x900] = 0x68 // PLA with no command-owned push
	a := newShadowCallbackAnalyzer(image, nil, nil, nil)
	paths := a.nativePaths(results[2].commandStreams[0], ShadowCommandPrefix{EntryPC: 0x8900})
	if len(paths) != 2 {
		t.Fatalf("entry D alternatives=%d", len(paths))
	}
	for _, p := range paths {
		if p.Status != "opaque_caller_stack_read" {
			t.Fatalf("borrowed native caller state: %+v", p)
		}
	}
}
