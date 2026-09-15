package tooling

import (
	"encoding/json"
	"reflect"
	"slices"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

func TestDecodedCommandInventoryReusesConditionalWalk(t *testing.T) {
	image, results := commandWalkFixture(t, nil)
	image[0x1200], image[0x1300] = 0x60, 0x60
	var graphs []*decoder.Graph
	for _, r := range results {
		g, err := decoder.DecodeFunction(image, 0, uint16(r.entry.Address), 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		graphs = append(graphs, g)
	}
	cfg := &config.Config{}
	configs := map[byte]*config.Config{0: cfg}
	before, _ := json.Marshal(configs)
	r := AnalyzeDecodedCommands(image, configs, graphs, graphs)
	if !slices.Equal(r.Targets, []uint32{0x9200, 0x9300}) {
		t.Fatalf("targets=%X walks=%+v", r.Targets, r.Walks)
	}
	slices.Reverse(graphs)
	if other := AnalyzeDecodedCommands(image, configs, graphs, graphs); !reflect.DeepEqual(r, other) {
		t.Fatal("graph order changed the conditional inventory")
	}
	after, _ := json.Marshal(configs)
	if string(before) != string(after) {
		t.Fatal("authored config mutated")
	}
	if got := AnalyzeDecodedCommands(image, configs, graphs, nil); len(got.Targets) != 0 {
		t.Fatalf("ineligible sources supplied targets: %X", got.Targets)
	}
	cfg.HLEFunctions = map[uint16]string{0x9300: "CustomCallback"}
	if got := AnalyzeDecodedCommands(image, configs, graphs, graphs); !slices.Equal(got.Targets, []uint32{0x9200}) {
		t.Fatalf("HLE callback bypassed: %X", got.Targets)
	}
	cfg.HLEFunctions = nil
	cfg.HLEFunctionsIf = map[uint16]config.HLEFunctionIf{0x9300: {}}
	if got := AnalyzeDecodedCommands(image, configs, graphs, graphs); !slices.Equal(got.Targets, []uint32{0x9200}) {
		t.Fatalf("conditional HLE callback bypassed: %X", got.Targets)
	}
	cfg.HLEFunctionsIf = nil
	cfg.HLEFunctions = map[uint16]string{0x8000: "HostCaller"}
	if got := AnalyzeDecodedCommands(image, configs, graphs, graphs); len(got.Targets) != 0 {
		t.Fatalf("HLE input path supplied native arguments: %X", got.Targets)
	}
}

func TestColdCommandPointerNeighborsAreBoundedOpenAnchors(t *testing.T) {
	image, results := commandWalkFixture(t, nil)
	root := resolveShadowCommandRoots(image, results)[0]
	copy(image[0x11010:], []byte{0, 0xa1})
	copy(image[0x11014:], []byte{0, 0xa2})
	second := ShadowCommandRootReference{TableIndex: 20}
	resolveShadowStreamPointer(image, root, &second)
	root.References = append(root.References, second)
	a := newShadowCallbackAnalyzer(image, nil, nil, nil)
	before, _ := json.Marshal(root)
	got := extendColdCommandPointerNeighbors(image, []ShadowCommandRoot{root}, a)[0]
	found := false
	for _, ref := range got.References {
		if ref.TableIndex == 16 {
			found = ref.Status == "open_neighbor_ROM_stream_reference" && ref.FirstFetchPC != nil && *ref.FirstFetchPC == 0x02a0ff && ref.LiteralValue == nil && len(ref.Calls) == 0
		}
	}
	if !found {
		t.Fatalf("missing separately labeled neighbor: %+v", got.References)
	}
	after, _ := json.Marshal(root)
	if string(before) != string(after) {
		t.Fatal("original proof references mutated")
	}
	for _, kind := range []string{"one anchor", "duplicate anchor", "unknown bank", "unsupported index", "code boundary", "null neighbor"} {
		t.Run(kind, func(t *testing.T) {
			r := root
			r.References = append([]ShadowCommandRootReference(nil), root.References...)
			b := newShadowCallbackAnalyzer(image, nil, nil, nil)
			saved := append([]byte(nil), image[0x11010:0x11012]...)
			defer copy(image[0x11010:0x11012], saved)
			switch kind {
			case "one anchor":
				r.References = r.References[:1]
			case "duplicate anchor":
				r.References[1] = r.References[0]
			case "unknown bank":
				r.StreamBank.UnknownPaths = true
			case "unsupported index":
				r.TableRead.Index.Operations = nil
			case "code boundary":
				b.starts[0x11010] = true
			case "null neighbor":
				image[0x11010], image[0x11011] = 0, 0
			}
			result := extendColdCommandPointerNeighbors(image, []ShadowCommandRoot{r}, b)[0]
			for _, ref := range result.References {
				if ref.TableIndex == 16 {
					t.Fatalf("crossed %s: %+v", kind, ref)
				}
			}
		})
	}
	// Filled records do not turn neighbors into new anchors. The outermost
	// original anchor is index 20, so the final permitted right probe is 148.
	for index := 24; index <= 200; index += 4 {
		copy(image[0x11000+index:], []byte{0, 0xa3})
	}
	got = extendColdCommandPointerNeighbors(image, []ShadowCommandRoot{root}, a)[0]
	last := uint16(0)
	for _, ref := range got.References {
		if ref.TableIndex > last {
			last = ref.TableIndex
		}
	}
	if last != 148 {
		t.Fatalf("probe budget/anchor recursion changed: last=%d", last)
	}
}
