package tooling

import (
	"bytes"
	"encoding/json"
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func TestDeferredCommandFieldRelationship(t *testing.T) {
	for _, bank := range []byte{0, 2} {
		g, streams := commandStreamFixture(t, bank, map[uint16][]byte{0x8800: {0xa6, 0x70, 0xb9, 2, 0, 0x95, 0x38, 0xc8, 0xc8, 0x60}})
		p := streams[0].Commands[0]
		if p.StopReason != "deferred_field_store" || p.Callback != nil || p.Deferred == nil {
			t.Fatalf("prefix %+v", p)
		}
		d := p.Deferred
		if d.StoreMode != "dp,x" || d.StoreOperand != 0x38 || d.OperandOffset != 2 || d.StreamBankSource != "command_entry_db" || d.StreamBank != nil || d.Width != 2 {
			t.Fatalf("operand %+v", d)
		}
		// The store ends the prefix. The following INY/INY does not establish
		// a complete command length, a saved cursor alias or a callback value.
		if p.CursorDelta != 0 || len(d.Consumers) != 0 {
			t.Fatal("invented deferred effects")
		}
		image := make(romimage.Image, (int(bank)+1)*0x8000)
		copy(image[int(bank)*0x8000+0x900:], []byte{0xb5, 0x38, 0xd0, 1, 0x60, 0x8b, 0x4b, 0xab, 0xda, 0x85, 0x72, 0xf4, 0x11, 0x89, 0x6c, 0x72, 0})
		consumer, err := decoder.DecodeFunction(image, bank, 0x8900, 0, 0, decoder.Options{})
		if err != nil {
			t.Fatal(err)
		}
		results := []shadowDecodeResult{
			{entry: decoder.Variant{Address: g.Entry.PC}, commandStreams: streams},
			{entry: decoder.Variant{Address: consumer.Entry.PC}, forwardedFields: decoder.ForwardedIndirectFields(consumer)},
		}
		out := mergeShadowCommandStreams(results)
		joined := out[0].Commands[0].Deferred
		if len(joined.Consumers) != 1 || joined.Consumers[0].Native.DispatchPC != uint32(bank)<<16|0x890e || joined.Consumers[0].Native.RequiredD != 0 {
			t.Fatalf("missing consumer %+v", joined)
		}
		if len(streams[0].Commands[0].Deferred.Consumers) != 0 {
			t.Fatal("merge mutated input")
		}
		reversed := []shadowDecodeResult{results[1], results[0], results[1]}
		if !reflect.DeepEqual(out, mergeShadowCommandStreams(reversed)) {
			t.Fatal("non-deterministic/de-duplicated join")
		}
		report := ShadowReport{Version: shadowReportVersion, NoWrite: true, CommandStreams: out, DeferredFields: mergeShadowDeferredFields(results)}
		var b bytes.Buffer
		if err := WriteShadowReport(&b, report, "json", true); err != nil {
			t.Fatal(err)
		}
		var decoded ShadowReport
		if err := json.Unmarshal(b.Bytes(), &decoded); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(decoded.CommandStreams, out) || !strings.Contains(b.String(), "object_index_equality") {
			t.Fatal("evidence did not roundtrip")
		}
		facts, _ := SelectStaticProvenDatabaseDispatchFacts(report)
		if len(facts) != 0 || len(SelectStaticProvenRoutineEntryFacts(report)) != 0 {
			t.Fatal("conditional fields became generation proof")
		}
		b.Reset()
		if err := WriteShadowReport(&b, report, "text", true); err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(b.String(), "[DEFERRED-FIELD]") || !strings.Contains(b.String(), "consumers=1") {
			t.Fatal(b.String())
		}
		b.Reset()
		if err := WriteShadowReport(&b, report, "text", false); err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(b.String(), "deferred field forwarding: 1 conditional native shape(s)") ||
			!strings.Contains(b.String(), "target values are not proven") || strings.Contains(b.String(), "[DEFERRED-FIELD]") {
			t.Fatal("summary lost conditional status or leaked verbose details: " + b.String())
		}
		// Similar spelling does not join unlike address modes or offsets.
		for _, mismatch := range []decoder.ForwardedIndirectField{{Mode: "abs,x", Operand: 0x38}, {Mode: "dp,x", Operand: 0x39}} {
			results[1].forwardedFields = []decoder.ForwardedIndirectField{mismatch}
			if len(mergeShadowCommandStreams(results)[0].Commands[0].Deferred.Consumers) != 0 {
				t.Fatal("unlike field joined")
			}
		}
	}
}

func TestDeferredCommandBankProvenance(t *testing.T) {
	_, streams := commandStreamFixture(t, 0, map[uint16][]byte{0x8800: {0xf4, 1, 0x82, 0xab, 0xab, 0xb9, 0, 0, 0x9d, 0x34, 0x12, 0x60}})
	d := streams[0].Commands[0].Deferred
	if d == nil || d.StoreMode != "abs,x" || d.StreamBank == nil || *d.StreamBank != 0x82 || d.StreamBankSource != "constant" {
		t.Fatalf("lost stream DB: %+v", d)
	}
	if len(d.ProofObligations) == 0 || !reflect.DeepEqual(d.ProofObligations, deferredFieldObligations()) {
		t.Fatal("lost conditional obligations")
	}
}
