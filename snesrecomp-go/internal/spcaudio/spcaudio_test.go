package spcaudio

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func runProgram(t *testing.T, program ...byte) *apu {
	t.Helper()
	a := newAPU()
	copy(a.ram[0x0200:], program)
	a.cpu.pc = 0x0200
	var samples []int16
	for instructions := 0; !a.cpu.stopped && instructions < 100; instructions++ {
		if err := a.step(&samples); err != nil {
			t.Fatal(err)
		}
	}
	if !a.cpu.stopped {
		t.Fatal("synthetic SPC700 program did not stop")
	}
	return a
}

func TestSPC700ArithmeticMovesAndMultiply(t *testing.T) {
	a := runProgram(t,
		0xe8, 0x7f, // MOV A,#$7f
		0x60,       // CLRC
		0x88, 0x01, // ADC A,#$01
		0xc4, 0x10, // MOV $10,A
		0xcd, 0x02, // MOV X,#2
		0x8d, 0x03, // MOV Y,#3
		0xe8, 0x02, // MOV A,#2
		0xcf,       // MUL YA
		0xda, 0x20, // MOVW $20,YA
		0xff,
	)
	if a.ram[0x10] != 0x80 {
		t.Fatalf("ADC result = %#02x, want 0x80", a.ram[0x10])
	}
	if a.cpu.psw&flagV == 0 {
		t.Fatal("ADC did not set overflow")
	}
	if a.ram[0x20] != 6 || a.ram[0x21] != 0 {
		t.Fatalf("MUL/MOVW result = %02x%02x, want 0006", a.ram[0x21], a.ram[0x20])
	}
}

func TestSPC700CallBranchAndStack(t *testing.T) {
	a := runProgram(t,
		0x3f, 0x08, 0x02, // CALL $0208
		0xc4, 0x30, // MOV $30,A
		0xff,       // STOP
		0x00, 0x00, // padding
		0xe8, 0x42, // $0208: MOV A,#$42
		0x6f, // RET
	)
	if a.ram[0x30] != 0x42 {
		t.Fatalf("subroutine result = %#02x, want 0x42", a.ram[0x30])
	}
	if a.cpu.sp != 0xef {
		t.Fatalf("stack pointer = %#02x, want 0xef", a.cpu.sp)
	}
}

func TestAPUTimerTwo(t *testing.T) {
	a := newAPU()
	a.write(0xfc, 2)
	a.write(0xf1, 0x04)
	for index := 0; index < 16*2*3; index++ {
		a.tickTimer(2)
	}
	if got := a.read(0xff); got != 3 {
		t.Fatalf("timer 2 counter = %d, want 3", got)
	}
	if got := a.read(0xff); got != 0 {
		t.Fatalf("timer counter did not clear on read: %d", got)
	}
}

func TestDSPDecodesBRRVoice(t *testing.T) {
	a := newAPU()
	// Directory page $20, source 0 -> a single looping positive BRR block.
	a.ram[0x2000], a.ram[0x2001] = 0x00, 0x30
	a.ram[0x2002], a.ram[0x2003] = 0x00, 0x30
	a.ram[0x3000] = 0x13 // range 1, end + loop
	for index := 1; index < 9; index++ {
		a.ram[0x3000+index] = 0x77
	}
	a.dsp.write(0x5d, 0x20)
	a.dsp.write(0x00, 0x7f)
	a.dsp.write(0x01, 0x7f)
	a.dsp.write(0x02, 0x00)
	a.dsp.write(0x03, 0x10) // pitch 1.0
	a.dsp.write(0x04, 0x00)
	a.dsp.write(0x05, 0x00)
	a.dsp.write(0x07, 0x7f) // direct gain
	a.dsp.write(0x0c, 0x7f)
	a.dsp.write(0x1c, 0x7f)
	a.dsp.write(0x6c, 0x20) // echo writes off, output unmuted
	a.dsp.write(0x4c, 0x01)
	nonzero := false
	for index := 0; index < 32; index++ {
		left, right := a.dsp.sample(&a.ram)
		if left != 0 || right != 0 {
			nonzero = true
		}
	}
	if !nonzero {
		t.Fatal("BRR voice produced only silence")
	}
}

func TestApplyImageLoadsBlocksAndBRRScript(t *testing.T) {
	rom := make([]byte, 0x40020)
	// Image at $00:8000 (offset zero), one data block and a two-index stage 2.
	copy(rom, []byte{
		3, 0, 0x00, 0x20, 1, 2, 3,
		0, 0, 2, 0, 1,
	})
	// Pool at $08:8000: chunk 0 = AA BB, chunk 1 = CC.
	copy(rom[0x40000:], []byte{2, 0, 0xaa, 0xbb, 1, 0, 0xcc})
	a := newAPU()
	result, err := applyImage(a, rom, 0x008000, true, 0x3000)
	if err != nil {
		t.Fatal(err)
	}
	if got := a.ram[0x2000:0x2003]; !bytes.Equal(got, []byte{1, 2, 3}) {
		t.Fatalf("block = %v", got)
	}
	if got := a.ram[0x3000:0x3003]; !bytes.Equal(got, []byte{0xaa, 0xbb, 0xcc}) {
		t.Fatalf("BRR stream = %x", got)
	}
	if result.nextBRR != 0x3003 {
		t.Fatalf("next BRR = %#04x", result.nextBRR)
	}
}

func TestWAVHeader(t *testing.T) {
	var output bytes.Buffer
	if err := writeWAV(&output, []int16{1, -1, 2, -2}); err != nil {
		t.Fatal(err)
	}
	content := output.Bytes()
	if string(content[:4]) != "RIFF" || string(content[8:12]) != "WAVE" ||
		string(content[36:40]) != "data" {
		t.Fatalf("bad WAV header: %q", content[:44])
	}
	if got := binary.LittleEndian.Uint32(content[24:28]); got != SampleRate {
		t.Fatalf("sample rate = %d", got)
	}
	if got := binary.LittleEndian.Uint32(content[40:44]); got != 8 {
		t.Fatalf("data size = %d", got)
	}
}

func TestActRaiserROMValidationRejectsWrongImage(t *testing.T) {
	_, err := normalizeActRaiserROM(make([]byte, actRaiserROMSize))
	if err == nil || !strings.Contains(err.Error(), "CRC32") {
		t.Fatalf("unexpected error: %v", err)
	}
}

// A cache entry is reused verbatim, so accepting one that is merely "a RIFF
// file" means a truncated write, a leftover from a different preview length, or
// a WAV an older renderer produced is served as this renderer's output forever.
func TestValidWAVFileRejectsAnythingButACompletePreview(t *testing.T) {
	directory := t.TempDir()
	const frames = 64
	write := func(name string, content []byte) string {
		path := filepath.Join(directory, name)
		if err := os.WriteFile(path, content, 0o600); err != nil {
			t.Fatal(err)
		}
		return path
	}
	var complete bytes.Buffer
	if err := writeWAV(&complete, make([]int16, frames*2)); err != nil {
		t.Fatal(err)
	}
	good := write("good.wav", complete.Bytes())
	if !validWAVFile(good, frames) {
		t.Error("a complete preview was rejected")
	}
	if validWAVFile(good, frames*2) {
		t.Error("a preview of the wrong duration was accepted")
	}
	body := complete.Bytes()
	for name, content := range map[string][]byte{
		"truncated.wav": body[:len(body)-8],
		"padded.wav":    append(append([]byte{}, body...), 0, 0, 0, 0),
		"prefix.wav":    []byte("RIFF\x00\x00\x00\x00WAVEpreview"),
	} {
		if path := write(name, content); validWAVFile(path, frames) {
			t.Errorf("%s was accepted as a complete preview", name)
		}
	}
	if validWAVFile(filepath.Join(directory, "absent.wav"), frames) {
		t.Error("a missing cache entry was reported valid")
	}
}

// Bumping the renderer generation stops old audio being served, but on its own
// it strands the previous generation's WAVs on disk with nothing to reclaim
// them. Only OLDER generations go: a current-generation directory is live
// whatever its duration, and an unrecognised name is left alone.
func TestPruneSupersededPreviewsKeepsOnlyTheCurrentGeneration(t *testing.T) {
	romDirectory := t.TempDir()
	current := previewCacheVariant(30 * time.Second)
	names := []string{current, "v1-30000ms", "v1-5000ms", "v99-30000ms", "notes"}
	for _, name := range names {
		if err := os.MkdirAll(filepath.Join(romDirectory, name), 0o700); err != nil {
			t.Fatal(err)
		}
	}
	pruneSupersededPreviews(romDirectory)
	survives := map[string]bool{current: true, "v99-30000ms": true, "notes": true}
	for _, name := range names {
		_, err := os.Stat(filepath.Join(romDirectory, name))
		if survives[name] && err != nil {
			t.Errorf("%s should have been kept: %v", name, err)
		}
		if !survives[name] && err == nil {
			t.Errorf("%s should have been pruned", name)
		}
	}
}
