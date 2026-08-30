package analysis

import "testing"

func TestEntryFactNormalizeDeduplicatesValueEqualRegionOwners(t *testing.T) {
	ownerA := EntryVariant{PC: 0xff008000, EntryMX: MXState{M: 3, X: 3}}
	ownerB := ownerA
	edge := EntryEdge{
		Source: EntryVariant{PC: 0xff008010, EntryMX: MXState{M: 3, X: 3}},
		Target: EntryVariant{PC: 0xff008100, EntryMX: MXState{M: 3, X: 3}},
	}
	fact := EntryFact{
		PC: 0xff008100, EntryMX: MXState{M: 3, X: 3},
		RegionOwners: []EntryVariant{ownerA, ownerB},
		ResumeEdges: []EntryEdge{
			{Source: edge.Source, Target: edge.Target, RegionOwner: &ownerA},
			{Source: edge.Source, Target: edge.Target, RegionOwner: &ownerB},
		},
	}
	fact.Normalize()
	if fact.PC != 0x008100 || fact.EntryMX != (MXState{M: 1, X: 1}) || len(fact.RegionOwners) != 1 {
		t.Fatalf("normalized fact identity/owners = %+v", fact)
	}
	if len(fact.ResumeEdges) != 1 || fact.ResumeEdges[0].RegionOwner == nil ||
		*fact.ResumeEdges[0].RegionOwner != (EntryVariant{PC: 0x008000, EntryMX: MXState{M: 1, X: 1}}) {
		t.Fatalf("normalized/deduplicated resume edges = %+v", fact.ResumeEdges)
	}
}
