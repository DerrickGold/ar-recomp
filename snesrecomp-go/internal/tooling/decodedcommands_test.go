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
			found = ref.Status == "open_interpolated_ROM_stream_reference" && ref.FirstFetchPC != nil && *ref.FirstFetchPC == 0x02a0ff && ref.LiteralValue == nil && len(ref.Calls) == 0
		}
	}
	if !found {
		t.Fatalf("missing separately labeled neighbor: %+v", got.References)
	}
	after, _ := json.Marshal(root)
	if string(before) != string(after) {
		t.Fatal("original proof references mutated")
	}
	for _, kind := range []string{"one anchor", "duplicate anchor", "unknown bank", "unsupported index", "code boundary", "non-stream slot", "null slot"} {
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
			case "non-stream slot":
				image[0x11010], image[0x11011] = 0x10, 0
			case "null slot":
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

func TestColdCommandPointerTableInterpolatesAcrossNullSlots(t *testing.T) {
	image, results := commandWalkFixture(t, nil)
	root := resolveShadowCommandRoots(image, results)[0]
	slot := func(index int, pointer uint16) {
		image[0x11000+index], image[0x11001+index] = byte(pointer), byte(pointer>>8)
	}
	for index := 16; index <= 700; index += 4 {
		slot(index, 0xa100)
	}
	// Anchors 12 and 332 are 80 records apart: indices 164 and 180 lie more
	// than 32 records from both, around a null run that is not a terminator.
	slot(332, 0xa200)
	far := ShadowCommandRootReference{TableIndex: 332}
	resolveShadowStreamPointer(image, root, &far)
	root.References = append(root.References, far)
	for _, index := range []int{40, 168, 172, 176, 340} {
		slot(index, 0)
	}
	a := newShadowCallbackAnalyzer(image, nil, nil, nil)
	got := extendColdCommandPointerNeighbors(image, []ShadowCommandRoot{root}, a)[0]
	status := map[int]string{}
	for _, ref := range got.References[len(root.References):] {
		status[int(ref.TableIndex)] = ref.Status
	}
	for index := 16; index < 332; index += 4 {
		want := "open_interpolated_ROM_stream_reference"
		if index == 40 || index >= 168 && index <= 176 {
			want = ""
		}
		if status[index] != want {
			t.Fatalf("index %d: status=%q want %q", index, status[index], want)
		}
	}
	// The outer window still ends 32 records past the last anchor; a null slot
	// inside it is skipped rather than referenced or granted extra reach.
	last := 0
	for index, s := range status {
		if index > 332 {
			if s != "open_neighbor_ROM_stream_reference" {
				t.Fatalf("index %d: status=%q", index, s)
			}
			last = max(last, index)
		}
	}
	if last != 332+32*4 || status[340] != "" {
		t.Fatalf("outer window last=%d null=%q", last, status[340])
	}
}

func TestColdCommandPointerTableRayBarriers(t *testing.T) {
	for _, tt := range []struct {
		name   string
		mutate func([]byte, *shadowCallbackAnalyzer)
		last   int
	}{
		{"outer window", func([]byte, *shadowCallbackAnalyzer) {}, 20 + 32*4},
		{"null slot", func(image []byte, _ *shadowCallbackAnalyzer) { image[0x11000+60], image[0x11001+60] = 0, 0 }, 20 + 32*4},
		{"code boundary", func(_ []byte, a *shadowCallbackAnalyzer) { a.starts[0x11000+60] = true }, 56},
		{"non-stream slot", func(image []byte, _ *shadowCallbackAnalyzer) { image[0x11000+60], image[0x11001+60] = 0x10, 0 }, 56},
		// Slot 40 points at a stream whose first fetch is slot 64's address:
		// the table cannot extend into data it points to.
		{"first pointed stream", func(image []byte, _ *shadowCallbackAnalyzer) { image[0x11000+40], image[0x11001+40] = 0x41, 0x90 }, 60},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image, results := commandWalkFixture(t, nil)
			root := resolveShadowCommandRoots(image, results)[0]
			for index := 16; index <= 400; index += 4 {
				image[0x11000+index], image[0x11001+index] = 0, 0xa1
			}
			second := ShadowCommandRootReference{TableIndex: 20}
			resolveShadowStreamPointer(image, root, &second)
			root.References = append(root.References, second)
			a := newShadowCallbackAnalyzer(image, nil, nil, nil)
			tt.mutate(image, a)
			got := extendColdCommandPointerNeighbors(image, []ShadowCommandRoot{root}, a)[0]
			last := 0
			for _, ref := range got.References {
				last = max(last, int(ref.TableIndex))
				if ref.TableIndex == 60 && (tt.name == "null slot" || tt.name == "code boundary" || tt.name == "non-stream slot") {
					t.Fatalf("referenced barrier slot: %+v", ref)
				}
			}
			if last != tt.last {
				t.Fatalf("last=%d want %d", last, tt.last)
			}
		})
	}
}

func TestColdCommandPointerAnchorsPoolOnlyAcrossIdenticalTables(t *testing.T) {
	image, results := commandWalkFixture(t, nil)
	root := resolveShadowCommandRoots(image, results)[0]
	for index := 16; index <= 400; index += 4 {
		image[0x11000+index], image[0x11001+index] = 0, 0xa1
	}
	other := root
	other.Context.PC++
	second := ShadowCommandRootReference{TableIndex: 24}
	resolveShadowStreamPointer(image, other, &second)
	other.References = []ShadowCommandRootReference{second}
	twin := root
	twin.Context.PC += 2
	twin.References = nil
	a := newShadowCallbackAnalyzer(image, nil, nil, nil)
	roots := []ShadowCommandRoot{root, other, twin}
	got := extendColdCommandPointerNeighbors(image, roots, a)
	for n, r := range got {
		status := map[int]string{}
		last := 0
		for _, ref := range r.References[len(roots[n].References):] {
			status[int(ref.TableIndex)] = ref.Status
			last = max(last, int(ref.TableIndex))
		}
		// Neither contextual root alone has two anchors. Pooled, they share one
		// table, which the reference-free twin probes too; probes never anchor.
		if status[16] != "open_interpolated_ROM_stream_reference" || status[20] != "open_interpolated_ROM_stream_reference" || last != 24+32*4 {
			t.Fatalf("root %d: last=%d statuses=%v", n, last, status)
		}
	}
	reversed := extendColdCommandPointerNeighbors(image, []ShadowCommandRoot{twin, other, root}, a)
	if !reflect.DeepEqual(got[0], reversed[2]) || !reflect.DeepEqual(got[1], reversed[1]) || !reflect.DeepEqual(got[2], reversed[0]) {
		t.Fatal("root order changed pooled anchors")
	}
	for _, kind := range []string{"cursor arithmetic", "table base", "stride", "unknown stream bank"} {
		t.Run(kind, func(t *testing.T) {
			o := other
			o.TableRead.Index.Operations = slices.Clone(other.TableRead.Index.Operations)
			switch kind {
			case "cursor arithmetic":
				o.FetchCursorDelta++
			case "table base":
				o.TableRead.Operand += 4
			case "stride":
				o.TableRead.Index.Operations = append(o.TableRead.Index.Operations, o.TableRead.Index.Operations[0])
			case "unknown stream bank":
				o.StreamBank.UnknownPaths = true
			}
			for _, r := range extendColdCommandPointerNeighbors(image, []ShadowCommandRoot{root, o}, a) {
				if len(r.References) != 1 {
					t.Fatalf("pooled across %s: %+v", kind, r.References)
				}
			}
		})
	}
}
