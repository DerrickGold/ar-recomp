package tooling

import (
	"encoding/json"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func coldValueFixture(t *testing.T) (rom.Image, []*decoder.Graph) {
	t.Helper()
	image := make(rom.Image, 0x18000)
	// A masked selector is published, then used in two independent table
	// reads separated by an unknown indirect call. Field +6 initializes the
	// record; field +8 is forwarded through a tail thunk to a saved pointer.
	copy(image[0x8000:], []byte{0x4b, 0xab, 0xa5, 0x70, 0x29, 1, 0, 0x0a, 0x85, 0x72, 0xa8,
		0xbe, 0, 0x90, 0x86, 0x40, 0xa6, 0x40, 0xfc, 6, 0,
		0xc2, 0x30, 0xa4, 0x72, 0xbe, 0, 0x90, 0xbd, 8, 0, 0xa2, 1, 0, 0x5c, 0, 0x82, 0})
	copy(image[0x200:], []byte{0x4c, 0, 0x83})
	copy(image[0x300:], []byte{0x4b, 0xab, 0x85, 0x50, 0x86, 0x52, 0x60})
	copy(image[0x400:], []byte{0xdc, 0x50, 0})
	copy(image[0x9000:], []byte{4, 0x90, 0x10, 0x90})
	copy(image[0x900a:], []byte{0, 0x94, 0, 0x95})
	copy(image[0x9016:], []byte{0x10, 0x94, 0x10, 0x95})
	for _, at := range []int{0x9400, 0x9410, 0x9500, 0x9510} {
		image[at] = 0x60
	}
	var graphs []*decoder.Graph
	for _, pc := range []uint32{0x018000, 0x008200, 0x008300, 0x008400} {
		g, err := decoder.DecodeFunction(image, byte(pc>>16), uint16(pc), 0, 0, decoder.Options{MaxInstructions: 128})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	return image, graphs
}

func TestColdValueSharedTableConsumers(t *testing.T) {
	image, graphs := coldValueFixture(t)
	before, _ := json.Marshal(graphs)
	want := []uint32{0x019400, 0x019410, 0x019500, 0x019510}
	var first ColdValueInventory
	for n := 0; n < 2; n++ {
		got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
		if !got.Converged || !slices.Equal(got.Targets, want) {
			t.Fatalf("got %X want %X; %s", got.Targets, want, got.Summary())
		}
		if !slices.Contains(got.Conditions, "same_table_bank_identity") || !slices.Contains(got.Conditions, "record_in_pointer_table_bank") {
			t.Fatalf("missing conditional bank evidence: %v", got.Conditions)
		}
		if n == 0 {
			first = got
		} else {
			a, _ := json.Marshal(first)
			b, _ := json.Marshal(got)
			if string(a) != string(b) {
				t.Fatalf("order-dependent values:\n%s\n%s", a, b)
			}
		}
		slices.Reverse(graphs)
	}
	after, _ := json.Marshal(graphs)
	if string(before) != string(after) {
		t.Fatal("decoded graphs mutated")
	}
}

func TestColdValuePointerCallerCorrelation(t *testing.T) {
	image := make(rom.Image, 0x18000)
	// Two callers pass different bank/word pairs. The crossed combinations
	// are valid-looking code too, but must not be admitted by joining A and X.
	for n := 0; n < 2; n++ {
		copy(image[0x100*n:], []byte{0xa9, byte(n * 16), 0x95, 0xa2, byte(n + 1), 0, 0x5c, 0, 0x83, 0})
		for bank := 1; bank < 3; bank++ {
			image[bank*0x8000+0x1500+n*16] = 0x60
		}
	}
	copy(image[0x300:], []byte{0x4b, 0xab, 0x85, 0x50, 0x86, 0x52, 0x60})
	copy(image[0x400:], []byte{0xdc, 0x50, 0})
	var graphs []*decoder.Graph
	for _, pc := range []uint16{0x8000, 0x8100, 0x8300, 0x8400} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
	if !slices.Equal(got.Targets, []uint32{0x019500, 0x029510}) {
		t.Fatalf("mixed callers: %X; %s", got.Targets, got.Summary())
	}
}

func TestColdValueOwnershipAndCycles(t *testing.T) {
	for _, kind := range []string{"HLE setter", "data target", "unknown bank", "cyclic slots"} {
		t.Run(kind, func(t *testing.T) {
			image, graphs := coldValueFixture(t)
			configs := map[byte]*config.Config{}
			want := []uint32{0x019400, 0x019410}
			switch kind {
			case "HLE setter":
				configs[0] = &config.Config{HLEFunctions: map[uint16]string{0x8300: "Host"}}
			case "data target":
				configs[1] = &config.Config{DataRegions: []config.DataRegion{{Bank: 1, Start: 0x9500, End: 0x9510}}}
			case "unknown bank":
				image[0x8000], image[0x8001] = 0xea, 0xea
				g, err := decoder.DecodeFunction(image, 1, 0x8000, 0, 0, decoder.Options{MaxInstructions: 128})
				if err != nil {
					t.Fatal(err)
				}
				graphs[0] = g
				want = nil
			case "cyclic slots":
				copy(image[0x8000:], []byte{0xa5, 0x74, 0x85, 0x72, 0xa5, 0x72, 0x85, 0x74, 0xa6, 0x74, 0xfc, 6, 0, 0x60})
				g, err := decoder.DecodeFunction(image, 1, 0x8000, 0, 0, decoder.Options{MaxInstructions: 128})
				if err != nil {
					t.Fatal(err)
				}
				graphs[0] = g
				want = nil
			}
			got := AnalyzeColdValueProvenance(image, configs, graphs, graphs)
			if !got.Converged || !slices.Equal(got.Targets, want) {
				t.Fatalf("got %X want %X; %s", got.Targets, want, got.Summary())
			}
		})
	}
}

func TestColdValueRawMaskedWordsAreNotSavedHandlers(t *testing.T) {
	image := make(rom.Image, 0x10000)
	copy(image, []byte{0x4b, 0xab, 0xa5, 0x70, 0x29, 1, 0, 0x0a, 0xaa, 0xbd, 0, 0x90, 0xa2, 1, 0, 0x85, 0x50, 0x86, 0x52, 0x60})
	copy(image[0x100:], []byte{0xdc, 0x50, 0})
	copy(image[0x1000:], []byte{0, 0x95, 0x10, 0x95})
	image[0x9500], image[0x9510] = 0x60, 0x60
	var graphs []*decoder.Graph
	for _, pc := range []uint16{0x8000, 0x8100} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
	if len(got.Targets) != 0 || !slices.Contains(got.Boundaries, "unbounded_saved_pointer_word") {
		t.Fatalf("mask manufactured table ownership: %+v", got)
	}
}

func TestColdValueKnownBankIsNotReplacedByTableHypothesis(t *testing.T) {
	image, graphs := coldValueFixture(t)
	// Add a known conflicting DB before the second table read. The witness
	// in bank 01 must not replace this native bank-02 selection.
	original := slices.Clone(image[0x8000:0x8026])
	copy(image[0x8000:], append(append(slices.Clone(original[:23]), 0xf4, 2, 2, 0xab, 0xab), original[23:]...))
	g, err := decoder.DecodeFunction(image, 1, 0x8000, 0, 0, decoder.Options{MaxInstructions: 128})
	if err != nil {
		t.Fatal(err)
	}
	graphs[0] = g
	got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
	if len(got.LongPointerTargets) != 0 {
		t.Fatalf("overrode native DB: %X", got.LongPointerTargets)
	}
}

func TestColdValueBusySetterDoesNotLoseAllCallerArguments(t *testing.T) {
	image := make(rom.Image, 0x10000)
	var graphs []*decoder.Graph
	var want []uint32
	for n := 0; n < 80; n++ {
		pc := uint16(0x8000 + n*16)
		target := uint16(0xa000 + n*2)
		copy(image[n*16:], []byte{0xa9, byte(target), byte(target >> 8), 0xa2, 1, 0, 0x5c, 0, 0x90, 0})
		image[int(target)] = 0x60 // bank-01 LoROM offset for this target
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
		want = append(want, uint32(0x010000)|uint32(target))
	}
	copy(image[0x1000:], []byte{0x4c, 0, 0x91})
	copy(image[0x1100:], []byte{0x4b, 0xab, 0x85, 0x50, 0x86, 0x52, 0x60})
	copy(image[0x1200:], []byte{0xdc, 0x50, 0})
	for _, pc := range []uint16{0x9000, 0x9100, 0x9200} {
		g, err := decoder.DecodeFunction(image, 0, pc, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	got := AnalyzeColdValueProvenance(image, nil, graphs, graphs)
	if !slices.Equal(got.LongPointerTargets, want) {
		t.Fatalf("lost busy setter callers: %X; %s", got.LongPointerTargets, got.Summary())
	}
}
