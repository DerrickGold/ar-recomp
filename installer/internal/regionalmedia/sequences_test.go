package regionalmedia

import (
	"bytes"
	"crypto/sha256"
	"os"
	"path/filepath"
	"testing"
)

func TestSequenceSources(t *testing.T) {
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		t.Skip("optional five-ROM sequence compatibility")
	}
	read := func(name string) []byte {
		t.Helper()
		b, err := os.ReadFile(filepath.Join(root, name))
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	us := read("ar.sfc")
	for _, name := range []string{"ar.sfc", "ar-jp.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc"} {
		rom := read(name)
		if !bytes.Equal(rom[0x40000:0x50000], us[0x40000:0x50000]) {
			t.Fatal(name, "sample pool mismatch")
		}
		for i, source := range []int{0x7769f, 0xcfa4b} {
			native, samples, err := sequenceImage(us, source)
			if err != nil {
				t.Fatal(err)
			}
			prefix := us[source : source+4+len(native[0])]
			donorSource := bytes.Index(rom, prefix)
			if donorSource < 0 || bytes.Index(rom[donorSource+1:], prefix) >= 0 {
				t.Fatal(name, "non-unique song identity")
			}
			donor, donorSamples, err := sequenceImage(rom, donorSource)
			if err != nil {
				t.Fatal(name, err)
			}
			for block := range native {
				if block == 2 && name == "ar-jp.sfc" {
					continue
				}
				if !bytes.Equal(native[block], donor[block]) {
					t.Fatal(name, i, block, "unexpected block difference")
				}
			}
			if !bytes.Equal(samples, donorSamples) {
				t.Fatal(name, "sample selection mismatch")
			}
			if name == "ar-jp.sfc" {
				if donorSource != []int{0xc0000, 0xd31e1}[i] || bytes.Equal(native[2], donor[2]) {
					t.Fatal("missing Japanese sequence difference")
				}
				resources, err := japaneseSequences(rom)
				if err != nil || !bytes.Equal(resources[i].Bytes, donor[2]) {
					t.Fatal("extracted sequence mismatch", err)
				}
				t.Logf("JP song%d source=%x sequence=%d sha256=%x", []int{9, 12}[i], donorSource, len(donor[2]), sha256.Sum256(donor[2]))
			}
		}
	}
}

func TestSequenceImageRejectsTruncation(t *testing.T) {
	for size := 0; size < 80; size++ {
		if _, _, err := sequenceImage(make([]byte, size), 0); err == nil {
			t.Fatal("accepted malformed image")
		}
	}
	if _, _, err := sequenceImage(nil, -1); err == nil {
		t.Fatal("accepted negative source")
	}
}
