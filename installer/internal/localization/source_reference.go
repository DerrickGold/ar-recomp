package localization

// NativeSourceReference records one proven consumer edge. Resolution is absent
// during discovery, then attached to a copied reference by catalog resolution.
// The embedded fields preserve the existing flat diagnostic JSON format.
type NativeSourceReference struct {
	SourcePC24       string `json:"source_pc24"`
	ReferenceKind    string `json:"reference_kind"`
	ViaCallSite      string `json:"via_call_site,omitempty"`
	ViaSourceTable   string `json:"via_source_table,omitempty"`
	ViaConsumerEntry string `json:"via_consumer_entry,omitempty"`
	*NativeSourceResolution
}

type NativeSourceResolution struct {
	ResolvedRecordID         *string `json:"resolved_record_id"`
	SourceOffsetWithinRecord *int    `json:"source_offset_within_record,omitempty"`
}

type NativeSourceSeeds struct {
	Status            string                  `json:"status"`
	ReferenceCount    int                     `json:"reference_count"`
	UniqueSourceCount int                     `json:"unique_source_count"`
	References        []NativeSourceReference `json:"references"`
}

type NativeSourceResolutionSummary struct {
	UniqueSourceCount         int      `json:"unique_source_count"`
	MappedUniqueSourceCount   int      `json:"mapped_unique_source_count"`
	UnmappedUniqueSourceCount int      `json:"unmapped_unique_source_count"`
	AllCurrentSeedsMapped     bool     `json:"all_current_seeds_mapped"`
	UnmappedSourcePC24s       []string `json:"unmapped_source_pc24s"`
}

type sourceReferenceResolution struct {
	References []NativeSourceReference
	Summary    NativeSourceResolutionSummary
}
