package tooling

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// Independent concrete arithmetic checks every represented byte value for
// all 3^8 byte domains, plus representative cross-byte word domains. These
// are arithmetic test oracles, not an interpreter used by the compiler.
func TestReturnKnownBitsSoundness(t *testing.T) {
	type operation struct {
		op          string
		imm         uint16
		carry       int8
		mask, value uint16
		outCarry    int8
	}
	check := func(width int, mask, value uint16) {
		w := returnBitsWord(mask, value)
		wm := returnWidthMask(width)
		m, v := returnWordBits(w, width)
		if m != mask || v != value {
			t.Fatalf("noncanonical mask=%04x value=%04x: %04x %04x", mask, value, m, v)
		}
		var ops []operation
		for _, op := range []string{"AND", "ORA", "EOR"} {
			for _, imm := range []uint16{0, 1, 0xf, 0x80, 0xff, 0x5555, 0xaaaa, 0xff00, 0xffff} {
				out := returnLogicBits(op, w, width, imm)
				m, v := returnWordBits(out, width)
				ops = append(ops, operation{op: op, imm: imm, mask: m, value: v, outCarry: -1})
			}
		}
		for _, op := range []string{"ASL", "LSR", "ROL", "ROR"} {
			for c := int8(-1); c <= 1; c++ {
				out, carry := returnShiftBits(op, w, width, c)
				m, v := returnWordBits(out, width)
				ops = append(ops, operation{op: op, carry: c, mask: m, value: v, outCarry: carry})
			}
		}
		for concrete := uint32(0); concrete <= uint32(wm); concrete++ {
			a := uint16(concrete)
			if a&mask != value {
				continue
			}
			for _, op := range ops {
				for carry := uint16(0); carry <= 1; carry++ {
					if op.carry >= 0 && carry != uint16(op.carry) {
						continue
					}
					var want, wantCarry uint16
					switch op.op {
					case "AND":
						want = a & op.imm
					case "ORA":
						want = a | op.imm
					case "EOR":
						want = a ^ op.imm
					case "ASL":
						want, wantCarry = a<<1, a>>(width*8-1)
					case "LSR":
						want, wantCarry = a>>1, a&1
					case "ROL":
						want, wantCarry = a<<1|carry, a>>(width*8-1)
					case "ROR":
						want, wantCarry = a>>1|(carry<<(width*8-1)), a&1
					}
					want &= wm
					if want&op.mask != op.value || (op.outCarry >= 0 && uint16(op.outCarry) != wantCarry) {
						t.Fatalf("width=%d source=(%04x,%04x) concrete=%04x %s imm=%04x C=%d => (%04x,%04x) C=%d excludes %04x C=%d", width, mask, value, a, op.op, op.imm, carry, op.mask, op.value, op.outCarry, want, wantCarry)
					}
				}
			}
		}
	}
	for mask := uint16(0); mask < 256; mask++ {
		for value := uint16(0); value < 256; value++ {
			if value & ^mask == 0 {
				check(1, mask, value)
			}
		}
	}
	for _, mask := range []uint16{0, 1, 0xff, 0xff00, 0x8000, 0xffff, 0xfffe, 0xfff0, 0x5555, 0xaaaa, 0xf00f} {
		values := []uint16{0, mask, 0x1234 & mask, 0xa55a & mask}
		slices.Sort(values)
		for _, value := range slices.Compact(values) {
			check(2, mask, value)
		}
	}
}

func TestReturnKnownBitsCanonicalAndOpaqueValues(t *testing.T) {
	for _, kind := range []uint8{returnValueUnknown, returnValuePC, returnValueStatus, returnValueBank} {
		if m, v := returnWordBits(returnSymbol(kind, 0xbeef), 2); m != 0 || v != 0 {
			t.Fatalf("symbolic token treated as numeric %d %04x %04x", kind, m, v)
		}
	}
	if w := returnBitsWord(0, 0xffff); w != (returnWord{}) {
		t.Fatal(w)
	}
	if w := returnBitsWord(0xffff, 0xbeef); w != returnSymbol(returnValueConstant, 0xbeef) {
		t.Fatal(w)
	}
	if w := returnBitsWord(0x00f0, 0xffff); w[0].value != 0xf0 || w[0].lane != 0xf0 || w[1] != (returnByte{}) {
		t.Fatal(w)
	}
}

func TestReturnKnownBitsIndexContracts(t *testing.T) {
	for _, tc := range []struct {
		name    string
		code    []byte
		operand uint32
		bounds  [2]uint32
		safe    bool
	}{
		{"constant index survives TXA ASL TAX", []byte{0xa2, 0, 0, 0x8a, 0x0a, 0xaa}, 0x7f0040, [2]uint32{0, 0}, true},
		{"mask bounds unknown word", []byte{0x29, 0xff, 0, 0xaa}, 0x7f0040, [2]uint32{0, 255}, true},
		{"mask plus two shifts", []byte{0x29, 0xff, 0, 0x0a, 0x0a, 0xaa}, 0x7f0040, [2]uint32{0, 1020}, true},
		{"ORA establishes lower bound and EOR retains mask", []byte{0x29, 0xff, 0, 0x09, 0, 0x10, 0x49, 0x80, 0, 0xaa}, 0x7f0040, [2]uint32{0x1000, 0x10ff}, true},
		{"mask crosses PHA PLA", []byte{0x29, 0xff, 0, 0x48, 0xa5, 0, 0x68, 0xaa}, 0x7f0040, [2]uint32{0, 255}, true},
		{"mask crosses TAX PHX PLX", []byte{0x29, 0xff, 0, 0xaa, 0xda, 0xa2, 0xff, 0xff, 0xfa}, 0x7f0040, [2]uint32{0, 255}, true},
		{"zero high X survives PHP SEP PLP", []byte{0x08, 0xe2, 0x10, 0x28, 0x8a, 0x0a, 0xaa}, 0x7f0040, [2]uint32{0, 510}, true},
		{"narrow A retains known B", []byte{0xa9, 0xff, 0xff, 0x08, 0xe2, 0x20, 0x29, 0, 0xaa, 0x28}, 0x7f2000, [2]uint32{0xff00, 0xff00}, false},
		{"narrow A does not clear unknown B", []byte{0x08, 0xe2, 0x20, 0x29, 0, 0xaa, 0x28}, 0x7f2000, [2]uint32{0, 0xff00}, false},
		{"XBA exchanges partial bytes", []byte{0x29, 0xff, 0, 0xeb, 0xaa}, 0x7f2000, [2]uint32{0, 0xff00}, false},
		{"carry clear bounds ROR", []byte{0x29, 0xff, 0, 0x18, 0x6a, 0xaa}, 0x7f8000, [2]uint32{0, 127}, true},
		{"unknown carry cannot bound ROR top bit", []byte{0x29, 0xff, 0, 0x6a, 0xaa}, 0x7f8000, [2]uint32{0, 0x807f}, false},
		{"ASL carry feeds following ROR", []byte{0xa9, 0, 0x80, 0x0a, 0x6a, 0xaa}, 0x7f8000, [2]uint32{0x8000, 0x8000}, false},
		{"ROL retains known one at bottom", []byte{0x29, 0x7f, 0, 0x38, 0x2a, 0xaa}, 0x7f0040, [2]uint32{1, 255}, true},
		{"LSR bound includes second store byte", []byte{0x4a, 0xaa}, 0x7f8000, [2]uint32{0, 0x7fff}, false},
		{"memory reload kills prior bound", []byte{0x29, 0xff, 0, 0xa5, 0, 0xaa}, 0x7f0040, [2]uint32{0, 0xffff}, false},
		{"memory AND is not an immediate mask", []byte{0x29, 0xff, 0, 0x25, 0, 0xaa}, 0x7f0040, [2]uint32{0, 0xffff}, false},
		{"alignment alone does not bound magnitude", []byte{0x0a, 0x0a, 0xaa}, 0x7f0040, [2]uint32{0, 0xfffc}, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			code := append(slices.Clone(tc.code), 0x9f, byte(tc.operand), byte(tc.operand>>8), byte(tc.operand>>16), 0x60)
			g, _, load := returnCallsFixture(t, map[uint32][]byte{0x8000: code}, decoder.Options{})
			r := auditShadowReturnCallMode(g, nil, load, true)
			if len(r.Writes) != 1 || *r.Writes[0].IndexRange != tc.bounds || (r.Status == "conditional_incoming_PC_preserved") != tc.safe {
				t.Fatalf("%+v", r)
			}
			if r.Writes[0].IndexBits != nil {
				bits := r.Writes[0].IndexBits
				if tc.bounds[0] != uint32(bits.Value) || tc.bounds[1] != uint32(bits.Value|^bits.Mask) {
					t.Fatal(bits)
				}
			}
		})
	}
}

func TestReturnKnownBitsNoFrameIdentityOrBranchAssumptions(t *testing.T) {
	for _, code := range [][]byte{
		{0x68, 0x29, 0xff, 0xff, 0x48, 0x60}, // no algebraic restoration of incoming-PC token
		{0x68, 0x4a, 0x0a, 0x48, 0x60},       // shifting a pulled return can discard a bit
		{0xa9, 0, 0, 0x29, 0xff, 0, 0xaa, 0xd0, 5, 0x9f, 0x40, 0, 0x7f, 0x60, 0x85, 0, 0x60},
	} {
		g, _, load := returnCallsFixture(t, map[uint32][]byte{0x8000: code}, decoder.Options{})
		r := auditShadowReturnCallMode(g, nil, load, true)
		if r.Status != "unproven" {
			t.Fatal(r)
		}
	}
}

func TestReturnKnownBitsReportOnlyIntegration(t *testing.T) {
	root := t.TempDir()
	romPath, cfgDir := filepath.Join(root, "fixture.sfc"), filepath.Join(root, "recomp")
	image := make([]byte, 0x8000)
	copy(image, []byte{0x08, 0x20, 0, 0x81, 0x28, 0x60})
	copy(image[0x100:], []byte{0x29, 0xff, 0, 0x0a, 0x0a, 0xaa, 0x9f, 0x40, 0, 0x7f, 0x60})
	if err := os.WriteFile(romPath, image, 0o644); err != nil {
		t.Fatal(err)
	}
	path, authored := filepath.Join(cfgDir, "bank00.cfg"), "bank = 00\nfunc Caller 8000 entry_mx:0,0\nfunc Leaf 8100 entry_mx:0,0\n"
	writeTestFile(t, path, authored)
	a, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 1})
	if err != nil {
		t.Fatal(err)
	}
	b, err := AnalyzeAuthoredShadow(ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 8})
	if err != nil {
		t.Fatal(err)
	}
	ja, _ := json.Marshal(a)
	jb, _ := json.Marshal(b)
	if !bytes.Equal(ja, jb) {
		t.Fatal("worker dependent report")
	}
	if a.ReturnAliases.Statuses["conditional_incoming_PC_preserved"] != 1 || a.ReturnCalls.Statuses["unproven"] != 1 || a.ReturnProvenance.Statuses["unproven"] != 1 {
		t.Fatalf("aliases=%+v calls=%+v local=%+v", a.ReturnAliases, a.ReturnCalls, a.ReturnProvenance)
	}
	facts, rejected := SelectStaticProvenDatabaseDispatchFacts(a)
	a.ReturnAliases = ShadowReturnCalls{}
	other, otherRejected := SelectStaticProvenDatabaseDispatchFacts(a)
	if !reflect.DeepEqual(facts, other) || rejected != otherRejected {
		t.Fatal("known bits entered production facts")
	}
	content, _ := os.ReadFile(path)
	entries, _ := os.ReadDir(root)
	if string(content) != authored || len(entries) != 2 {
		t.Fatal("analysis modified project")
	}
	var out bytes.Buffer
	writeShadowReturnAliases(&out, b.ReturnAliases, true)
	if !strings.Contains(out.String(), "X[$0000..$03FC]") || !strings.Contains(out.String(), "bits & $FC03 = $0000") {
		t.Fatal(out.String())
	}
}
