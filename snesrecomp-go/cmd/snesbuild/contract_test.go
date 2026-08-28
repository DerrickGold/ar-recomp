package main

import (
	"bytes"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestCollectHleObligationsUsesCfgGrammar(t *testing.T) {
	directory := t.TempDir()
	path := filepath.Join(directory, "bank01.cfg")
	contents := "bank = 01\n" +
		"func Entry 8000\n" +
		"hle_func 8000 HostFrame\n" +
		"hle_func_if 8010 HostConditional HostConditionalEnabled\n" +
		"hle_dispatch 8020 HostDispatch\n"
	if err := os.WriteFile(path, []byte(contents), 0o600); err != nil {
		t.Fatal(err)
	}
	got, err := collectHleObligations(directory)
	if err != nil {
		t.Fatal(err)
	}
	var symbols []string
	for _, obligation := range got {
		symbols = append(symbols, obligation.symbol)
	}
	want := []string{
		"HostConditional", "HostConditionalEnabled", "HostDispatch", "HostFrame",
	}
	if !reflect.DeepEqual(symbols, want) {
		t.Fatalf("symbols = %#v, want %#v", symbols, want)
	}
}

func TestCollectRequiredRunnerSymbolsReadsMarkedHeader(t *testing.T) {
	path := filepath.Join(t.TempDir(), "required_symbols.h")
	contents := `
#define SR_GAME_PROVIDES
SR_GAME_PROVIDES void Lock(void);
/* SR_GAME_PROVIDES void CommentedOut(void); */
SR_GAME_PROVIDES
void Unlock(
    void);
`
	if err := os.WriteFile(path, []byte(contents), 0o600); err != nil {
		t.Fatal(err)
	}
	got, err := collectRequiredRunnerSymbols(path)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"Lock", "Unlock"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("required symbols = %#v, want %#v", got, want)
	}
}

func TestSymbolEvidenceHandlesRealDefinitionsWithoutFalseFailures(t *testing.T) {
	source := sanitizeCSource(`
RecompReturn MultiLine(
    CpuState *cpu)
{
    return RECOMP_RETURN_NORMAL;
}

RecompReturn Attributed(CpuState *cpu)
    __attribute__((unused))
{
    return RECOMP_RETURN_NORMAL;
}

extern RecompReturn PrototypeOnly(CpuState *cpu);
void Caller(void) { (void)CallOnly(0); }
#define PreprocessorOnly(cpu) fake_definition(cpu) {
/* CommentOnly(CpuState *cpu) { } */
`)
	for _, symbol := range []string{"MultiLine", "Attributed"} {
		if got := symbolEvidenceInSource(source, symbol); got != symbolDefined {
			t.Errorf("%s evidence = %v, want defined", symbol, got)
		}
	}
	for _, symbol := range []string{"PrototypeOnly", "CallOnly"} {
		if got := symbolEvidenceInSource(source, symbol); got != symbolMentioned {
			t.Errorf("%s evidence = %v, want mentioned", symbol, got)
		}
	}
	for _, symbol := range []string{"PreprocessorOnly", "CommentOnly", "Missing"} {
		if got := symbolEvidenceInSource(source, symbol); got != symbolAbsent {
			t.Errorf("%s evidence = %v, want absent", symbol, got)
		}
	}
}

func TestReportGameContractUsesManifestSourcesAndNamesMissingCfg(t *testing.T) {
	root := t.TempDir()
	cfgDir := filepath.Join(root, "recomp")
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(
		"bank = 00\nfunc Entry 8000\nhle_func 8000 MissingHle\n"),
		0o600); err != nil {
		t.Fatal(err)
	}
	required := filepath.Join(root, "required_symbols.h")
	if err := os.WriteFile(required, []byte(
		"#define SR_GAME_PROVIDES\n"+
			"SR_GAME_PROVIDES void RtlApuLock(void);\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "game.c"), []byte(
		"void RtlApuLock(void) {}\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	// A definition outside the manifest must not satisfy the build contract.
	if err := os.WriteFile(filepath.Join(root, "not_built.c"), []byte(
		"void MissingHle(void *cpu) { (void)cpu; }\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	generatedDir := filepath.Join(root, "src", "gen")
	if err := os.MkdirAll(generatedDir, 0o755); err != nil {
		t.Fatal(err)
	}
	// Generated sources are build inputs, but they cannot fulfill an authored
	// HLE obligation even when the manifest lists them explicitly.
	if err := os.WriteFile(filepath.Join(generatedDir, "generated.c"), []byte(
		"void MissingHle(void *cpu) { (void)cpu; }\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	missing, err := reportGameContract(&output, root, cfgDir, generatedDir,
		required, []string{"game.c", "src/gen/generated.c"})
	if err != nil {
		t.Fatal(err)
	}
	if !missing {
		t.Fatalf("missing HLE did not fail contract:\n%s", output.String())
	}
	for _, fragment := range []string{"MissingHle", "hle_func", "bank00.cfg"} {
		if !strings.Contains(output.String(), fragment) {
			t.Errorf("contract output is missing %q:\n%s", fragment, output.String())
		}
	}
}

func TestReportGameContractDoesNotFailUnverifiedMacroDefinition(t *testing.T) {
	root := t.TempDir()
	cfgDir := filepath.Join(root, "recomp")
	if err := os.MkdirAll(cfgDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(cfgDir, "bank00.cfg"), []byte(
		"bank = 00\nfunc Entry 8000\nhle_func 8000 MacroHle\n"),
		0o600); err != nil {
		t.Fatal(err)
	}
	required := filepath.Join(root, "required_symbols.h")
	if err := os.WriteFile(required, []byte(
		"#define SR_GAME_PROVIDES\n"+
			"SR_GAME_PROVIDES void RequiredLock(void);\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, "game.c"), []byte(
		"DEFINE_HLE(MacroHle)\nvoid RequiredLock(void) {}\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	var output bytes.Buffer
	missing, err := reportGameContract(&output, root, cfgDir, "", required,
		[]string{"game.c"})
	if err != nil {
		t.Fatal(err)
	}
	if missing || !strings.Contains(output.String(), "WARNING") {
		t.Fatalf("macro-authored symbol should warn without failing:\n%s",
			output.String())
	}
}
