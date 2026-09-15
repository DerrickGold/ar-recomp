package tooling

import (
	"encoding/json"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestFiniteStoredIndexedCallTargets(t *testing.T) {
	for _, kind := range []string{"call", "jump", "DP slot", "unknown mask", "unknown bank", "index clobber", "byte writer", "byte consumer", "HLE writer", "HLE consumer", "HLE target", "data target", "ineligible writer", "record wrap"} {
		t.Run(kind, func(t *testing.T) {
			image := make(rom.Image, 3*0x8000)
			// Read pointer words in bank 02, store the selected pointer, then
			// read field +6 in the independent consumer's live program bank 01.
			writer := []byte{0xad, 0, 0x12, 0x29, 3, 0, 0x0a, 0xa8, 0xf4, 2, 2, 0xab, 0xab,
				0xbe, 0, 0x90, 0x8e, 0x40, 0, 0x60}
			consumer := []byte{0xae, 0x40, 0, 0xfc, 6, 0, 0x60}
			for n := 0; n < 4; n++ {
				copy(image[0x11000+2*n:], []byte{byte(n * 16), 0x92})
				copy(image[0x9206+16*n:], []byte{byte(n * 16), 0x94})
				image[0x9400+16*n] = 0x60
			}
			// A null record pointer is not a terminator for the finite mask.
			image[0x11002], image[0x11003] = 0, 0
			want := []uint32{0x019400, 0x019420, 0x019430}
			configs := map[byte]*config.Config{0: {}, 1: {}}
			wm, wx, cx := uint8(0), uint8(0), uint8(0)
			switch kind {
			case "jump":
				consumer[3] = 0x7c
			case "DP slot":
				writer = append(slices.Clone(writer[:16]), 0x86, 0x40, 0x60)
				consumer = []byte{0xa6, 0x40, 0xfc, 6, 0, 0x60}
			case "unknown mask":
				writer[4], writer[5] = 0xff, 0xff
				want = nil
			case "unknown bank":
				for n := 8; n < 13; n++ {
					writer[n] = 0xea
				}
				want = nil
			case "index clobber":
				writer = append(append(slices.Clone(writer[:13]), 0xac, 0, 0x14), writer[13:]...)
				want = nil
			case "byte writer":
				wx = 1
				want = nil
			case "byte consumer":
				cx = 1
				want = nil
			case "HLE writer":
				configs[0].HLEFunctions = map[uint16]string{0x8000: "Host"}
				want = nil
			case "HLE consumer":
				configs[1].HLEFunctions = map[uint16]string{0x8100: "Host"}
				want = nil
			case "HLE target":
				configs[0x81] = &config.Config{HLEFunctionsIf: map[uint16]config.HLEFunctionIf{0x9420: {}}}
				want = []uint32{0x019400, 0x019430}
			case "data target":
				configs[1].DataRegions = []config.DataRegion{{Bank: 1, Start: 0x9420, End: 0x9420}}
				want = []uint32{0x019400, 0x019430}
			case "record wrap":
				consumer[4], consumer[5] = 0xff, 0xff
				want = nil
			}
			copy(image, writer)
			copy(image[0x8100:], consumer)
			wg, err := decoder.DecodeFunction(image, 0, 0x8000, wm, wx, decoder.Options{})
			if err != nil {
				t.Fatal(err)
			}
			cg, err := decoder.DecodeFunction(image, 1, 0x8100, 0, cx, decoder.Options{})
			if err != nil {
				t.Fatal(err)
			}
			graphs := []*decoder.Graph{wg, cg}
			eligible := slices.Clone(graphs)
			if kind == "ineligible writer" {
				eligible = eligible[1:]
				want = nil
			}
			before, _ := json.Marshal(configs)
			for range 2 {
				if got := FiniteStoredIndexedCallTargets(image, configs, graphs, eligible); !slices.Equal(got, want) {
					t.Fatalf("got %X want %X", got, want)
				}
				slices.Reverse(graphs)
				slices.Reverse(eligible)
			}
			after, _ := json.Marshal(configs)
			if string(before) != string(after) {
				t.Fatal("config mutated")
			}
		})
	}
}

func TestIndexedCallPointerTableStopsAtItsRecords(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0x4b, 0xab, 0xa5, 0x70, 0x29, 0xff, 0, 0xa8, 0xbe, 0, 0x90, 0x8e, 0x40, 0, 0x60})
	copy(image[0x100:], []byte{0xae, 0x40, 0, 0xfc, 6, 0, 0x60})
	// Six bytes of pointer table, followed by variable-size records. Do not
	// reinterpret words from a record as additional record pointers.
	copy(image[0x1000:], []byte{0x20, 0x90, 6, 0x90, 0x10, 0x90})
	for _, p := range []int{0x100c, 0x1016, 0x1026} {
		copy(image[p:], []byte{0, 0x94})
	}
	copy(image[0x1008:], []byte{0, 0x96})
	copy(image[0x1606:], []byte{0, 0x95})
	image[0x1400], image[0x1500] = 0x60, 0x60
	w, _ := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
	c, _ := decoder.DecodeFunction(image, 0, 0x8100, 0, 0, decoder.Options{})
	g := []*decoder.Graph{w, c}
	if got := FiniteStoredIndexedCallTargets(image, nil, g, g); !slices.Equal(got, []uint32{0x9400}) {
		t.Fatalf("read beyond pointer table: %X", got)
	}
}

func TestIndexedCallHiROMRecordAndCodeBelow8000(t *testing.T) {
	image := make(rom.Image, 4*0x10000)
	copy(image[0xffc0:], []byte("SYNTHETIC TABLE ROM   "))
	image[0xffd5], image[0xffdc], image[0xffdd], image[0xfffd] = 0x31, 0xff, 0xff, 0x80
	if image.Mapper() != rom.HiROM {
		t.Fatal("fixture lost HiROM")
	}
	copy(image[0x14000:], []byte{0xa5, 0x70, 0x29, 1, 0, 0x0a, 0xaa, 0xbf, 0, 0x30, 0xc2, 0x85, 0x40, 0x60})
	copy(image[0x35000:], []byte{0xa6, 0x40, 0xfc, 6, 0, 0x60})
	copy(image[0x23000:], []byte{0, 0x40, 0x10, 0x40})
	copy(image[0x34006:], []byte{0, 0x60})
	copy(image[0x34016:], []byte{0x10, 0x60})
	image[0x36000], image[0x36010] = 0x60, 0x60
	for _, bank := range []byte{0x43, 0xc3} {
		w, err := decoder.DecodeFunction(image, 0xc1, 0x4000, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		c, err := decoder.DecodeFunction(image, bank, 0x5000, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		g := []*decoder.Graph{w, c}
		want := []uint32{uint32(bank)<<16 | 0x6000, uint32(bank)<<16 | 0x6010}
		if got := FiniteStoredIndexedCallTargets(image, nil, g, g); !slices.Equal(got, want) {
			t.Fatalf("got %X want %X", got, want)
		}
	}
}
