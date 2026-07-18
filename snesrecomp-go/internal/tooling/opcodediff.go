package tooling

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/codegen"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/ir"
	"github.com/DerrickGold/snesrecomp-go/internal/lowering"
)

type OpcodeDiffOptions struct {
	CacheDir, RuntimeSourceDir, WorkDir string
	Opcodes                             []byte
	All                                 bool
	Count, MaxShow                      int
	Mode                                string
	Keep                                bool
}

type harteState struct {
	PC, S, P, A, X, Y, DBR, D, PBR, E int
	RAM                               [][]int `json:"ram"`
}

type harteVector struct {
	Initial harteState `json:"initial"`
	Final   harteState `json:"final"`
}

type opcodeRecord struct {
	Index, Opcode, TestNo int
	Lines                 []string
	Initial, Final        harteState
}

var defaultOpcodes = []byte{
	0xA9, 0xA2, 0xA0, 0x69, 0xE9, 0x29, 0x09, 0x49, 0xC9, 0xE0, 0xC0, 0x89,
	0x0A, 0x4A, 0x2A, 0x6A, 0x1A, 0x3A, 0xAA, 0x8A, 0xA8, 0x98, 0x9B, 0xBB,
	0xBA, 0x9A, 0x5B, 0x7B, 0x1B, 0x3B, 0xE8, 0xC8, 0xCA, 0x88, 0x18, 0x38,
	0x58, 0x78, 0xD8, 0xF8, 0xB8, 0xC2, 0xE2, 0xEB, 0xEA,
}

var skippedOpcodeMnemonics = map[string]struct{}{
	"JMP": {}, "JML": {}, "JSR": {}, "JSL": {}, "RTS": {}, "RTL": {}, "RTI": {},
	"BRK": {}, "COP": {}, "STP": {}, "WAI": {}, "WDM": {}, "BRA": {}, "BRL": {},
	"BPL": {}, "BMI": {}, "BVC": {}, "BVS": {}, "BCC": {}, "BCS": {}, "BNE": {},
	"BEQ": {}, "PER": {}, "MVN": {}, "MVP": {},
}

func supportedOpcodeList() []byte {
	var result []byte
	for value := 0; value < 256; value++ {
		data := []byte{byte(value), 0, 0, 0}
		instruction, err := cpu65816.Decode(data, 0, 0x8000, 0, 1, 1)
		if err != nil || instruction == nil {
			continue
		}
		if _, skip := skippedOpcodeMnemonics[instruction.Mnemonic]; !skip {
			result = append(result, byte(value))
		}
	}
	return result
}

func emitOpcode(data []byte, pc uint16, bank, m, x byte) ([]string, error) {
	instruction, err := cpu65816.Decode(data, 0, pc, bank, m, x)
	if err != nil || instruction == nil {
		return nil, err
	}
	instruction.M, instruction.X = m&1, x&1
	nextValue := 0
	ops := lowering.Lower(instruction, func() ir.Value {
		nextValue++
		return ir.Value{ID: nextValue}
	})
	context := codegen.NewContext()
	context.CurrentSite = instruction.Address & 0xffffff
	var lines []string
	for _, operation := range ops {
		emitted, emitErr := codegen.EmitOperation(context, operation)
		if emitErr != nil {
			return nil, emitErr
		}
		lines = append(lines, emitted...)
	}
	return lines, nil
}

func loadOpcodeRecords(options OpcodeDiffOptions, op byte, startIndex int) ([]opcodeRecord, int, error) {
	suffix := "n"
	if options.Mode == "emu" {
		suffix = "e"
	}
	path := filepath.Join(options.CacheDir, fmt.Sprintf("%02x.%s.json", op, suffix))
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, 0, fmt.Errorf("read Harte vectors %s (populate tools/oracle/harte_cache first): %w", path, err)
	}
	var vectors []harteVector
	if err := json.Unmarshal(data, &vectors); err != nil {
		return nil, 0, fmt.Errorf("parse %s: %w", path, err)
	}
	var records []opcodeRecord
	skipped := 0
	for testNo, vector := range vectors {
		if len(records) >= options.Count {
			break
		}
		initial := vector.Initial
		m, x := byte(initial.P>>5)&1, byte(initial.P>>4)&1
		pc, bank := uint16(initial.PC), byte(initial.PBR)
		ram := make(map[uint32]byte, len(initial.RAM))
		for _, pair := range initial.RAM {
			if len(pair) >= 2 {
				ram[uint32(pair[0])&0xffffff] = byte(pair[1])
			}
		}
		instructionBytes := make([]byte, 8)
		for offset := 0; offset < 4; offset++ {
			address := uint32(bank)<<16 | uint32(uint16(int(pc)+offset))
			instructionBytes[offset] = ram[address]
		}
		if instructionBytes[0] != op {
			skipped++
			continue
		}
		lines, emitErr := emitOpcode(instructionBytes, pc, bank, m, x)
		if emitErr != nil {
			skipped++
			continue
		}
		records = append(records, opcodeRecord{Index: startIndex + len(records), Opcode: int(op), TestNo: testNo, Lines: lines, Initial: initial, Final: vector.Final})
	}
	return records, skipped, nil
}

const opcodeHarnessPreamble = `
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "cpu_trace.h"
int snes_frame_counter;
const char *g_last_recomp_func;
uint8_t g_ram[0x20000];
static uint8_t *MEM;
static inline uint32_t canon(uint8_t bank, uint16_t addr) {
    if (bank == 0x00 || bank == 0x7E) return 0x7E0000u | addr;
    if (bank == 0x7F) return 0x7F0000u | addr;
    return ((uint32_t)bank << 16) | addr;
}
uint8 cpu_read8(CpuState *c, uint8 b, uint16 a) { (void)c; return MEM[canon(b,a)]; }
uint16 cpu_read16(CpuState *c, uint8 b, uint16 a) { (void)c; uint32_t f=canon(b,a); return (uint16)(MEM[f] | (MEM[(f+1)&0xFFFFFF] << 8)); }
void cpu_write8(CpuState *c, uint8 b, uint16 a, uint8 v) { (void)c; MEM[canon(b,a)] = v; }
void cpu_write16(CpuState *c, uint8 b, uint16 a, uint16 v) { (void)c; uint32_t f=canon(b,a); MEM[f]=(uint8)(v&0xFF); MEM[(f+1)&0xFFFFFF]=(uint8)(v>>8); }
typedef struct { uint16_t a,x,y,s,d; uint8_t dbr,pbr,p,e; } Regs;
typedef struct { uint32_t addr; uint8_t val; } RamPair;
typedef void (*TFn)(CpuState *);
static inline uint32_t canon_pair(uint32_t flat) { return canon((uint8)((flat>>16)&0xFF), (uint16_t)flat); }
`

const opcodeHarnessMain = `
int main(void) {
    MEM = (uint8_t*)calloc(0x1000000u, 1); if (!MEM) return 2;
    int fails = 0;
    for (int i = 0; i < NTESTS; i++) {
        for (int k=0;k<IRAM_CNT[i];k++) MEM[canon_pair(IRAM[IRAM_OFF[i]+k].addr)]=0;
        for (int k=0;k<FRAM_CNT[i];k++) MEM[canon_pair(FRAM[FRAM_OFF[i]+k].addr)]=0;
        for (int k=0;k<IRAM_CNT[i];k++) MEM[canon_pair(IRAM[IRAM_OFF[i]+k].addr)]=IRAM[IRAM_OFF[i]+k].val;
        CpuState cpu; memset(&cpu,0,sizeof cpu); Regs in=INIT[i];
        cpu.A=in.a; cpu.X=in.x; cpu.Y=in.y; cpu.S=in.s; cpu.D=in.d; cpu.DB=in.dbr; cpu.PB=in.pbr; cpu.P=in.p; cpu.emulation=in.e; cpu_p_to_mirrors(&cpu);
        FNS[i](&cpu); cpu_mirrors_to_p(&cpu); Regs ex=FIN[i]; int bad=0; char buf[512]; int n=0;
#define CK(field,got,exp) do { if ((unsigned)(got)!=(unsigned)(exp)) { n+=snprintf(buf+n,sizeof(buf)-n," %s=%X(exp %X)",field,(unsigned)(got),(unsigned)(exp)); bad=1; } } while(0)
        CK("A",cpu.A,ex.a); CK("X",cpu.X,ex.x); CK("Y",cpu.Y,ex.y); CK("S",cpu.S,ex.s); CK("D",cpu.D,ex.d); CK("DB",cpu.DB,ex.dbr); CK("P",cpu.P,ex.p); CK("E",cpu.emulation,ex.e);
        for (int k=0;k<FRAM_CNT[i];k++) { uint32_t a=FRAM[FRAM_OFF[i]+k].addr; uint8_t want=FRAM[FRAM_OFF[i]+k].val, got=MEM[canon_pair(a)]; if(got!=want){n+=snprintf(buf+n,sizeof(buf)-n," m[%06X]=%02X(exp %02X)",a,got,want);bad=1;} }
        if (bad) { fails++; if (fails<=MAXSHOW) printf("FAIL op=%02X test=%d:%s\n",OPCODE[i],TESTNO[i],buf); }
    }
    printf("RESULT %d/%d passed (%d failed)\n",NTESTS-fails,NTESTS,fails); return fails?1:0;
}
`

func cHex(value int) string { return fmt.Sprintf("0x%X", value) }

func appendIntArray(builder *strings.Builder, name string, values []int) {
	fmt.Fprintf(builder, "static int %s[] = {", name)
	for index, value := range values {
		if index > 0 {
			builder.WriteByte(',')
		}
		fmt.Fprint(builder, value)
	}
	builder.WriteString("};\n")
}

func buildOpcodeHarness(records []opcodeRecord, maxShow int) string {
	var out strings.Builder
	out.WriteString(opcodeHarnessPreamble)
	for _, record := range records {
		fmt.Fprintf(&out, "static void t_%d(CpuState *cpu) {\n", record.Index)
		for _, line := range record.Lines {
			fmt.Fprintf(&out, "  %s\n", line)
		}
		out.WriteString("}\n")
	}
	fmt.Fprintf(&out, "#define NTESTS %d\n#define MAXSHOW %d\n", len(records), maxShow)
	out.WriteString("static TFn FNS[NTESTS] = {")
	for index, record := range records {
		if index > 0 {
			out.WriteByte(',')
		}
		fmt.Fprintf(&out, "t_%d", record.Index)
	}
	out.WriteString("};\n")
	opcodes, testNos := make([]int, len(records)), make([]int, len(records))
	for index, record := range records {
		opcodes[index], testNos[index] = record.Opcode, record.TestNo
	}
	appendIntArray(&out, "OPCODE", opcodes)
	appendIntArray(&out, "TESTNO", testNos)
	writeRegs := func(name string, final bool) {
		fmt.Fprintf(&out, "static Regs %s[NTESTS] = {", name)
		for index, record := range records {
			if index > 0 {
				out.WriteByte(',')
			}
			state := record.Initial
			if final {
				state = record.Final
			}
			fmt.Fprintf(&out, "{%s,%s,%s,%s,%s,%s,%s,%s,%s}", cHex(state.A), cHex(state.X), cHex(state.Y), cHex(state.S), cHex(state.D), cHex(state.DBR), cHex(state.PBR), cHex(state.P), cHex(state.E))
		}
		out.WriteString("};\n")
	}
	writeRegs("INIT", false)
	writeRegs("FIN", true)
	writeRAM := func(prefix string, final bool) {
		var pool [][2]int
		offsets, counts := make([]int, 0, len(records)), make([]int, 0, len(records))
		for _, record := range records {
			state := record.Initial
			if final {
				state = record.Final
			}
			offsets = append(offsets, len(pool))
			counts = append(counts, len(state.RAM))
			for _, pair := range state.RAM {
				if len(pair) >= 2 {
					pool = append(pool, [2]int{pair[0] & 0xffffff, pair[1] & 0xff})
				}
			}
		}
		fmt.Fprintf(&out, "static const RamPair %s[] = {", prefix)
		if len(pool) == 0 {
			out.WriteString("{0,0}")
		}
		for index, pair := range pool {
			if index > 0 {
				out.WriteByte(',')
			}
			fmt.Fprintf(&out, "{0x%X,0x%X}", pair[0], pair[1])
		}
		out.WriteString("};\n")
		appendIntArray(&out, prefix+"_OFF", offsets)
		appendIntArray(&out, prefix+"_CNT", counts)
	}
	writeRAM("IRAM", false)
	writeRAM("FRAM", true)
	out.WriteString(opcodeHarnessMain)
	return out.String()
}

func RunOpcodeDiff(options OpcodeDiffOptions) error {
	if options.Count <= 0 {
		options.Count = 64
	}
	if options.MaxShow <= 0 {
		options.MaxShow = 12
	}
	if options.Mode == "" {
		options.Mode = "native"
	}
	if options.Mode != "native" && options.Mode != "emu" {
		return fmt.Errorf("mode must be native or emu")
	}
	opcodes := append([]byte(nil), options.Opcodes...)
	if options.All {
		opcodes = supportedOpcodeList()
	} else if len(opcodes) == 0 {
		opcodes = append(opcodes, defaultOpcodes...)
	}
	var records []opcodeRecord
	skips := make(map[byte]int)
	for _, op := range opcodes {
		part, skipped, err := loadOpcodeRecords(options, op, len(records))
		if err != nil {
			return err
		}
		records = append(records, part...)
		if skipped > 0 {
			skips[op] = skipped
		}
	}
	if len(records) == 0 {
		return fmt.Errorf("no opcode records gathered")
	}
	if err := os.MkdirAll(options.WorkDir, 0755); err != nil {
		return err
	}
	cPath, binaryPath := filepath.Join(options.WorkDir, "harness.c"), filepath.Join(options.WorkDir, "harness")
	if err := os.WriteFile(cPath, []byte(buildOpcodeHarness(records, len(records))), 0644); err != nil {
		return err
	}
	compile := exec.Command("cc", "-O1", "-std=gnu11", "-I"+options.RuntimeSourceDir, cPath, "-o", binaryPath)
	if output, err := compile.CombinedOutput(); err != nil {
		return fmt.Errorf("opcode harness compile failed: %w\n%s\n(generated C kept at %s)", err, string(output), cPath)
	}
	run := exec.Command(binaryPath)
	var stdout, stderr bytes.Buffer
	run.Stdout = &stdout
	run.Stderr = &stderr
	runErr := run.Run()
	if stderr.Len() > 0 {
		fmt.Fprint(os.Stderr, stderr.String())
	}
	failRE := regexp.MustCompile(`^FAIL op=([0-9A-Fa-f]+) `)
	failures := make(map[int]int)
	samples := make(map[int]string)
	resultLine := ""
	for _, line := range strings.Split(stdout.String(), "\n") {
		if match := failRE.FindStringSubmatch(line); match != nil {
			value, _ := strconv.ParseInt(match[1], 16, 16)
			failures[int(value)]++
			if samples[int(value)] == "" {
				samples[int(value)] = line
			}
		}
		if strings.HasPrefix(line, "RESULT") {
			resultLine = line
		}
	}
	if len(failures) == 0 {
		fmt.Printf("All %d tested opcodes clean.\n", len(opcodes))
	} else {
		keys := make([]int, 0, len(failures))
		for key := range failures {
			keys = append(keys, key)
		}
		sort.Slice(keys, func(i, j int) bool { return failures[keys[i]] > failures[keys[j]] })
		fmt.Printf("=== FAILING OPCODES (%d of %d tested) ===\n", len(keys), len(opcodes))
		for _, key := range keys {
			fmt.Printf("  op=%02X %d fail(s)  e.g. %s\n", key, failures[key], strings.TrimPrefix(samples[key], "FAIL "))
		}
	}
	if resultLine != "" {
		fmt.Println(resultLine)
	}
	if len(skips) > 0 {
		fmt.Printf("skipped (decode/opcode mismatch): %v\n", skips)
	}
	if !options.Keep {
		_ = os.Remove(cPath)
		_ = os.Remove(binaryPath)
	}
	return runErr
}
