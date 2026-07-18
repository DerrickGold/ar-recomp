package emitter

import (
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

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
