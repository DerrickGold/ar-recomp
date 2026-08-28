package analysis

import "testing"

func address(value uint32) *uint32 { return &value }

func TestCompareDispatchFactsExactPartialAndConflict(t *testing.T) {
	authored := []DispatchFact{
		{SitePC: 0x008000, Transfer: TransferResume, TargetEntryKind: EntryContinuation, Targets: []uint32{0x008100}, TargetSetClosed: true},
		{SitePC: 0x008010, Transfer: TransferCall, TargetEntryKind: EntryComputed, Targets: []uint32{0x008200}, TargetSetClosed: true, IndexRegister: "A", TableBases: []uint32{0x008300}, ReturnPC: address(0x008011)},
		{SitePC: 0x008020, Transfer: TransferTail, TargetEntryKind: EntryComputed, Targets: []uint32{0x008400}, TargetSetClosed: true},
	}
	inferred := []DispatchFact{
		{SitePC: 0x008000, Transfer: TransferResume, TargetEntryKind: EntryContinuation, Targets: []uint32{0x008100}, TargetSetClosed: true},
		{SitePC: 0x008010, Transfer: TransferCall, TargetEntryKind: EntryComputed, IndexRegister: "A", TableBases: []uint32{0x008300}, ReturnPC: address(0x008011), UnknownFields: []string{"targets"}},
		{SitePC: 0x008020, Transfer: TransferCall, TargetEntryKind: EntryComputed, Targets: []uint32{0x008400}, TargetSetClosed: true},
		{SitePC: 0x008030, Transfer: TransferTail, TargetEntryKind: EntryComputed, Targets: []uint32{0x008500}, TargetSetClosed: true},
	}
	comparisons, summary := CompareDispatchFacts(authored, inferred)
	if summary.ExactMatches != 1 || summary.PartialMatches != 1 || summary.Conflicts != 1 || summary.Automatic != 1 {
		t.Fatalf("unexpected summary: %+v", summary)
	}
	want := []ComparisonStatus{ComparisonExact, ComparisonPartial, ComparisonConflict, ComparisonAutomatic}
	for index, status := range want {
		if comparisons[index].Status != status {
			t.Errorf("comparison %d status = %s, want %s", index, comparisons[index].Status, status)
		}
	}
}

func TestCompareDispatchFactsRecognizesCompatibleContinuationGuards(t *testing.T) {
	authored := []DispatchFact{
		{SitePC: 0x038100, Mnemonic: "RTS", Transfer: TransferResume, TargetEntryKind: EntryContinuation, Targets: []uint32{0x038200, 0x038300}, TargetSetClosed: true},
		{SitePC: 0x038110, Mnemonic: "RTS", Transfer: TransferResume, TargetEntryKind: EntryContinuation, Targets: []uint32{0x038200, 0x038300}, TargetSetClosed: true},
	}
	inferred := []DispatchFact{
		{SitePC: 0x038100, Mnemonic: "RTS", Transfer: TransferResume, TargetEntryKind: EntryContinuation, Targets: []uint32{0x038200}, TargetSetClosed: true},
		{SitePC: 0x038120, Mnemonic: "RTS", Transfer: TransferResume, TargetEntryKind: EntryContinuation, Targets: []uint32{0x038300}, TargetSetClosed: true},
	}
	comparisons, summary := CompareDispatchFacts(authored, inferred)
	if summary.Compatible != 2 || summary.Conflicts != 0 || summary.AuthoredOnly != 0 {
		t.Fatalf("unexpected summary: %+v", summary)
	}
	if comparisons[0].Status != ComparisonCompatible || comparisons[1].Status != ComparisonCompatible {
		t.Fatalf("statuses = %s, %s; want compatible guards", comparisons[0].Status, comparisons[1].Status)
	}
}

func TestCompareDispatchFactsSeparatesGarbageOnlyAutomaticFacts(t *testing.T) {
	inferred := []DispatchFact{{
		SitePC: 0x008001, Transfer: TransferTail,
		TargetEntryKind: EntryComputed, CodeOwnership: OwnershipGarbageOnly,
	}}
	comparisons, summary := CompareDispatchFacts(nil, inferred)
	if summary.Automatic != 0 || summary.GarbageOnly != 1 {
		t.Fatalf("unexpected summary: %+v", summary)
	}
	if len(comparisons) != 1 || comparisons[0].Status != ComparisonGarbageOnly {
		t.Fatalf("comparisons = %+v, want one garbage-only fact", comparisons)
	}
}
