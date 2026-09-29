package emitter

import (
	"fmt"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestBankDefaultContextRetainsAllFunctionDemands(t *testing.T) {
	image := make(rom.Image, 0x10000)
	copy(image, []byte{0x22, 0x00, 0x80, 0x01, 0x60})        // JSL $01:8000; RTS
	copy(image[0x10:], []byte{0x22, 0x10, 0x80, 0x01, 0x60}) // JSL $01:8010; RTS
	entries := []config.Entry{
		{Name: "First", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}},
		{Name: "Second", Start: 0x8010, EntryMX: config.MX{M: 1, X: 1}},
	}
	implicit, err := EmitBank(image, 0, entries, BankOptions{})
	if err != nil {
		t.Fatal(err)
	}
	explicit, err := EmitBank(image, 0, entries, BankOptions{Context: codegen.NewContext()})
	if err != nil {
		t.Fatal(err)
	}
	if implicit != explicit {
		t.Error("default context must emit the same declarations and bodies as an explicit empty context")
	}
	for _, pc := range []uint16{0x8000, 0x8010} {
		for m := 0; m < 2; m++ {
			for x := 0; x < 2; x++ {
				declaration := fmt.Sprintf("RecompReturn bank_01_%04X_M%dX%d(CpuState *cpu);", pc, m, x)
				if !strings.Contains(implicit, declaration) {
					t.Errorf("default context lost demanded declaration %s", declaration)
				}
			}
		}
	}
}

func TestBankOrderForwardDeclarationsAndAlias(t *testing.T) {
	image := make(rom.Image, 0x8000)
	copy(image, []byte{0x60, 0xEA, 0x60})
	entries := []config.Entry{
		{Name: "First", Start: 0x8000, EntryMX: config.MX{M: 1, X: 1}},
		{Name: "Second", Start: 0x8002, EntryMX: config.MX{M: 1, X: 1}},
	}
	source, err := EmitBank(image, 0, entries, BankOptions{Context: codegen.NewContext()})
	if err != nil {
		t.Fatal(err)
	}
	firstDecl := strings.Index(source, "RecompReturn First_M1X1(CpuState *cpu);")
	secondDecl := strings.Index(source, "RecompReturn Second_M1X1(CpuState *cpu);")
	firstBody := strings.Index(source, "RecompReturn First_M1X1(CpuState *cpu) {")
	if firstDecl < 0 || secondDecl <= firstDecl || firstBody <= secondDecl {
		t.Fatalf("forward declaration/body order is wrong:\n%s", source)
	}
	if !strings.Contains(source, "void First(CpuState *cpu)") || !strings.Contains(source, "First_M1X1(cpu)") {
		t.Errorf("named alias is missing:\n%s", source)
	}
}
