package tooling

import (
	"encoding/json"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func queuedCallbackFixture() rom.Image {
	image := make(rom.Image, 3*0x8000)
	copy(image, []byte{0xa9, 0, 0x92, 0x22, 0, 0x81, 1, 0x60})
	copy(image[0x8100:], []byte{0x4c, 0, 0x82})
	copy(image[0x8200:], []byte{0xac, 0, 0x10, 0x99, 0, 0x12, 0xa3, 3, 0x99, 2, 0x12, 0xa5, 0x70, 0x99, 3, 0x12, 0x6b})
	copy(image[0x10300:], []byte{0xac, 0, 0x10, 0xb9, 0, 0x12, 0x85, 0x42, 0xb9, 2, 0x12, 0x85, 0x44,
		0xbe, 3, 0x12, 0x86, 0x70, 0x4b, 0xf4, 0x18, 0x83, 0xdc, 0x42, 0, 0x6b})
	image[0x1200] = 0x6b
	return image
}

func TestQueuedCallbackArgumentAndCallerBank(t *testing.T) {
	for _, kind := range []string{"native", "unknown argument", "non JSL caller", "shifted stack", "wrong stack lane", "writer index changed", "reader index changed", "reader DB changed", "scratch overwritten", "missing consumer", "HLE caller", "HLE setter", "HLE consumer", "HLE mirrored target", "data target"} {
		t.Run(kind, func(t *testing.T) {
			image := queuedCallbackFixture()
			configs := map[byte]*config.Config{}
			want := []uint32{0x009200}
			switch kind {
			case "unknown argument":
				copy(image, []byte{0xa5, 0x77, 0x22, 0, 0x81, 1, 0x60})
				want = nil
			case "non JSL caller":
				image[3] = 0x5c
				want = nil
			case "shifted stack":
				copy(image[0x8200:], []byte{0xda, 0xac, 0, 0x10, 0x99, 0, 0x12, 0xa3, 3, 0x99, 2, 0x12, 0xfa, 0x6b})
				want = nil
			case "wrong stack lane":
				image[0x8207] = 4
				want = nil
			case "writer index changed":
				copy(image[0x8200:], []byte{0xac, 0, 0x10, 0x99, 0, 0x12, 0xc8, 0xa3, 3, 0x99, 2, 0x12, 0x6b})
				want = nil
			case "reader index changed":
				code := slices.Clone(image[0x10300:0x1031a])
				copy(image[0x10300:], append(append(slices.Clone(code[:8]), 0xc8), code[8:]...))
				want = nil
			case "reader DB changed":
				code := slices.Clone(image[0x10300:0x1031a])
				copy(image[0x10300:], append(append(slices.Clone(code[:8]), 0xab), code[8:]...))
				want = nil
			case "scratch overwritten":
				image[0x10311] = 0x42
				want = nil
			case "HLE caller":
				configs[0] = &config.Config{HLEFunctions: map[uint16]string{0x8000: "Host"}}
				want = nil
			case "HLE setter":
				configs[1] = &config.Config{HLEFunctionsIf: map[uint16]config.HLEFunctionIf{0x8100: {}}}
				want = nil
			case "HLE consumer":
				configs[2] = &config.Config{HLEFunctions: map[uint16]string{0x8300: "Host"}}
				want = nil
			case "HLE mirrored target":
				configs[0x80] = &config.Config{HLEFunctions: map[uint16]string{0x9200: "Host"}}
				want = nil
			case "data target":
				configs[0] = &config.Config{DataRegions: []config.DataRegion{{Bank: 0, Start: 0x9200, End: 0x9200}}}
				want = nil
			}
			var graphs []*decoder.Graph
			for _, pc := range []uint32{0x8000, 0x018100, 0x028300} {
				g, err := decoder.DecodeFunction(image, byte(pc>>16), uint16(pc), 0, 0, decoder.Options{})
				if err != nil {
					t.Fatal(err)
				}
				graphs = append(graphs, g)
			}
			eligible := slices.Clone(graphs)
			if kind == "missing consumer" {
				eligible = eligible[:2]
				want = nil
			}
			before, _ := json.Marshal(configs)
			for range 2 {
				if got := QueuedCallbackTargets(image, configs, graphs, eligible); !slices.Equal(got, want) {
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

func TestQueuedCallbackHiROMCallerBelow8000(t *testing.T) {
	lo := queuedCallbackFixture()
	image := make(rom.Image, 4*0x10000)
	copy(image[0x4000:], []byte{0xa9, 0, 0x30, 0x22, 0, 0x81, 0xc1, 0x60})
	copy(image[0x18100:0x20000], lo[0x8100:0x10000])
	copy(image[0x28300:0x30000], lo[0x10300:0x18000])
	image[0x3000] = 0x6b
	copy(image[0xffc0:], []byte("SYNTHETIC QUEUE ROM   "))
	image[0xffd5], image[0xffdc], image[0xffdd], image[0xfffd] = 0x31, 0xff, 0xff, 0x80
	if image.Mapper() != rom.HiROM {
		t.Fatal("fixture lost mapping")
	}
	var graphs []*decoder.Graph
	for _, pc := range []uint32{0xc04000, 0xc18100, 0xc28300} {
		g, err := decoder.DecodeFunction(image, byte(pc>>16), uint16(pc), 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	if got := QueuedCallbackTargets(image, nil, graphs, graphs); !slices.Equal(got, []uint32{0xc03000}) {
		t.Fatalf("lost live caller-bank reference: %X", got)
	}
}
