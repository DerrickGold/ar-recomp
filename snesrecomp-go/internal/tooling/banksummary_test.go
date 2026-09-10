package tooling

import (
	"bytes"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func bankProgramFixture(t *testing.T, image romimage.Image, pc uint16, m, x uint8) *shadowDBProgram {
	t.Helper()
	g, err := decoder.DecodeFunction(image, 0, pc, m, x, decoder.Options{})
	if err != nil {
		t.Fatal(err)
	}
	return collectShadowDBProgram(image, g)
}

func TestBankSummaryLocalStackAndAliases(t *testing.T) {
	for _, test := range []struct {
		name             string
		code             []byte
		valid, preserves bool
	}{
		{"leaf", []byte{0x60}, true, true},
		{"memory write without saved DB", []byte{0x8d, 0, 0x20, 0x60}, true, true},
		{"balanced PHB and temporary bank", []byte{0x8b, 0x4b, 0xab, 0xab, 0x60}, true, true},
		{"temporary A push", []byte{0x8b, 0x48, 0x68, 0xab, 0x60}, true, true},
		{"PHP restored", []byte{0x08, 0xe2, 0x30, 0x28, 0x60}, true, true},
		{"bank changed", []byte{0x4b, 0xab, 0x60}, true, false},
		{"memory aliases saved DB", []byte{0x8b, 0x8d, 0, 0x20, 0xab, 0x60}, true, false},
		{"memory aliases status", []byte{0x08, 0x8d, 0, 0x20, 0x28, 0x60}, false, false},
		{"PLB outside frame", []byte{0xab, 0x60}, false, false},
		{"unbalanced return", []byte{0x8b, 0x60}, false, false},
		{"TXS", []byte{0x9a, 0x60}, false, false},
		{"RTI", []byte{0x40}, false, false},
		{"block move", []byte{0x54, 0, 0, 0x60}, false, false},
		{"BRK", []byte{0, 0, 0x60}, false, false},
		{"WDM", []byte{0x42, 0, 0x60}, false, false},
		{"external tail", []byte{0x5c, 0, 0x80, 1}, false, false},
		{"stack budget", append(bytes.Repeat([]byte{0x8b}, shadowDBStackLimit+1), 0x60), false, false},
		{"infinite loop", []byte{0x80, 0xfe}, false, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			image := make(romimage.Image, 0x8000)
			copy(image, test.code)
			p := bankProgramFixture(t, image, 0x8000, 0, 0)
			s := evaluateShadowDB(p, nil, nil, nil).summary
			if s.valid != test.valid || s.preserves != test.preserves {
				t.Fatalf("summary=%+v", s)
			}
		})
	}
}

func TestBankSummaryDirectCallsAndFixedPoint(t *testing.T) {
	image := make(romimage.Image, 0x8000)
	copy(image, []byte{0x4b, 0xab, 0x20, 0, 0x81, 0xad, 0, 0x90, 0x60})
	copy(image[0x100:], []byte{0x20, 0, 0x82, 0x60})
	image[0x200] = 0x60
	programs := make(map[decoder.Variant]*shadowDBProgram)
	for _, pc := range []uint16{0x8000, 0x8100, 0x8200} {
		p := bankProgramFixture(t, image, pc, 0, 0)
		programs[p.entry] = p
	}
	summaries := buildShadowDBSummaries(programs, nil)
	p := programs[decoder.Variant{Address: 0x8000}]
	q := shadowBankQuery(p, summaries, nil, decoder.Variant{Address: 0x8005})
	if q.Status != "local_constant_set" || len(q.Banks) != 1 || q.Banks[0] != 0 || len(q.Calls) != 1 || q.Calls[0].PreservingCount != 1 {
		t.Fatalf("call preservation: %+v", q)
	}
	if !strings.Contains(shadowStoredJSONKey(q), `"constant_banks":[0]`) {
		t.Fatal("bank values encoded as opaque bytes")
	}
	if atHook := shadowBankQuery(p, summaries, map[uint32]bool{0x8005: true}, decoder.Variant{Address: 0x8005}); atHook.Status != "blocked" {
		t.Fatal("queried through HLE at source PC")
	}
	if !summaries[decoder.Variant{Address: 0x8100}].preserves {
		t.Fatal("fixed point did not propagate leaf summary")
	}
	q = shadowBankQuery(p, summaries, map[uint32]bool{0x8100: true}, decoder.Variant{Address: 0x8005})
	if q.Status != "blocked" || q.Calls[0].Blockers[0].Reason != "HLE_target_contract" {
		t.Fatalf("HLE crossed: %+v", q)
	}
	// Recursive groups cannot establish their own proof merely by assuming
	// the other member preserves DB.
	copy(image[0x200:], []byte{0x20, 0, 0x81, 0x60})
	programs[decoder.Variant{Address: 0x8200}] = bankProgramFixture(t, image, 0x8200, 0, 0)
	summaries = buildShadowDBSummaries(programs, nil)
	if summaries[decoder.Variant{Address: 0x8100}].valid || summaries[decoder.Variant{Address: 0x8200}].valid {
		t.Fatal("recursive preservation bootstrapped")
	}
}

func TestBankSummaryCallerSavedDBAndCalleeAliases(t *testing.T) {
	for _, memoryWrite := range []bool{false, true} {
		image := make(romimage.Image, 0x8000)
		copy(image, []byte{0x8b, 0x20, 0, 0x81, 0xab, 0x60})
		callee := []byte{0x4b, 0xab}
		if memoryWrite {
			callee = append(callee, 0x8d, 0, 0x20)
		}
		callee = append(callee, 0x60)
		copy(image[0x100:], callee)
		a, b := bankProgramFixture(t, image, 0x8000, 0, 0), bankProgramFixture(t, image, 0x8100, 0, 0)
		s := buildShadowDBSummaries(map[decoder.Variant]*shadowDBProgram{a.entry: a, b.entry: b}, nil)
		if !s[a.entry].valid || s[a.entry].preserves == memoryWrite {
			t.Fatalf("caller alias contract: %+v", s[a.entry])
		}
	}
}

func TestBankSummaryCallReturnKindAndMX(t *testing.T) {
	for _, test := range []struct {
		name       string
		call, body []byte
		reason     string
	}{
		{"JSL cannot use RTS", []byte{0x22, 0, 0x81, 0}, []byte{0x60}, "callee_return_kind_mismatch"},
		{"JSR cannot use RTL", []byte{0x20, 0, 0x81}, []byte{0x6b}, "callee_return_kind_mismatch"},
		{"missing exit width", []byte{0x20, 0, 0x81}, []byte{0xe2, 0x20, 0x60}, "exit_MX_not_in_decoded_successors"},
	} {
		t.Run(test.name, func(t *testing.T) {
			image := make(romimage.Image, 0x8000)
			copy(image, append(test.call, 0x60))
			copy(image[0x100:], test.body)
			a, b := bankProgramFixture(t, image, 0x8000, 0, 0), bankProgramFixture(t, image, 0x8100, 0, 0)
			s := buildShadowDBSummaries(map[decoder.Variant]*shadowDBProgram{a.entry: a, b.entry: b}, nil)
			r := evaluateShadowDB(a, s, nil, nil)
			if r.summary.valid || !strings.Contains(shadowStoredJSONKey(r)+shadowStoredJSONKey(r.checks)+shadowStoredJSONKey(r.summary.blockers), test.reason) {
				t.Fatalf("return contract: %+v / %+v", r.summary, r.checks)
			}
		})
	}
}

func TestBankSummaryIndirectSupersetCompleteness(t *testing.T) {
	for _, bounded := range []bool{false, true} {
		image := make(romimage.Image, 0x8000)
		code := []byte{0x4b, 0xab}
		if bounded {
			code = append(code, 0xa2, 0, 0)
		}
		pc := uint32(0x8000 + len(code))
		code = append(code, 0xfc, 0, 0x90, 0x60)
		copy(image, code)
		copy(image[0x1000:], []byte{0, 0x81})
		image[0x100] = 0x60
		a, b := bankProgramFixture(t, image, 0x8000, 0, 0), bankProgramFixture(t, image, 0x8100, 0, 0)
		s := buildShadowDBSummaries(map[decoder.Variant]*shadowDBProgram{a.entry: a, b.entry: b}, nil)
		r := evaluateShadowDB(a, s, nil, nil)
		if r.summary.valid != bounded || len(r.checks) != 1 || r.checks[0].PC != pc || r.checks[0].TargetSetClosed != bounded {
			t.Fatalf("indirect closure: %+v / %+v", r.summary, r.checks)
		}
		if bounded && r.checks[0].PreservingCount != 1 {
			t.Fatal("bounded target not checked")
		}
	}
	// A heuristic decoder prefix cannot stand in for all masked indices.
	image := make(romimage.Image, 0x8000)
	copy(image, []byte{0xad, 0, 0x20, 0x29, 2, 0, 0xaa, 0xfc, 0, 0x90, 0x60})
	copy(image[0x1000:], []byte{0, 0x81, 0, 0x82})
	image[0x100] = 0x60
	a, b := bankProgramFixture(t, image, 0x8000, 0, 0), bankProgramFixture(t, image, 0x8100, 0, 0)
	s := buildShadowDBSummaries(map[decoder.Variant]*shadowDBProgram{a.entry: a, b.entry: b}, nil)
	r := evaluateShadowDB(a, s, nil, nil)
	if r.summary.valid || len(r.checks) != 1 || !r.checks[0].TargetSetClosed || r.checks[0].TargetCount != 2 || r.checks[0].PreservingCount != 1 {
		t.Fatalf("missing second target ignored: %+v", r.checks)
	}
}

func TestBankQueryIgnoresBranchesThatCannotReachRead(t *testing.T) {
	image := make(romimage.Image, 0x8000)
	// BNE takes the read path. The fallthrough calls a missing routine and
	// returns; that branch cannot reach the read and is not its blocker.
	copy(image, []byte{0x4b, 0xab, 0xd0, 4, 0x20, 0, 0x81, 0x60, 0xad, 0, 0x90, 0x60})
	p := bankProgramFixture(t, image, 0x8000, 0, 0)
	q := shadowBankQuery(p, nil, nil, decoder.Variant{Address: 0x8008})
	if q.Status != "local_constant_set" || len(q.Calls) != 0 || len(q.Blockers) != 0 {
		t.Fatalf("irrelevant path inflated debt: %+v", q)
	}
	if evaluateShadowDB(p, nil, nil, nil).summary.valid {
		t.Fatal("whole-routine call requirement disappeared")
	}
}
