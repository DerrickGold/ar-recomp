package tooling

import (
	"bytes"
	"encoding/json"
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// Original redistributable native code. The callback is entered with saved
// DB/Y, NOT JSR, and returns through shared cleanup. No game bytes/addresses.
func commandCallbackFixture(t *testing.T, body []byte) (romimage.Image, ShadowCommandStream, ShadowCommandPrefix) {
	t.Helper()
	i := make(romimage.Image, 0x8000)
	copy(i, []byte{0xb9, 0, 0}) // fetch
	copy(i[0x800:], []byte{0xb9, 0, 0, 0x85, 0x42, 0xc8, 0xc8, 0x8b, 0x4b, 0xab, 0x5a, 0x6c, 0x42, 0})
	copy(i[0x1000:], body)
	copy(i[0x1100:], []byte{0x7a, 0xab, 0x4c, 0, 0x80}) // PLY; PLB; JMP fetch
	g, err := decoder.DecodeFunction(i, 0, 0x8800, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	c := summarizeShadowCommandPrefix(g, g.Entry, 0)
	if c.Callback == nil || c.Callback.Frame == nil {
		t.Fatalf("missing frame %+v", c)
	}
	s := ShadowCommandStream{SelectorPC: 0x8100, FetchPC: 0x8000, TagByteOffset: 1, SelectorMask: 3, EntryCursorDelta: 2, Commands: []ShadowCommandPrefix{c}}
	return i, s, c
}

func TestCommandCallbackSavedCursorAndOwnedCalls(t *testing.T) {
	for _, tt := range []struct {
		name           string
		code           []byte
		delta, returns int
	}{
		{"shared cleanup", []byte{0x4c, 0, 0x91}, 2, 0},
		{"known carry excludes bad return", []byte{0x18, 0xb0, 3, 0x4c, 0, 0x91, 0x60}, 2, 0},
		{"saved cursor rewritten", []byte{0x68, 0x1a, 0x48, 0x4c, 0, 0x91}, 3, 0},
		{"explicit binary addition", []byte{0x68, 0xd8, 0x18, 0x69, 3, 0, 0x48, 0x4c, 0, 0x91}, 5, 0},
		{"explicit binary subtraction", []byte{0x68, 0xd8, 0x38, 0xe9, 1, 0, 0x48, 0x4c, 0, 0x91}, 1, 0},
		{"saved Y survives clobbered live Y", []byte{0xa0, 0, 0x77, 0x4c, 0, 0x91}, 2, 0},
		{"balanced short call", []byte{0x20, 0, 0x92, 0x4c, 0, 0x91}, 2, 1},
		{"balanced long mirrored call", []byte{0x22, 0, 0x93, 0x80, 0x4c, 0, 0x91}, 2, 1},
		{"nested calls", []byte{0x22, 0, 0x94, 0x80, 0x4c, 0, 0x91}, 2, 2},
	} {
		t.Run(tt.name, func(t *testing.T) {
			i, s, c := commandCallbackFixture(t, tt.code)
			copy(i[0x1200:], []byte{0x48, 0xa9, 0, 0, 0x85, 0x20, 0x68, 0x60})
			copy(i[0x1300:], []byte{0x8b, 0x4b, 0xab, 0xa0, 0, 0, 0xab, 0x6b})
			copy(i[0x1400:], []byte{0x20, 0, 0x92, 0x6b})
			before := slices.Clone(i)
			a := newShadowCallbackAnalyzer(i, nil, nil, nil)
			paths := a.paths(s, c, 0x9000)
			if len(paths) != 1 || paths[0].Status != "owned_native_refetch" || paths[0].CursorDelta != tt.delta || paths[0].StackBytes != 0 || paths[0].DBSource != "entry_db" || len(paths[0].Returns) != tt.returns {
				t.Fatalf("paths %+v", paths)
			}
			if !bytes.Equal(before, i) || !reflect.DeepEqual(paths, a.paths(s, c, 0x9000)) {
				t.Fatal("mutated image or unstable cache")
			}
			f := c.Callback.Frame
			if len(f.Stack) != 3 || f.Stack[0].Source != "entry_db" || f.Stack[1].Source != "entry_y" || f.Stack[1].Part != 1 || f.Stack[2].Part != 0 || f.Stack[2].Delta != 2 || f.DB.Source != "constant" {
				t.Fatalf("wrong frame order: %+v", f)
			}
		})
	}
}

func TestCommandCallbackBarriers(t *testing.T) {
	for _, tt := range []struct {
		name   string
		body   []byte
		reason string
	}{
		{"RTS is not PLY", []byte{0x60}, "saved_data_is_not_return_frame"},
		{"RTL is not PLY PLB", []byte{0x6b}, "saved_data_is_not_return_frame"},
		{"decimal unknown", []byte{0x68, 0x18, 0x69, 3, 0}, "saved_cursor_decimal_unproven"},
		{"decimal set", []byte{0x68, 0xf8, 0x18, 0x69, 3, 0}, "saved_cursor_decimal_unproven"},
		{"carry unknown", []byte{0x68, 0xd8, 0x69, 3, 0}, "saved_cursor_carry_unproven"},
		{"borrow caller bytes", []byte{0x68, 0x68}, "opaque_caller_stack_read"},
		{"lost cursor", []byte{0x68, 0xa9, 1, 0, 0x48, 0x4c, 0, 0x91}, "refetch_frame_or_cursor_unproven"},
		{"wrong bank restore", []byte{0x7a, 0xab, 0x4b, 0xab, 0x4c, 0, 0x80}, "refetch_frame_or_cursor_unproven"},
		{"unconsumed frame", []byte{0x4c, 0, 0x80}, "refetch_frame_or_cursor_unproven"},
		{"width change", []byte{0xe2, 0x20}, "callback_width_change"},
		{"unknown restored status", []byte{0x28}, "callback_unsupported_PLP"},
		{"stack relative write", []byte{0x83, 1}, "callback_stack_relative_access"},
		{"stack relative read", []byte{0xa3, 1}, "callback_stack_relative_access"},
		{"stack relocation", []byte{0x1b}, "callback_unsupported_TCS"},
		{"dynamic jump", []byte{0x6c, 0x40, 0}, "callback_dynamic_transfer"},
		{"dynamic call", []byte{0xfc, 0, 0x92}, "callback_dynamic_call"},
		{"cycle", []byte{0x80, 0xfe}, "callback_cycle_or_repeated_native_site"},
		{"BRK", []byte{0, 0}, "callback_unsupported_BRK"},
		{"call wrong return kind", []byte{0x20, 0, 0x92}, "mismatched_native_return"},
		{"modified return", []byte{0x20, 0, 0x93}, "modified_native_return_frame"},
	} {
		t.Run(tt.name, func(t *testing.T) {
			i, s, c := commandCallbackFixture(t, tt.body)
			copy(i[0x1200:], []byte{0x6b})
			copy(i[0x1300:], []byte{0x68, 0xa9, 0, 0x90, 0x48, 0x60})
			paths := newShadowCallbackAnalyzer(i, nil, nil, nil).paths(s, c, 0x9000)
			if len(paths) != 1 || paths[0].Status != tt.reason {
				t.Fatalf("paths %+v", paths)
			}
		})
	}
}

func TestCommandCallbackPEAAndFrameUnknown(t *testing.T) {
	i, s, c := commandCallbackFixture(t, []byte{0x60})
	// Insert an actual PEA before the callback dispatch. Its target+1 is the
	// shared cleanup, and that cleanup still consumes Y/DB itself.
	copy(i[0x80b:], []byte{0xf4, 0xff, 0x90, 0x6c, 0x42, 0})
	g, err := decoder.DecodeFunction(i, 0, 0x8800, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	c = summarizeShadowCommandPrefix(g, g.Entry, 0)
	p := newShadowCallbackAnalyzer(i, nil, nil, nil).paths(s, c, 0x9000)
	if len(p) != 1 || p[0].Status != "owned_native_refetch" || len(p[0].Returns) != 1 || p[0].Returns[0].TargetPC != 0x9100 || p[0].Returns[0].Source != "command_pea" {
		t.Fatalf("PEA %+v", p)
	}
	c.Callback.Frame.Complete = false
	p = newShadowCallbackAnalyzer(i, nil, nil, nil).paths(s, c, 0x9000)
	if len(p) != 1 || p[0].Status != "callback_frame_unknown" {
		t.Fatal(p)
	}
	// Prefix-level PLB must record that it borrowed the opaque caller stack.
	copy(i[0x800:], []byte{0xab, 0xb9, 0, 0, 0x85, 0x42, 0x6c, 0x42, 0})
	g, err = decoder.DecodeFunction(i, 0, 0x8800, 0, 0, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	c = summarizeShadowCommandPrefix(g, g.Entry, 0)
	if c.Callback == nil || c.Callback.Frame.Complete {
		t.Fatal("caller stack borrowing hidden")
	}
}

func TestCommandCallbackOwnershipAndBudgets(t *testing.T) {
	for _, tt := range []struct {
		name, reason string
		cfg          config.Config
	}{
		{"HLE mirror", "native_HLE_boundary", config.Config{HLEFunctions: map[uint16]string{0x9000: "hook"}}},
		{"exclude", "authored_exclude_boundary", config.Config{ExcludeRanges: []config.Range{{Start: 0x9000, End: 0x9001}}}},
		{"data operand byte", "authored_data_boundary", config.Config{DataRegions: []config.DataRegion{{Bank: 0x80, Start: 0x9001, End: 0x9002}}}},
		{"forced width", "authored_width_override", config.Config{ForceVariantAt: map[uint32]config.MX{0x809000: {M: 1}}}},
	} {
		t.Run(tt.name, func(t *testing.T) {
			i, s, c := commandCallbackFixture(t, []byte{0x4c, 0, 0x91})
			p := newShadowCallbackAnalyzer(i, []shadowBank{{ID: 0x80, Config: &tt.cfg}}, nil, nil).paths(s, c, 0x9000)
			if len(p) != 1 || p[0].Status != tt.reason {
				t.Fatal(p)
			}
		})
	}
	i, s, c := commandCallbackFixture(t, []byte{0x4c, 0, 0x91})
	a := newShadowCallbackAnalyzer(i, nil, nil, []ShadowTableSpan{{StartPC: 0x809001, EndExclusive: 0x809003}})
	if p := a.paths(s, c, 0x9000); p[0].Status != "dispatch_table_boundary" {
		t.Fatal(p)
	}
	a = newShadowCallbackAnalyzer(i, nil, nil, nil)
	a.interiors[0x1000] = true
	if p := a.paths(s, c, 0x9000); p[0].Status != "conflicting_decoded_byte_ownership" {
		t.Fatal(p)
	}
	if p := newShadowCallbackAnalyzer(i, nil, nil, nil).paths(s, c, 0x7e9000); p[0].Status != "callback_not_ROM" {
		t.Fatal(p)
	}
	i[0x7fff] = 0xea
	if p := newShadowCallbackAnalyzer(i, nil, nil, nil).paths(s, c, 0xffff); p[0].Status != "callback_bank_boundary" {
		t.Fatal(p)
	}
	for _, tt := range []struct {
		name, reason string
		body         []byte
	}{
		{"instructions", "callback_instruction_budget", bytes.Repeat([]byte{0xea}, shadowCallbackInstructionLimit+1)},
		{"stack", "callback_stack_budget", bytes.Repeat([]byte{0x48}, shadowCallbackStackLimit)},
		{"branches", "callback_path_budget", bytes.Repeat([]byte{0xd0, 1, 0xea}, 20)},
	} {
		t.Run(tt.name, func(t *testing.T) {
			i, s, c := commandCallbackFixture(t, tt.body)
			p := newShadowCallbackAnalyzer(i, nil, nil, nil).paths(s, c, 0x9000)
			if len(p) > shadowCallbackPathLimit {
				t.Fatal("path cap", len(p))
			}
			for _, path := range p {
				if path.Status != tt.reason {
					t.Fatalf("budget %+v", p)
				}
			}
		})
	}
	for n := 0; n < 10; n++ {
		copy(i[0x1000+n*4:], []byte{0x20, byte(0x9004 + n*4), byte((0x9004 + n*4) >> 8), 0x60})
	}
	if p := newShadowCallbackAnalyzer(i, nil, nil, nil).paths(s, c, 0x9000); p[0].Status != "callback_call_depth" {
		t.Fatal(p)
	}
}

func TestCommandCallbackOwnedBoundariesAndFiniteWork(t *testing.T) {
	i, s, c := commandCallbackFixture(t, []byte{0xa9, 0, 0, 0x4c, 1, 0x90})
	if p := newShadowCallbackAnalyzer(i, nil, nil, nil).paths(s, c, 0x9000); p[0].Status != "conflicting_scout_byte_ownership" {
		t.Fatal(p)
	}
	// A known instruction start inside a freshly scouted instruction is also
	// a conflict, not just entering the interior of an existing instruction.
	r := []shadowDecodeResult{{entry: decoder.Variant{Address: 0x8800}, instructions: []shadowDecodedInstruction{{PC: 0x9001, Instruction: cpu65816.Instruction{Length: 1}}}}}
	if p := newShadowCallbackAnalyzer(i, nil, r, nil).paths(s, c, 0x9000); p[0].Status != "conflicting_decoded_byte_ownership" {
		t.Fatal(p)
	}
	r[0].instructions[0].PC = 0x9000
	end := uint16(0x880a)
	banks := []shadowBank{{ID: 0x80, Config: &config.Config{HLEFunctionsIf: map[uint16]config.HLEFunctionIf{0x8800: {Function: "hook"}}}},
		{ID: 0, Config: &config.Config{Entries: []config.Entry{{Start: 0x8800, End: &end}}}}}
	a := newShadowCallbackAnalyzer(i, banks, r, nil)
	if p := a.paths(s, c, 0x9000); p[0].Status != "authored_body_override" {
		t.Fatal(p)
	}
	if a.blocked[0x1000] != "authored_body_override" {
		t.Fatal("internal HLE/body entry not blocked")
	}
	slices.Reverse(banks)
	if !reflect.DeepEqual(a.blocked, newShadowCallbackAnalyzer(i, banks, r, nil).blocked) {
		t.Fatal("override ordering changes evidence")
	}
	a = newShadowCallbackAnalyzer(i, nil, nil, nil)
	a.remaining = 1
	if p := a.paths(s, c, 0x9000); p[0].Status != "callback_total_work_budget" {
		t.Fatal(p)
	}
	// A partial/mixed PEA word must not turn other saved bytes into a return.
	i, s, c = commandCallbackFixture(t, []byte{0x60})
	c.Callback.Frame.Stack = append(c.Callback.Frame.Stack,
		ShadowCommandStackByte{Source: "constant", Value: 0x90, Part: 0, PushPC: 0x8870},
		ShadowCommandStackByte{Source: "constant", Value: 0xff, Part: 0, PushPC: 0x8870})
	if p := newShadowCallbackAnalyzer(i, nil, nil, nil).paths(s, c, 0x9000); p[0].Status != "saved_data_is_not_return_frame" {
		t.Fatal(p)
	}
}

func TestCommandCallbackWalkBranchesAndNoFactPromotion(t *testing.T) {
	// Both unknown outcomes remain visible: one skips an extra stream byte
	// by changing saved Y; one preserves it. They must never be conflated.
	i, s, _ := commandCallbackFixture(t, []byte{0xd0, 3, 0x68, 0x1a, 0x48, 0x4c, 0, 0x91})
	copy(i[0x2000:], []byte{0, 0x80, 0, 0x90, 0, 0, 0, 0})
	first := uint32(0xa000)
	roots := []ShadowCommandRoot{{SelectorPC: s.SelectorPC, FetchPC: s.FetchPC, References: []ShadowCommandRootReference{{StreamPC: &first, FirstFetchPC: &first}}}}
	a := newShadowCallbackAnalyzer(i, nil, nil, nil)
	w := walkShadowCommandStreamsWithCallbacks(i, []ShadowCommandStream{s}, roots, a)
	if len(w) != 2 {
		t.Fatal(w)
	}
	for n, path := range w {
		if len(path.Steps) != 1 || path.Steps[0].CallbackPath == nil || path.Steps[0].NextPC == nil || *path.Steps[0].NextPC != 0xa005-uint32(n) || len(path.Steps[0].CallbackPath.Branches) != 1 || path.StopReason != "untagged_data_path_unmodeled" {
			t.Fatalf("walk %+v", path)
		}
	}
	before, _ := json.Marshal(s)
	if !reflect.DeepEqual(w, walkShadowCommandStreamsWithCallbacks(i, []ShadowCommandStream{s}, roots, a)) {
		t.Fatal("unstable walks")
	}
	after, _ := json.Marshal(s)
	if !bytes.Equal(before, after) {
		t.Fatal("mutated streams")
	}
	report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandStreams: []ShadowCommandStream{s}, CommandWalks: w}
	data, err := json.Marshal(report)
	if err != nil {
		t.Fatal(err)
	}
	var read ShadowReport
	if err := json.Unmarshal(data, &read); err != nil || !reflect.DeepEqual(read.CommandWalks, w) {
		t.Fatal("JSON", err)
	}
	var text bytes.Buffer
	if err := WriteShadowReport(&text, report, "text", true); err != nil || !strings.Contains(text.String(), "owned_native_refetch") {
		t.Fatal(text.String(), err)
	}
	facts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
	if len(facts) != 0 || len(SelectStaticProvenRoutineEntryFacts(report)) != 0 {
		t.Fatal("conditional native scout became generation facts")
	}
	if len(walkShadowCommandStreamsWithCallbacks(i, []ShadowCommandStream{s}, nil, a)) != 0 {
		t.Fatal("unrooted scout")
	}
}
