package emitter

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// buildGuard renders emitRTSDispatchGuard for a bank-03 RTS-trick dispatch at
// $03:A119 whose single target is $9EF4, with the given set of decoded local
// variants of that target.
func buildGuard(localVariants [][2]uint8) []string {
	ins := &cpu65816.Instruction{
		Address:         0x03A119,
		Mnemonic:        "RTS",
		DispatchKind:    "rts_trick",
		DispatchEntries: []uint32{0x03_9EF4},
	}
	local := map[decoder.DecodeKey]struct{}{}
	for _, mx := range localVariants {
		local[decoder.DecodeKey{PC: 0x03_9EF4, M: mx[0], X: mx[1]}] = struct{}{}
	}
	return emitRTSDispatchGuard(ins, local)
}

// TestRTSGuardSelectsRuntimeVariant is the details.md scenario: the target
// $9EF4 was decoded at both M0X0 and M1X0 in the enclosing function. The guard
// must offer BOTH labels gated on runtime m/x, not hardcode one.
func TestRTSGuardSelectsRuntimeVariant(t *testing.T) {
	src := strings.Join(buildGuard([][2]uint8{{0, 0}, {1, 0}}), "\n")
	for _, want := range []string{
		"case 0x9EF4:",
		"if (_rts_mx == 0) { cpu->S = (uint16)(_rts_s + 2); goto L_9EF4_M0X0; }",
		"if (_rts_mx == 2) { cpu->S = (uint16)(_rts_s + 2); goto L_9EF4_M1X0; }",
	} {
		if !strings.Contains(src, want) {
			t.Errorf("guard missing %q:\n%s", want, src)
		}
	}
	for _, forbidden := range []string{"goto L_9EF4_M0X1", "goto L_9EF4_M1X1"} {
		if strings.Contains(src, forbidden) {
			t.Errorf("guard offers undecoded variant %q:\n%s", forbidden, src)
		}
	}
}

// TestRTSGuardRefusesWrongWidth verifies that when only the M0X0 body was
// decoded (the original bug's exact input), a runtime m=1 dispatch does NOT
// jump into the M0X0 body — it falls through to the width-refusal return.
func TestRTSGuardRefusesWrongWidth(t *testing.T) {
	src := strings.Join(buildGuard([][2]uint8{{0, 0}}), "\n")
	if !strings.Contains(src, "if (_rts_mx == 0) { cpu->S = (uint16)(_rts_s + 2); goto L_9EF4_M0X0; }") {
		t.Errorf("guard should still jump for the decoded M0X0 case:\n%s", src)
	}
	if strings.Contains(src, "_rts_mx == 2") {
		t.Errorf("guard must not offer an M1X0 goto when only M0X0 was decoded:\n%s", src)
	}
	if !strings.Contains(src, "rts_dispatch_width") {
		t.Errorf("guard should emit the width-refusal diagnostic:\n%s", src)
	}
	if strings.Contains(src, "case 0x9EF4: cpu->S") {
		t.Errorf("S adjusted before the runtime-m/x guard (double-adjust risk):\n%s", src)
	}
}

// TestRTSGuardNoVariantDecoded keeps the pre-existing behavior for a target no
// variant of which was decoded locally: a comment, no goto, and the unknown
// default path is still present.
func TestRTSGuardNoVariantDecoded(t *testing.T) {
	src := strings.Join(buildGuard(nil), "\n")
	if strings.Contains(src, "goto L_9EF4") {
		t.Errorf("guard must not goto an undecoded target:\n%s", src)
	}
	if !strings.Contains(src, "no variant decoded in this function") {
		t.Errorf("guard should note the undecoded target:\n%s", src)
	}
	if !strings.Contains(src, "rts_dispatch_miss") {
		t.Errorf("guard should retain the unregistered-target default:\n%s", src)
	}
}
