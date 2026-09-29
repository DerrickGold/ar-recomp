package regen

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
)

func TestSharingRequiresIdenticalDecodedBehavior(t *testing.T) {
	root := decoder.DecodeKey{PC: 0x8000, M: 1, X: 1}
	start := decoder.DecodeKey{PC: 0x8010, M: 1, X: 1}
	end := decoder.DecodeKey{PC: 0x8012, M: 1, X: 1}
	exit := decoder.DecodeKey{PC: 0x8100, M: 1, X: 1}
	graph := func(owner bool) *decoder.Graph {
		g := &decoder.Graph{Entry: start, Instructions: map[decoder.DecodeKey]*decoder.DecodedInstruction{
			start: {Key: start, Instruction: &cpu65816.Instruction{Address: start.PC, Opcode: 0x80, Mnemonic: "BRA", Length: 2, M: 1, X: 1}, Successors: []decoder.DecodeKey{end}},
			end:   {Key: end, Instruction: &cpu65816.Instruction{Address: end.PC, Opcode: 0x60, Mnemonic: "RTS", Length: 1, M: 1, X: 1}, Successors: []decoder.DecodeKey{exit}},
		}, BoundaryExits: []decoder.DecodeKey{exit}}
		if owner {
			g.Entry = root
			g.Instructions[root] = &decoder.DecodedInstruction{Key: root, Instruction: &cpu65816.Instruction{Opcode: 0x80}, Successors: []decoder.DecodeKey{start}}
		}
		return g
	}
	if !sameDecodeClosure(graph(true), graph(false), start) {
		t.Fatal("equal independently allocated closures rejected")
	}
	for name, mutate := range map[string]func(*decoder.Graph){
		"operand":             func(g *decoder.Graph) { g.Instructions[start].Instruction.Operand = 17 },
		"dispatch target":     func(g *decoder.Graph) { g.Instructions[start].Instruction.DispatchEntries = []uint32{0x8123} },
		"open dispatch":       func(g *decoder.Graph) { g.Instructions[start].Instruction.DispatchOpen = true },
		"successor":           func(g *decoder.Graph) { g.Instructions[end].Successors = []decoder.DecodeKey{start} },
		"boundary":            func(g *decoder.Graph) { g.BoundaryExits = nil },
		"missing instruction": func(g *decoder.Graph) { g.Instructions[start].Instruction = nil },
		"missing entry":       func(g *decoder.Graph) { delete(g.Instructions, start) },
	} {
		t.Run(name, func(t *testing.T) {
			candidate := graph(false)
			mutate(candidate)
			if sameDecodeClosure(graph(true), candidate, start) {
				t.Fatal("unsafe sharing accepted")
			}
		})
	}
}

func TestSharingHonorsBoundedConditionalHLEOwner(t *testing.T) {
	root := t.TempDir()
	romPath, cfgDir, out := filepath.Join(root, "game.sfc"), filepath.Join(root, "cfg"), filepath.Join(root, "gen")
	image := make([]byte, 0x8000)
	copy(image, []byte{0x64, 0x10, 0x60}) // bounded STZ prefix; separate RTS continuation
	if err := os.WriteFile(romPath, image, 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(cfgDir, 0700); err != nil {
		t.Fatal(err)
	}
	cfg := "bank = 00\nfunc Root 8000 end:8002 entry_mx:0,0\nhle_func_if 8000 Hook UseHook\nfunc bank_00_8002 8002 entry_mx:0,0\n"
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(cfg), 0600); err != nil {
		t.Fatal(err)
	}
	shadow, err := tooling.AnalyzeAuthoredShadow(tooling.ShadowAnalysisOptions{ROMPath: romPath, CFGDir: cfgDir, Jobs: 1})
	if err != nil {
		t.Fatal(err)
	}
	facts := tooling.SelectStaticProvenContinuationEntryFacts(shadow)
	if len(facts) != 1 || facts[0].PC != 0x8002 {
		t.Fatalf("fixture must prove the bounded continuation: %+v", facts)
	}
	report, err := Run(Options{ROMPath: romPath, ConfigDir: cfgDir, OutputDir: out, Jobs: 1, AllowStubs: true, ProvenEntryFacts: facts})
	if err != nil {
		t.Fatal(err)
	}
	if report.SharedRegionBodies != 0 || report.SharedRegionContinuationFallbacks != 1 {
		t.Fatalf("a bounded prefix cannot absorb the next body: %+v", report)
	}
	source, err := os.ReadFile(filepath.Join(out, "bank00_v2.c"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(source), "if (UseHook(cpu))") || !strings.Contains(string(source), "Hook(cpu)") || strings.Contains(string(source), "sr_region_") {
		t.Fatalf("conditional hook/boundary lost: %s", source)
	}
}
