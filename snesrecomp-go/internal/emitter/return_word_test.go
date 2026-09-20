package emitter

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestReturnWordShuttleContracts(t *testing.T) {
	for _, tt := range []struct {
		name string
		code []byte
		m, x uint8
		want bool
	}{
		{"Y shuttle", []byte{0x7a, 0x1b, 0x5a, 0x60}, 0, 0, true},
		{"X shuttle", []byte{0xfa, 0x1b, 0xda, 0x60}, 1, 0, true},
		{"A shuttle", []byte{0x68, 0x9a, 0x48, 0x60}, 0, 0, true},
		{"guarded absolute store", []byte{0x7a, 0x1b, 0x5a, 0x9c, 0x10, 1, 0x60}, 0, 0, true},
		{"guarded direct store", []byte{0x7a, 0x1b, 0x5a, 0x64, 0x10, 0x60}, 1, 0, true},
		{"narrow Y", []byte{0x7a, 0x1b, 0x5a, 0x60}, 0, 1, false},
		{"narrow A", []byte{0x68, 0x9a, 0x48, 0x60}, 1, 0, false},
		{"wrong pushed register", []byte{0x7a, 0x1b, 0xda, 0x60}, 0, 0, false},
		{"destroyed shuttle value", []byte{0x7a, 0x1b, 0xc8, 0x5a, 0x60}, 0, 0, false},
		{"call between push and return", []byte{0x7a, 0x1b, 0x5a, 0x20, 0, 0x81, 0x60}, 0, 0, false},
		{"unmodeled store", []byte{0x7a, 0x1b, 0x5a, 0x85, 0x10, 0x60}, 0, 0, false},
		{"long return not a short word", []byte{0x7a, 0x1b, 0x5a, 0x6b}, 0, 0, false},
		{"entry bypasses witness", []byte{0xd0, 2, 0x7a, 0x1b, 0x5a, 0x60}, 0, 0, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			image := make(rom.Image, 0x8000)
			copy(image, tt.code)
			r, err := EmitFunction(image, 0, 0x8000, tt.m, tt.x, FunctionOptions{})
			if err != nil {
				t.Fatal(err)
			}
			if strings.Contains(r.Source, "cpu_capture_return_word") != tt.want || strings.Contains(r.Source, "cpu_accept_return_word_relocation") != tt.want {
				t.Fatalf("contract=%t\n%s", tt.want, r.Source)
			}
			if tt.want {
				if !strings.Contains(r.Source, "cpu_accept_adjusted_return") || !strings.Contains(r.Source, "cpu_accept_stacked_result_return") || !strings.Contains(r.Source, "cpu_resolve_ancestor_skip") {
					t.Fatal("legacy guards lost")
				}
				if strings.Contains(tt.name, "guarded") && !strings.Contains(r.Source, "cpu_return_word_store_disjoint") {
					t.Fatal("store assumed nonaliasing")
				}
			}
		})
	}
}

func TestReturnWordShuttleHonorsOverrides(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0x7a, 0x1b, 0x5a, 0x60})
	for _, opts := range []FunctionOptions{
		{HLEFunction: "hook"},
		{HLEDispatch: map[uint16]string{0x8002: "hook"}},
		{Decode: decoder.Options{NativeReturnBarriers: [][2]uint32{{0x8001, 0x8001}}}},
		{ExcludeRanges: [][2]uint16{{0x8000, 0x8003}}},
	} {
		r, err := EmitFunction(image, 0, 0x8000, 0, 0, opts)
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(r.Source, "cpu_capture_return_word") {
			t.Fatalf("override bypassed: %+v", opts)
		}
	}
	// Conditional HLE preserves its explicit hook and guards the ROM path.
	r, err := EmitFunction(image, 0, 0x8000, 0, 0, FunctionOptions{HLEFunctionIf: config.HLEFunctionIf{Function: "hook", Predicate: "use_hook"}})
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(r.Source, "if (use_hook(cpu))") || !strings.Contains(r.Source, "hook(cpu)") || !strings.Contains(r.Source, "cpu_capture_return_word") {
		t.Fatal("conditional HLE or its original ROM path was dropped")
	}
}

func TestReturnWordShuttleRetainsExitMXDiagnostic(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0x7a, 0x1b, 0x5a, 0x60})
	r, err := EmitFunction(image, 0, 0x8000, 0, 0, FunctionOptions{ExitMX: &decoder.MX{M: 0, X: 0}})
	if err != nil {
		t.Fatal(err)
	}
	start := strings.Index(r.Source, "cpu_accept_return_word_relocation")
	if start < 0 || strings.Index(r.Source[start:], "sr_exit_mx_check") < 0 || strings.Index(r.Source[start:], "sr_exit_mx_check") > strings.Index(r.Source[start:], "return RECOMP_RETURN_NORMAL") {
		t.Fatal("witnessed return bypassed the configured exit-M/X check")
	}
}
