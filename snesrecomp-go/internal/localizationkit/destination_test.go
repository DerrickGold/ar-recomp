package localizationkit

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"reflect"
	"slices"
	"testing"
)

type destinationTestCase struct {
	ID       string `json:"id"`
	Method   string `json:"method"`
	Site     int    `json:"site"`
	Role     string `json:"role"`
	Expected any    `json:"expected"`
	Reject   bool   `json:"reject"`
	Offset   int    `json:"offset"`
	Data     []int  `json:"data"`
}

func runDestinationCase(d *Decoder, p destinationProfile, c destinationTestCase) (any, error) {
	var result any
	var err error
	switch c.Method {
	case "outside":
		result, err = d.outsideBG3(c.Site, c.Role)
	case "hud":
		result, err = d.hudTemplate(c.Site, c.Role, p)
	case "range":
		result, err = d.decodedBG3Evidence(p)
	case "vram":
		result, err = d.directVRAMPaths(p)
	case "dma":
		result, err = d.dmaLaunches(p)
	case "descriptor":
		result, err = d.vramDescriptor(p)
	case "indirect":
		result, err = d.indirectDestinations(p)
	case "ending":
		result, err = d.endingGraphicalSurface()
	case "asset_script":
		var entries []assetEntry
		entries, err = d.assetScript()
		if err == nil {
			rows := []IRObject{}
			for _, entry := range entries {
				commands := []IRObject{}
				for _, command := range entry.commands {
					commands = append(commands, IRObject{"command": int(command.code), "operands_hex": hex.EncodeToString(command.operands)})
				}
				rows = append(rows, IRObject{"mode": int(entry.mode), "submode": int(entry.submode), "commands": commands})
			}
			result = rows
		}
	case "title", "font":
		var entries []assetEntry
		entries, err = d.assetScript()
		if err == nil {
			if c.Method == "title" {
				result, err = d.titleGraphicalSurface(entries)
			} else {
				result, err = d.dialogFont(entries)
			}
		}
	case "decompress":
		var data []byte
		var consumed int
		data, consumed, err = decompressNative(d.rom, 0)
		if err == nil {
			result = IRObject{"decoded_hex": hex.EncodeToString(data), "stream_cursor_bytes": consumed}
		}
	default:
		err = fmt.Errorf("unknown reference method %q", c.Method)
	}
	if err != nil {
		return nil, err
	}
	return result, nil
}

func TestDestinationReferenceParity(t *testing.T) {
	path := os.Getenv("AR_LOCALIZATION_DESTINATION_VECTORS")
	if path == "" {
		t.Skip("run tools/check_go_localization_decoder.py for independent destination parity")
	}
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	var groups []struct {
		ID      string                `json:"id"`
		ROM     []byte                `json:"rom"`
		Profile destinationProfile    `json:"profile"`
		Cases   []destinationTestCase `json:"cases"`
	}
	if err := json.NewDecoder(io.LimitReader(file, 32<<20)).Decode(&groups); err != nil {
		t.Fatal(err)
	}
	if len(groups) == 0 {
		t.Fatal("empty destination gate")
	}
	total, rejected := 0, 0
	for _, group := range groups {
		if len(group.Cases) == 0 {
			t.Fatal("empty group", group.ID)
		}
		for _, c := range group.Cases {
			d := catalogTestDecoder(t, group.ROM)
			if len(c.Data) > 0 {
				d, err = mutatedDecoder(t, d, referenceMutation{Offset: c.Offset, Data: c.Data})
				if err != nil {
					t.Fatal(err)
				}
			}
			result, err := runDestinationCase(d, group.Profile, c)
			if c.Reject {
				if err == nil || result != nil {
					t.Fatalf("%s/%s: broken proof accepted", group.ID, c.ID)
				}
				rejected++
			} else {
				if err != nil {
					t.Fatalf("%s/%s: %v", group.ID, c.ID, err)
				}
				requireJSONEqual(t, group.ID+"/"+c.ID, result, c.Expected)
			}
			total++
		}
	}
	t.Logf("%d destination/asset/LZSS cases agree (%d intentional failures)", total, rejected)
}

func TestDestinationBoundsAndEvidence(t *testing.T) {
	if len(destinationFacts.Profiles) != len(decoderFacts.Profiles) {
		t.Fatal("destination profile coverage differs")
	}
	for _, profile := range decoderFacts.Profiles {
		p, ok := destinationFacts.Profiles[profile.ID]
		if !ok || len(p.VRAMPaths) != 5 || len(p.OutsideRoles) != 5 || len(p.Indirect) != 9 {
			t.Fatal("missing destination obligations", profile.ID)
		}
	}
	d := catalogTestDecoder(t, make([]byte, 64))
	for _, site := range []int{-1, 0, 0x8000, 0x803f, 0x7fffff} {
		if result, err := d.outsideBG3(site, "status_strip_clear"); err == nil || result != nil {
			t.Fatal("invalid maintenance proof", site)
		}
		if result, err := d.hudTemplate(site, "sim_sky_hud_template", destinationProfile{}); err == nil || result != nil {
			t.Fatal("invalid template proof", site)
		}
	}
	if result, err := d.assetScript(); err == nil || result != nil {
		t.Fatal("truncated asset header accepted")
	}
	for _, method := range []string{"descriptor", "vram", "dma", "ending", "title", "font", "outside", "hud"} {
		if _, err := runDestinationCase(d, destinationProfile{}, destinationTestCase{Method: method}); err == nil {
			t.Fatal("empty profile/source accepted", method)
		}
	}
	for _, sig := range []destinationSignature{{Address: 0x8000, Hex: ""}, {Address: 0x8000, Hex: "xyz"}, {Address: 0x803f, Hex: "0000"}} {
		if result, err := d.rejectedDestinations([]destinationSignature{sig}, "test"); err == nil || result != nil {
			t.Fatal("invalid rejected-decode evidence accepted")
		}
	}
	p := destinationProfile{RangeCount: 1}
	if result, err := d.decodedBG3Evidence(p); err == nil || result != nil {
		t.Fatal("inconsistent range evidence accepted")
	}
	count := 1
	p.IndirectCount = &count
	if result, err := d.indirectDestinations(p); err == nil || result != nil {
		t.Fatal("inconsistent indirect evidence accepted")
	}
	p.IndirectCount = nil
	p.Indirect = []destinationSites{{ID: "asset_workspace", Hex: "0000", Sites: []int{0x8000, 0x8000}}}
	if result, err := d.indirectDestinations(p); err == nil || result != nil {
		t.Fatal("duplicate indirect write accepted")
	}
}

func TestDestinationRawScanAndMetadataIsolation(t *testing.T) {
	rom := instructionBytes("8f00b07f9fffbf7f8f00c07f9fffaF7f8f00b0")
	rows := scanLongBG3(rom)
	if len(rows) != 2 || rows[0]["destination"] != "$7F:B000" || rows[1]["destination"] != "$7F:BFFF" {
		t.Fatal("long store range/opcode scan", rows)
	}
	for size := 0; size < 4; size++ {
		if len(scanLongBG3(rom[:size])) != 0 {
			t.Fatal("truncated long store accepted")
		}
	}
	before, err := json.Marshal(destinationFacts.DescriptorABIs["native"])
	if err != nil {
		t.Fatal(err)
	}
	copy := cloneIR(destinationFacts.DescriptorABIs["native"]).(IRObject)
	copy["slot_0"].(IRObject)["source_address"] = "changed"
	after, err := json.Marshal(destinationFacts.DescriptorABIs["native"])
	if err != nil || !bytes.Equal(before, after) {
		t.Fatal("diagnostic edit changed embedded ABI")
	}
	roles := slices.Clone(destinationFacts.DMARoles)
	clone := slices.DeleteFunc(slices.Clone(roles), func(s string) bool { return s == "world_effect" })
	if len(clone) != 11 || !reflect.DeepEqual(roles, destinationFacts.DMARoles) {
		t.Fatal("regional role filtering mutated shared facts")
	}
}

func TestNativeCompressedAndAssetBoundaries(t *testing.T) {
	for _, input := range [][]byte{nil, {1}, {1, 0}, {1, 0, 0x80}, {2, 0, 0}} {
		if data, consumed, err := decompressNative(input, 0); err == nil || data != nil || consumed != 0 {
			t.Fatal("truncated compressed data accepted", input)
		}
	}
	for _, offset := range []int{-1, 2, 3, 1 << 30} {
		if _, _, err := decompressNative([]byte{0, 0}, offset); err == nil {
			t.Fatal("bad compressed offset", offset)
		}
	}
	data, cursor, err := decompressNative([]byte{0, 0}, 0)
	if err != nil || len(data) != 0 || cursor != 1 {
		t.Fatal("zero-size cursor convention changed", data, cursor, err)
	}
	// Eight 9-bit zero literals end exactly on a byte boundary. The reference
	// cursor convention reports one beyond the consumed stream, excludes header.
	data, cursor, err = decompressNative(instructionBytes("0800804020100804020100"), 0)
	if err != nil || !bytes.Equal(data, make([]byte, 8)) || cursor != 10 {
		t.Fatal("aligned stream cursor contract", data, cursor, err)
	}
	rom := make([]byte, assetScriptLimit)
	d := catalogTestDecoder(t, rom)
	if result, err := d.assetScript(); err == nil || result != nil {
		t.Fatal("unterminated scene table accepted")
	}
	copy(rom[assetScriptBase+3:], []byte{7, 8, 0})
	d = catalogTestDecoder(t, rom[:assetScriptBase+6])
	entries, err := d.assetScript()
	if err != nil || len(entries) != 1 {
		t.Fatal("bounded final entry", err)
	}
	if result, err := d.dialogFont(entries); err == nil || result != nil {
		t.Fatal("missing font source accepted")
	}
}

func FuzzNativeAssetBounds(f *testing.F) {
	for _, data := range [][]byte{nil, {0, 0}, {1, 0, 0xff}, {16, 0, 0, 0}, {7, 8, 0}, {7, 8, 0x80}} {
		f.Add(data)
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		if len(data) > 2048 {
			return
		}
		decoded, consumed, err := decompressNative(data, 0)
		if err != nil {
			if decoded != nil || consumed != 0 {
				t.Fatal("partial decompression escaped")
			}
		} else if len(data) < 2 || len(decoded) != word(data) || consumed < 1 || consumed > len(data) {
			t.Fatal("decompression escaped bounds")
		}
		rom := make([]byte, assetScriptBase+3+len(data))
		copy(rom[assetScriptBase+3:], data)
		d := catalogTestDecoder(t, rom)
		entries, err := d.assetScript()
		if err != nil {
			if entries != nil {
				t.Fatal("partial script escaped")
			}
			return
		}
		for _, entry := range entries {
			if entry.start < assetScriptBase+3 || entry.end > len(rom) || entry.end <= entry.start {
				t.Fatal("script bounds escaped")
			}
		}
	})
}
