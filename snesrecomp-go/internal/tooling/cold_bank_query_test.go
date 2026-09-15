package tooling

import (
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestColdBankQueryDoesNotPublishStackAliasHypothesis(t *testing.T) {
	for _, kind := range []string{"saved DB", "HLE callee", "unknown pull", "recursive callee", "explicit different bank"} {
		t.Run(kind, func(t *testing.T) {
			image := make(rom.Image, 0x10000)
			copy(image[0x8000:], []byte{0x20, 0, 0x81, 0x20, 0, 0x82, 0x60})
			copy(image[0x8100:], []byte{0x4b, 0xab, 0x60})
			copy(image[0x8200:], []byte{0x8b, 0x4b, 0xab, 0x85, 0x42, 0xab, 0x60})
			configs := map[byte]*config.Config{}
			want := true
			value := uint16(1)
			switch kind {
			case "HLE callee":
				configs[1] = &config.Config{HLEFunctions: map[uint16]string{0x8200: "Host"}}
				want = false
			case "unknown pull":
				copy(image[0x8200:], []byte{0xab, 0x60})
				want = false
			case "recursive callee":
				copy(image[0x8200:], []byte{0x20, 0, 0x82, 0x60})
				want = false
			case "explicit different bank":
				copy(image[0x8200:], []byte{0xf4, 2, 2, 0xab, 0xab, 0x60})
				value = 0 // unsupported literal stack stays unknown
				want = false
			}
			g, err := decoder.DecodeFunction(image, 1, 0x8000, 0, 0, decoder.Options{})
			if err != nil {
				t.Fatal(err)
			}
			_, a := coldNativeGraphs(image, configs, []*decoder.Graph{g}, []*decoder.Graph{g})
			q := coldBankQueries{a: a}
			at := decoder.DecodeKey{PC: 0x018006}
			got, ok := shadowStreamConstantWord(q.bank(g, at))
			if ok != want || ok && got != value {
				t.Fatalf("got=%x known=%v want=%x known=%v", got, ok, value, want)
			}
			if kind == "saved DB" {
				strict := buildShadowDBSummaries(q.programs, nil)
				entry := decoder.Variant{Address: g.Entry.PC}
				target := decoder.Variant{Address: at.PC}
				published := shadowBankQuery(q.programs[entry], strict, nil, target)
				if published.Status == "local_constant_set" {
					t.Fatal("cold stack hypothesis leaked into a bank fact")
				}
				setup := strict[decoder.Variant{Address: 0x018100}]
				if !setup.valid || setup.constant == nil || *setup.constant != 1 {
					t.Fatal("lost exact native DB setter summary")
				}
			}
		})
	}
}
