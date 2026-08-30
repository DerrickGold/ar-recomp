package tooling

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeAPUAuditFixture(t *testing.T, directory string, aram, dsp, written []byte, trace []apuTraceLine) string {
	t.Helper()
	prefix := filepath.Join(directory, "capture")
	if err := os.WriteFile(prefix+".aram", aram, 0o600); err != nil {
		t.Fatal(err)
	}
	if dsp != nil {
		if err := os.WriteFile(prefix+".dsp", dsp, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	if written != nil {
		if err := os.WriteFile(prefix+".written", written, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	if trace != nil {
		file, err := os.Create(prefix + ".audio.jsonl")
		if err != nil {
			t.Fatal(err)
		}
		encoder := json.NewEncoder(file)
		for _, line := range trace {
			if err := encoder.Encode(line); err != nil {
				file.Close()
				t.Fatal(err)
			}
		}
		if err := file.Close(); err != nil {
			t.Fatal(err)
		}
	}
	return prefix
}

func markAPUAuditWritten(bitmap []byte, start, count int) {
	for address := start; address < start+count; address++ {
		bitmap[address>>3] |= 1 << (address & 7)
	}
}

func TestPopAPUQueuedWriteMatchesAppliedValue(t *testing.T) {
	matched, remaining := popAPUQueuedWrite([]apuQueuedWrite{
		{value: 0x10, sourceBlock: 0x1111, function: "superseded"},
		{value: 0xff, sourceBlock: 0xd583, function: "upload"},
	}, 0xff)
	if matched.sourceBlock != 0xd583 || matched.function != "upload" || len(remaining) != 0 {
		t.Fatalf("applied write was paired by FIFO rather than value: matched=%+v remaining=%+v", matched, remaining)
	}
}

func TestAPUAuditValidLoopingActiveSample(t *testing.T) {
	aram := make([]byte, apuRAMBytes)
	dsp := make([]byte, dspRegisterBytes)
	written := make([]byte, apuWrittenBitmapBytes)
	directory := 0x2000 + 3*4
	aram[directory], aram[directory+1] = 0x00, 0x30
	aram[directory+2], aram[directory+3] = 0x00, 0x30
	aram[directory+4], aram[directory+5] = 0x12, 0x30
	aram[0x3000] = 0x00
	aram[0x3009] = 0x03
	markAPUAuditWritten(written, 0x3000, 18)
	dsp[0x5d] = 0x20
	dsp[0x00], dsp[0x01], dsp[0x04], dsp[0x08] = 0x7f, 0x40, 3, 1

	prefix := writeAPUAuditFixture(t, t.TempDir(), aram, dsp, written, nil)
	report, err := BuildAPUAudit(APUAuditOptions{Prefix: prefix})
	if err != nil {
		t.Fatal(err)
	}
	if report.SilentCapture || report.Summary.ActiveVoices != 1 || report.Summary.ValidSamples != 1 || len(report.Samples) != 1 {
		t.Fatalf("unexpected summary: %+v", report.Summary)
	}
	sample := report.Samples[0]
	if sample.Source != 3 || !sample.Looping || sample.BlockCount != 2 || sample.WriteCoverage != "complete" || sample.EndAddress == nil || *sample.EndAddress != 0x3011 {
		t.Fatalf("unexpected sample: %+v", sample)
	}
}

func TestAPUAuditReportsMissingEndAndUnwrittenBlock(t *testing.T) {
	aram := make([]byte, apuRAMBytes)
	written := make([]byte, apuWrittenBitmapBytes)
	directory := 0x1000 + 7*4
	aram[directory], aram[directory+1] = 0x00, 0x40
	aram[directory+2], aram[directory+3] = 0x00, 0x40
	aram[directory+4], aram[directory+5] = 0x12, 0x40
	markAPUAuditWritten(written, 0x4000, 9)
	prefix := writeAPUAuditFixture(t, t.TempDir(), aram, nil, written, nil)
	page := uint8(0x10)
	report, err := BuildAPUAudit(APUAuditOptions{Prefix: prefix, DirectoryPage: &page, Sources: []uint8{7}})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.InvalidSamples != 1 || len(report.Samples) != 1 {
		t.Fatalf("unexpected summary: %+v", report.Summary)
	}
	reasons := make(map[string]bool)
	for _, finding := range report.Samples[0].Findings {
		reasons[finding.Reason] = true
	}
	if !reasons["unwritten_brr"] || !reasons["missing_end_before_address_cycle"] {
		t.Fatalf("missing findings: %+v", report.Samples[0].Findings)
	}
}

func TestAPUAuditAllowsSamplesToShareDirectorySuffix(t *testing.T) {
	aram := make([]byte, apuRAMBytes)
	written := make([]byte, apuWrittenBitmapBytes)
	directory := 0x1800 + 7*4
	aram[directory], aram[directory+1] = 0x00, 0x40
	aram[directory+2], aram[directory+3] = 0x12, 0x40
	aram[directory+4], aram[directory+5] = 0x12, 0x40
	aram[directory+6], aram[directory+7] = 0x12, 0x40
	aram[0x4000] = 0x00
	aram[0x4009] = 0x00
	aram[0x4012] = 0x03
	markAPUAuditWritten(written, 0x4000, 27)
	prefix := writeAPUAuditFixture(t, t.TempDir(), aram, nil, written, nil)
	page := uint8(0x18)
	report, err := BuildAPUAudit(APUAuditOptions{Prefix: prefix, DirectoryPage: &page, Sources: []uint8{7}})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.ValidSamples != 1 || report.Samples[0].BlockCount != 3 {
		t.Fatalf("shared BRR suffix was treated as a directory bound: %+v", report.Samples[0])
	}
}

func TestAPUAuditRefusesStaleSilentDSPSource(t *testing.T) {
	aram := make([]byte, apuRAMBytes)
	dsp := make([]byte, dspRegisterBytes)
	dsp[0x5d], dsp[0x04] = 0x20, 9
	prefix := writeAPUAuditFixture(t, t.TempDir(), aram, dsp, nil, nil)
	report, err := BuildAPUAudit(APUAuditOptions{Prefix: prefix})
	if err != nil {
		t.Fatal(err)
	}
	if !report.SilentCapture || report.Summary.SourcesAudited != 0 {
		t.Fatalf("silent capture was treated as evidence: %+v", report)
	}
}

func TestAPUAuditCorrelatesKeyOnAndPortOverwrite(t *testing.T) {
	aram := make([]byte, apuRAMBytes)
	dsp := make([]byte, dspRegisterBytes)
	written := make([]byte, apuWrittenBitmapBytes)
	directory := 0x2000 + 3*4
	aram[directory], aram[directory+1] = 0x00, 0x30
	aram[directory+2], aram[directory+3] = 0x00, 0x30
	aram[directory+4], aram[directory+5] = 0x09, 0x30
	aram[0x3000] = 0x01
	markAPUAuditWritten(written, 0x3000, 9)
	trace := []apuTraceLine{
		{Format: "snesrecomp-audio-trace", Version: 2, EventCount: 13, CPUWrites: 3, APUPortApplies: 3, SPCPortReads: 7, CPUPortSameValueRewrites: [4]uint64{1}},
		{Kind: "event", Type: "dsp_write", Address: 0x5d, Value: 0x20},
		{Kind: "event", Type: "dsp_write", Address: 0x00, Value: 0x7f},
		{Kind: "event", Type: "dsp_write", Address: 0x01, Value: 0x40},
		{Kind: "event", Type: "dsp_write", Address: 0x04, Value: 3},
		{Kind: "event", Type: "dsp_write", Address: 0x4c, Value: 1},
		{Kind: "event", Type: "cpu_port_write", Address: 0, Value: 0x10, Frame: 8, SourceBlock: 0x001111, Function: "superseded"},
		{Kind: "event", Type: "cpu_port_write", Address: 0, Value: 0xff, Frame: 8, SourceBlock: 0x00d583, Function: "upload"},
		{Kind: "event", Type: "apu_port_apply", Address: 0, Value: 0xff, Frame: 8},
		{Kind: "event", Type: "cpu_port_write", Address: 0, Value: 1, Frame: 8, SourceBlock: 0x00d590, Function: "command"},
		{Kind: "event", Type: "apu_port_apply", Address: 0, Value: 1, Frame: 8},
		{Kind: "event", Type: "apu_port_apply", Address: 0, Value: 1, Frame: 8},
		{Kind: "event", Type: "spc_port_read", Address: 0, Value: 1, Frame: 9},
	}
	prefix := writeAPUAuditFixture(t, t.TempDir(), aram, dsp, written, trace)
	report, err := BuildAPUAudit(APUAuditOptions{Prefix: prefix})
	if err != nil {
		t.Fatal(err)
	}
	if report.Summary.ObservedKeyOns != 1 || report.Summary.ValidSamples != 1 || len(report.Ports.Overwrites) != 1 {
		t.Fatalf("unexpected report: %+v", report)
	}
	if report.Ports.CPUWrites != 3 || report.Ports.SPCReads != 7 || report.Ports.RetainedSPCReads != 1 || report.Ports.SameValueRewrites[0] != 1 {
		t.Fatalf("raw and retained port counts were not distinguished: %+v", report.Ports)
	}
	overwrite := report.Ports.Overwrites[0]
	if overwrite.PreviousValue != 0xff || overwrite.ReplacementValue != 1 || overwrite.SourceBlock != 0x00d590 || overwrite.Function != "command" {
		t.Fatalf("unexpected overwrite: %+v", overwrite)
	}
	var output strings.Builder
	if err := WriteAPUAudit(&output, report, "text"); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(output.String(), "[PORT-OVERWRITE]") || !strings.Contains(output.String(), "SRCN $03") {
		t.Fatalf("missing text details:\n%s", output.String())
	}
}
