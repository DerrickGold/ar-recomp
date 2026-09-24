package regionalmedia

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func TestCatalog(t *testing.T) {
	var last uint32
	for _, d := range Definitions() {
		if d.ID <= last || d.Size == 0 || d.Size > MaximumBytes || d.Key == "" {
			t.Fatal(d)
		}
		last = d.ID
		for _, hash := range d.Hashes {
			b, err := hex.DecodeString(hash)
			if err != nil || len(b) != 32 {
				t.Fatal(hash)
			}
		}
	}
	if _, err := Pack(nil); err == nil {
		t.Fatal("nil extraction accepted")
	}
	if _, err := Pack(&Extraction{}); err == nil {
		t.Fatal("unknown donor accepted")
	}
}

func TestGeneratedCatalogCurrent(t *testing.T) {
	path := filepath.Join(t.TempDir(), "catalog.inc")
	cmd := exec.Command("go", "run", "./cmd/gencatalog", path)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("%v: %s", err, output)
	}
	want, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile("../../../src/regional/media/regional_media_catalog.inc")
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(want, got) {
		t.Fatal("run go generate ./internal/regionalmedia; C catalog is stale")
	}
}

func TestROMPackageRoundTrip(t *testing.T) {
	root := os.Getenv("AR_MEDIA_ROM_DIR")
	if root == "" {
		t.Skip("optional donor ROM roundtrip")
	}
	for _, name := range []string{"ar.sfc", "ar-jp.sfc", "ar-eu.sfc", "ar-ger.sfc", "ar-fra.sfc"} {
		rom, err := os.ReadFile(filepath.Join(root, name))
		if err != nil {
			t.Fatal(err)
		}
		e, err := Extract(rom)
		if err != nil {
			t.Fatal(err)
		}
		data, err := Pack(e)
		if err != nil {
			t.Fatal(err)
		}
		restored, err := Unpack(data)
		if err != nil {
			t.Fatal(err)
		}
		again, err := Pack(restored)
		if err != nil || !bytes.Equal(data, again) {
			t.Fatal("non-deterministic roundtrip", err)
		}
		// Exhaust small resources. The larger actor packet is SHA-covered too;
		// sample its block boundaries here instead of repeatedly hashing O(n²)
		// picture bytes. Its structural parser has separate malformed tests.
		actorOffset := len(data)
		for i, r := range e.Resources {
			if r.ID == ActorArt {
				actorOffset = int(binary.LittleEndian.Uint32(data[headerBytes+i*entryBytes+4:]))
			}
		}
		for i := range data {
			if i >= actorOffset && i%4096 != 0 && i != actorOffset && i != len(data)-1 {
				continue
			}
			data[i] ^= 0x80
			if _, err := Unpack(data); err == nil {
				t.Fatalf("accepted changed byte %s/%d", name, i)
			}
			data[i] ^= 0x80
		}
		for end := 0; end < len(data); end++ {
			if _, err := Unpack(data[:end]); err == nil {
				t.Fatal("accepted truncation", end)
			}
		}
		if _, err := Unpack(append(data, 0)); err == nil {
			t.Fatal("accepted trailing data")
		}
		restored.Resources[0].Bytes[0] ^= 1
		if bytes.Equal(restored.Resources[0].Bytes, e.Resources[0].Bytes) {
			t.Fatal("resource alias")
		}
		if _, err := Pack(restored); err == nil {
			t.Fatal("modified extraction accepted")
		}
	}
}

func FuzzUnpack(f *testing.F) {
	f.Add([]byte("ARMEDIA\x00"))
	f.Fuzz(func(t *testing.T, data []byte) {
		e, err := Unpack(data)
		if err != nil {
			return
		}
		out, err := Pack(e)
		if err != nil || !bytes.Equal(data, out) {
			t.Fatal("noncanonical accepted", err)
		}
	})
}
