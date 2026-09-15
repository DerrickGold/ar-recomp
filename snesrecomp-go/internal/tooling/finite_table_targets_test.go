package tooling

import (
	"encoding/json"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestFiniteStoredTableTargets(t *testing.T) {
	for _, kind := range []string{"DP", "absolute", "long table", "sparse mask", "unknown index", "index clobber", "unknown bank", "wrong store bank", "ineligible writer", "ineligible consumer", "HLE writer", "HLE consumer", "HLE mirror target", "data target", "HLE target interior", "wrapped table"} {
		t.Run(kind, func(t *testing.T) {
			image := make(rom.Image, 3*0x8000)
			// Source table in bank 02, consumer/target in bank 01. The mask
			// and stride precede DB setup and unrelated record-field stores.
			code := []byte{0xad, 0, 0x12, 0x29, 3, 0, 0x0a, 0x0a, 0x0a, 0xa8,
				0xf4, 2, 2, 0xab, 0xab, 0xb9, 0, 0x90, 0x8f, 0, 0x12, 0x7e,
				0xa9, 0, 1, 0x85, 0x44, 0xb9, 6, 0x90, 0x85, 0x42, 0x60}
			copy(image[0x8100:], []byte{0x6c, 0x42, 0})
			for n := 0; n < 4; n++ {
				copy(image[0x11006+n*8:], []byte{byte(n * 0x10), 0x92})
				image[0x9200+n*0x10] = 0x60
			}
			// A null cell does not terminate a finite domain. The next valid
			// record remains eligible, but a cell outside the mask does not.
			image[0x1100e], image[0x1100f] = 0, 0
			copy(image[0x11026:], []byte{0x40, 0x92})
			image[0x9240] = 0x60
			want := []uint32{0x019200, 0x019220, 0x019230}
			cfg := &config.Config{}
			configs := map[byte]*config.Config{0: cfg}
			switch kind {
			case "absolute":
				code = append(code[:30], 0x8d, 0x42, 0, 0x60)
			case "long table":
				// TYX preserves the finite record offsets. Explicit long bank
				// provenance does not require any usable DB at the load.
				for n := 10; n < 15; n++ {
					code[n] = 0xea
				}
				code = append(slices.Clone(code[:27]), 0xbb, 0xbf, 6, 0x90, 2, 0x85, 0x42, 0x60)
			case "sparse mask":
				code[4] = 5
				want = []uint32{0x019200, 0x019240}
			case "unknown index":
				code[4], code[5] = 0xff, 0xff
				want = nil
			case "index clobber":
				code = append(append(slices.Clone(code[:27]), 0xac, 0, 0x14), code[27:]...)
				want = nil
			case "unknown bank":
				for n := 10; n < 15; n++ {
					code[n] = 0xea
				}
				want = nil
			case "wrong store bank":
				// Long table read is still known, but ABS store is in ROM.
				code[11], code[12] = 0xc2, 0xc2
				code = append(slices.Clone(code[:27]), 0xbb, 0xbf, 6, 0x90, 2, 0x8d, 0x42, 0, 0x60)
				want = nil
			case "HLE writer":
				cfg.HLEFunctions = map[uint16]string{0x8000: "Host"}
				want = nil
			case "HLE consumer":
				configs[1] = &config.Config{HLEFunctions: map[uint16]string{0x8100: "Host"}}
				want = nil
			case "HLE mirror target":
				configs[0x81] = &config.Config{HLEFunctionsIf: map[uint16]config.HLEFunctionIf{0x9220: {}}}
				want = []uint32{0x019200, 0x019230}
			case "data target":
				cfg.DataRegions = []config.DataRegion{{Bank: 1, Start: 0x9220, End: 0x9220}}
				want = []uint32{0x019200, 0x019230}
			case "HLE target interior":
				configs[1] = &config.Config{HLEFunctions: map[uint16]string{0x921f: "Host"}}
				image[0x921f] = 0xea
				want = []uint32{0x019200, 0x019230}
			case "wrapped table":
				code[28], code[29] = 0xff, 0xff
				want = nil
			}
			copy(image, code)
			decode := func(bank byte, pc uint16) *decoder.Graph {
				g, err := decoder.DecodeFunction(image, bank, pc, 0, 0, decoder.Options{})
				if err != nil {
					t.Fatal(err)
				}
				return g
			}
			graphs := []*decoder.Graph{decode(0, 0x8000), decode(1, 0x8100)}
			eligible := slices.Clone(graphs)
			if kind == "HLE target interior" {
				graphs = append(graphs, decode(1, 0x921f))
			}
			if kind == "ineligible writer" {
				eligible = eligible[1:]
				want = nil
			}
			if kind == "ineligible consumer" {
				eligible = eligible[:1]
				want = nil
			}
			before, _ := json.Marshal(configs)
			for range 2 {
				got := FiniteStoredTableTargets(image, configs, graphs, eligible)
				if !slices.Equal(got, want) {
					t.Fatalf("got %X, want %X", got, want)
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

func TestFiniteStoredTableHiROMFullBankWindows(t *testing.T) {
	image := make(rom.Image, 4*0x10000)
	copy(image[0xffc0:], []byte("SYNTHETIC TABLE ROM   "))
	image[0xffd5], image[0xffdc], image[0xffdd], image[0xfffd] = 0x31, 0xff, 0xff, 0x80
	if image.Mapper() != rom.HiROM {
		t.Fatal("fixture lost HiROM mapping")
	}
	// A full-ROM bank still writes DP in bank zero. Its handler words and
	// table may legitimately point below $8000 in different full-ROM banks.
	copy(image[0x14000:], []byte{0xa5, 0x70, 0x29, 1, 0, 0x0a, 0xaa,
		0xbf, 0, 0x30, 0xc2, 0x85, 0x42, 0x60})
	copy(image[0x35000:], []byte{0x6c, 0x42, 0})
	copy(image[0x23000:], []byte{0, 0x60, 0x10, 0x60})
	image[0x36000], image[0x36010] = 0x60, 0x60
	for _, bank := range []byte{0x43, 0xc3} {
		writer, err := decoder.DecodeFunction(image, 0xc1, 0x4000, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		consumer, err := decoder.DecodeFunction(image, bank, 0x5000, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs := []*decoder.Graph{writer, consumer}
		want := []uint32{uint32(bank)<<16 | 0x6000, uint32(bank)<<16 | 0x6010}
		if got := FiniteStoredTableTargets(image, nil, graphs, graphs); !slices.Equal(got, want) {
			t.Fatalf("bank %02X: got %X, want %X", bank, got, want)
		}
	}
}

func TestFiniteStoredTableStopsAtPointedCode(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0xa5, 0x70, 0x29, 0x0f, 0, 0x0a, 0x0a, 0x0a, 0xa8,
		0x4b, 0xab, 0xb9, 6, 0x90, 0x85, 0x42, 0x6c, 0x42, 0})
	copy(image[0x1006:], []byte{0x30, 0x90})
	image[0x1030] = 0x60
	// This permitted masked position is already beyond the first handler.
	// Its instruction-looking word must not be admitted as another record.
	copy(image[0x1036:], []byte{0x10, 0x95})
	image[0x1510] = 0x60
	g, err := decoder.DecodeFunction(image, 0, 0x8000, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	graphs := []*decoder.Graph{g}
	if got := FiniteStoredTableTargets(image, nil, graphs, graphs); !slices.Equal(got, []uint32{0x9030}) {
		t.Fatalf("followed the finite domain into pointed code: %X", got)
	}
}
