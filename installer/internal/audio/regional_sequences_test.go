package audio

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/regionalmedia"
)

// This uses the existing production preview SPC/DSP core. It compares the US
// driver plus the extracted sequence against the complete Japanese song upload,
// including multiple loops. It is not a subjective listening test.
func TestRegionalSequenceAudio(t *testing.T) {
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		t.Skip("optional regional sequence audio render")
	}
	read := func(name string) []byte {
		t.Helper()
		data, err := os.ReadFile(filepath.Join(root, name))
		if err != nil {
			t.Fatal(err)
		}
		return data
	}
	us, jp := read("ar.sfc"), read("ar-jp.sfc")
	extraction, err := regionalmedia.Extract(jp)
	if err != nil {
		t.Fatal(err)
	}
	base, next, err := prepareBaseAPU(us)
	if err != nil {
		t.Fatal(err)
	}
	// Locate the native Japanese driver independently: a sequence comparison
	// must not assume the complete firmware is byte-identical.
	const jpBoot = 0x1183e // Native02:96A7 selects02:983E.
	jpSize := int(binary.LittleEndian.Uint16(jp[jpBoot:]))
	t.Logf("JP boot file%x payload%d; US payload%d", jpBoot, jpSize, binary.LittleEndian.Uint16(us[0x11acd:]))
	jpBase := newAPU()
	boot, err := applyImage(jpBase, jp, 0x02983e, false, 0)
	if err != nil || boot.finalWord != 0x400 {
		t.Fatal("Japanese boot shape", err)
	}
	idle := bytes.Index(jp[jpBoot+4:jpBoot+4+jpSize], []byte{0xec, 0xfd, 0x00, 0xf0, 0xfb})
	if idle < 0 {
		t.Fatal("Japanese boot idle not found")
	}
	jpBase.cpu.pc = boot.finalWord
	var discard []int16
	for elapsed := 0; elapsed < bootCycleLimit && jpBase.cpu.pc != uint16(0x400+idle) && jpBase.cpu.pc != uint16(0x403+idle); {
		before := jpBase.cpu.cycles
		if err := jpBase.step(&discard); err != nil {
			t.Fatal(err)
		}
		elapsed += int(jpBase.cpu.cycles - before)
		discard = discard[:0]
	}
	if jpBase.cpu.pc != uint16(0x400+idle) && jpBase.cpu.pc != uint16(0x403+idle) {
		t.Fatal("Japanese boot timeout")
	}
	common, err := applyImage(jpBase, jp, commonImageSource, true, 0x3000)
	if err != nil || common.nextBRR != next {
		t.Fatal("Japanese common sample contract", err)
	}
	for i, source := range []uint32{0x0ef69f, 0x19fa4b} {
		native, projected, western, japanese := base.clone(), base.clone(), base.clone(), jpBase.clone()
		if _, err := applyImage(native, jp, []uint32{0x188000, 0x1ab1e1}[i], true, next); err != nil {
			t.Fatal(err)
		}
		if _, err := applyImage(japanese, jp, []uint32{0x188000, 0x1ab1e1}[i], true, next); err != nil {
			t.Fatal(err)
		}
		for _, a := range []*apu{projected, western} {
			if _, err := applyImage(a, us, source, true, next); err != nil {
				t.Fatal(err)
			}
		}
		found := false
		for _, resource := range extraction.Resources {
			if resource.ID == regionalmedia.Sequence09+uint32(i) {
				copy(projected.ram[0x1200:], resource.Bytes)
				found = true
			}
		}
		if !found {
			t.Fatal("missing extracted sequence")
		}
		native.inPorts[0], projected.inPorts[0], western.inPorts[0] = 1, 1, 1
		japanese.inPorts[0] = 1
		jpHash, usHash := sha256.New(), sha256.New()
		difference, audible, driverDifference := false, false, false
		for chunk := 0; chunk < 180; chunk++ {
			original, err := native.renderFrames(SampleRate)
			if err != nil {
				t.Fatal(err)
			}
			candidate, err := projected.renderFrames(SampleRate)
			if err != nil {
				t.Fatal(err)
			}
			control, err := western.renderFrames(SampleRate)
			if err != nil {
				t.Fatal(err)
			}
			jpControl, err := japanese.renderFrames(SampleRate)
			if err != nil {
				t.Fatal(err)
			}
			for sample := range original {
				if original[sample] != candidate[sample] {
					t.Fatalf("song%d second%d sample%d differs from Japanese image", []int{9, 12}[i], chunk, sample)
				}
				difference = difference || original[sample] != control[sample]
				audible = audible || original[sample] != 0
				driverDifference = driverDifference || original[sample] != jpControl[sample]
			}
			if err := binary.Write(jpHash, binary.LittleEndian, original); err != nil {
				t.Fatal(err)
			}
			if err := binary.Write(usHash, binary.LittleEndian, control); err != nil {
				t.Fatal(err)
			}
		}
		if !audible {
			t.Fatal("silent song")
		}
		t.Logf("song%d:180 seconds exact Japanese-image/US-driver PCM; differs from US sequence=%t; JP driver difference=%t; projected=%x US=%x", []int{9, 12}[i], difference, driverDifference, jpHash.Sum(nil), usHash.Sum(nil))
	}
}
