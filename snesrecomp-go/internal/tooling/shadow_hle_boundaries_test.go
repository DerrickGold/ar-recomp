package tooling

import (
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// A falls through an intercepted prefix whose ROM fallback branches to Tail.
// Tail is not part of A's generated region: claiming that ownership would
// either fail fact import or bypass the hook when sharing continuations.
func TestShadowContinuationOwnershipStopsAtHLEEntries(t *testing.T) {
	for _, kind := range []string{"unconditional", "conditional", "spc"} {
		t.Run(kind, func(t *testing.T) {
			image := make(romimage.Image, 0x8000)
			copy(image, []byte{0xea, 0xea, 0x80, 0x0c})
			image[0x10] = 0x60
			cfg := &config.Config{Entries: []config.Entry{
				{Name: "A", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}},
				{Name: "Hook", Start: 0x8001, EntryMX: config.MX{M: 1, X: 1}},
				{Name: "bank_00_8010", Start: 0x8010, EntryMX: config.MX{M: 1, X: 1}},
			}}
			switch kind {
			case "unconditional":
				cfg.HLEFunctions = map[uint16]string{0x8001: "Hook"}
			case "conditional":
				cfg.HLEFunctionsIf = map[uint16]config.HLEFunctionIf{0x8001: {Function: "Hook", Predicate: "UseHook"}}
			case "spc":
				cfg.HLESPCUpload = []uint16{0x8001}
			}
			results, _ := runShadowDecodePass(image, []shadowBank{{ID: 0, Config: cfg}}, map[byte][]config.Entry{0: cfg.Entries}, nil, nil, 1)
			found := false
			for _, result := range results {
				if result.entry.Address != 0x8000 {
					continue
				}
				found = true
				if result.issue != nil {
					t.Fatal(result.issue)
				}
				if len(result.resumeEdges[decoder.Variant{Address: 0x8010, M: 1, X: 1}]) != 0 {
					t.Fatal("claimed ownership beyond the HLE wrapper")
				}
				if len(result.resumeEdges[decoder.Variant{Address: 0x8001, M: 1, X: 1}]) != 1 {
					t.Fatal("lost the incoming edge to the HLE wrapper")
				}
			}
			if !found {
				t.Fatal("no root analysis")
			}
		})
	}
}
