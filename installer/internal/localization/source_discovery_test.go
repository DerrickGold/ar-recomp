package localization

import (
	"bytes"
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

func TestSourceInstructionHelpers(t *testing.T) {
	rom := make([]byte, 0x10000)
	copy(rom[8:], []byte{0x20, 0x00, 0x81})
	copy(rom[0x8008:], []byte{0x20, 0x00, 0x81})
	copy(rom[0x8010:], []byte{0x22, 0x00, 0x81, 0})
	d := &Decoder{rom: rom}
	raw, err := d.callsTo(0x008100, "jsr", false)
	if err != nil || !reflect.DeepEqual(raw, []int{0x008008, 0x018008}) {
		t.Fatalf("raw calls: %v %v", raw, err)
	}
	local, err := d.callsTo(0x008100, "jsr", true)
	if err != nil || !reflect.DeepEqual(local, []int{0x008008}) {
		t.Fatalf("bank filtering: %v %v", local, err)
	}
	long, err := d.callsTo(0x008100, "jsl", true)
	if err != nil || !reflect.DeepEqual(long, []int{0x018010}) {
		t.Fatalf("long-call bank incorrectly filtered: %v %v", long, err)
	}
	if _, err := d.callsTo(0x008100, "jmp", true); err == nil {
		t.Fatal("unknown call kind")
	}
	if len(scanPattern([]byte{1, 1, 1}, []byte{1, 1})) != 2 {
		t.Fatal("overlapping match lost")
	}
	if len(scanPattern(rom, nil)) != 0 {
		t.Fatal("empty signature matched")
	}

	for _, opcode := range []byte{0x10, 0x30, 0x50, 0x70, 0x80, 0x90, 0xb0, 0xd0, 0xf0} {
		for displacement := -128; displacement <= 127; displacement++ {
			target, ok := branchTarget([]byte{opcode, byte(displacement)}, 0)
			if !ok || target != 2+displacement {
				t.Fatalf("short branch %02X %+d: %d", opcode, displacement, target)
			}
		}
	}
	for _, displacement := range []int{-32768, -1, 0, 1, 32767} {
		target, ok := branchTarget([]byte{0x82, byte(displacement), byte(displacement >> 8)}, 0)
		if !ok || target != 3+displacement {
			t.Fatal("long branch displacement")
		}
	}
	for _, data := range [][]byte{nil, {0x82}, {0x82, 0}, {0xea, 0}} {
		if _, ok := branchTarget(data, 0); ok {
			t.Fatal("invalid/truncated branch accepted")
		}
	}

	rom = make([]byte, 256)
	copy(rom[20:], []byte{0xa0, 0x01, 0x90, 0x80, 25}) // Branch to 50.
	copy(rom[30:], []byte{0xa0, 0x02, 0x90, 0x3a, 0xf0, 14})
	copy(rom[40:], []byte{0x80, 8}) // No proven LDY: not a source edge.
	d.rom = rom
	incoming, err := d.incomingY(0x8032)
	if err != nil || len(incoming) != 2 || incoming[0].Y != 0x9001 || incoming[1].Shape != "ldy_immediate_dec_a_then_branch" {
		t.Fatalf("branch shapes: %+v %v", incoming, err)
	}
	if !reflect.DeepEqual(candidateYs(incoming, 0x9001), []int{0x9001, 0x9002}) {
		t.Fatal("deduplication changed source order")
	}

	offset := 100
	copy(rom[offset-21:], []byte{0xa0, 1, 0x90, 0, 0, 0, 0, 0, 0xf0, 10, 0xa0, 2, 0x90, 0, 0, 0xf0, 3, 0xa0, 3, 0x90, 0x68})
	values, err := d.conditionalY(offsetPC(offset))
	if err != nil || !reflect.DeepEqual(values, []int{0x9001, 0x9002, 0x9003}) {
		t.Fatalf("conditional join: %v %v", values, err)
	}
	for _, position := range []int{offset - 21, offset - 11, offset - 4, offset - 1, offset - 13, offset - 12, offset - 6, offset - 5} {
		original := rom[position]
		rom[position] ^= 1
		if _, err := d.conditionalY(offsetPC(offset)); err == nil {
			t.Fatalf("damaged conditional proof accepted at %d", position)
		}
		rom[position] = original
	}
}

func TestSourceBoundsAndAtomicFailure(t *testing.T) {
	if len(sourceProfiles) != len(decoderFacts.Profiles) {
		t.Fatal("source/reader profile coverage differs")
	}
	for _, profile := range decoderFacts.Profiles {
		if _, ok := sourceProfiles[profile.ID]; !ok {
			t.Fatal("missing source profile", profile.ID)
		}
	}
	var absent *Decoder
	if result, err := absent.DiscoverNativeSources(); err == nil || result != nil {
		t.Fatal("nil decoder published census")
	}
	d := &Decoder{rom: make([]byte, 100)}
	if result, err := d.DiscoverNativeSources(); err == nil || result != nil {
		t.Fatal("unknown source profile published census")
	}
	for _, bounds := range [][2]int{{-1, 1}, {99, 2}, {0, -1}, {0, 101}, {101, 0}} {
		if _, err := d.span(bounds[0], bounds[1]); err == nil {
			t.Fatal("accepted span", bounds)
		}
	}
	for _, invocation := range [][3]int{{0x8000, -1, 0}, {0x8000, 0x4001, 0}, {0x8063, 1, 0}, {0x8000, 1, 0x80}} {
		if _, err := d.pointerTargets(invocation[0], invocation[1], invocation[2]); err == nil {
			t.Fatal("invalid pointer invocation", invocation)
		}
	}
	if _, err := d.pointerTargets(0x8000, 1, 0); err == nil {
		t.Fatal("RAM pointer accepted as ROM source")
	}
	copy(d.rom, []byte{0xff, 0xff})
	if _, err := d.pointerTargets(0x8000, 1, 0); err == nil {
		t.Fatal("pointer outside image")
	}
	copy(d.rom, []byte{0x20, 0x80})
	targets, err := d.pointerTargets(0x8000, 1, 0)
	if err != nil || !reflect.DeepEqual(targets, []int{0x8020}) {
		t.Fatal("valid pointer", targets, err)
	}
	if _, err := d.directY(0x8064); err == nil {
		t.Fatal("out-of-ROM call")
	}
	if y, err := d.directY(0x8000); err != nil || y != nil {
		t.Fatal("before-first-instruction underflow")
	}
	s := sourceDiscovery{d: d, references: []IRObject{}}
	if err := s.reference(0x8020, "test", 0x8000, "wrong"); err == nil || len(s.references) != 0 {
		t.Fatal("invalid provenance published")
	}
	for _, profile := range sourceProfiles {
		d.profile.ID = profile.ID
		result, err := d.DiscoverNativeSources()
		if err == nil || result != nil {
			t.Fatal("truncated image published partial census")
		}
	}
}

func FuzzSourceInstructionBounds(f *testing.F) {
	f.Add([]byte{0xa0, 0x34, 0x92, 0x80, 0, 0x82, 0, 0}, 0)
	f.Add([]byte{0xa0, 0x34, 0x92, 0x80, 0, 0x82, 0, 0}, 3)
	f.Add(bytes.Repeat([]byte{0xea}, 48), 32)
	f.Add([]byte{}, -1)
	f.Fuzz(func(t *testing.T, data []byte, index int) {
		if len(data) > 4096 {
			return
		}
		original := append([]byte{}, data...)
		d := &Decoder{rom: data}
		_, _ = branchTarget(data, index)
		address := index
		if index >= 0 && index < len(data) {
			address = offsetPC(index)
		}
		_, _ = d.directY(address)
		_, _ = d.incomingY(address)
		_, _ = d.conditionalY(address)
		_, _ = d.pointerTargets(address, 2, 0)
		if !bytes.Equal(original, data) {
			t.Fatal("source inspection mutated ROM")
		}
	})
}

func requireJSONEqual(t *testing.T, label string, actual, expected any) {
	t.Helper()
	canonical := func(value any) []string {
		t.Helper()
		data, err := json.Marshal(value)
		if err != nil {
			t.Fatal(err)
		}
		var normalized any
		if err := json.Unmarshal(data, &normalized); err != nil {
			t.Fatal(err)
		}
		data, err = json.MarshalIndent(normalized, "", "  ")
		if err != nil {
			t.Fatal(err)
		}
		return strings.Split(string(data), "\n")
	}
	got, want := canonical(actual), canonical(expected)
	for i := 0; i < min(len(got), len(want)); i++ {
		if got[i] != want[i] {
			t.Fatalf("%s differs at canonical line %d:\n%s\nactual: %s\nexpected: %s", label, i+1,
				strings.Join(want[max(0, i-4):i], "\n"), got[i], want[i])
		}
	}
	if len(got) != len(want) {
		t.Fatalf("%s differs in canonical length: %d != %d", label, len(got), len(want))
	}
}
