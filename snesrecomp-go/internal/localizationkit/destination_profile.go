package localizationkit

import (
	_ "embed"
	"encoding/json"
)

type destinationSites struct {
	ID    string `json:"id"`
	Hex   string `json:"hex"`
	Sites []int  `json:"sites"`
}
type destinationSignature struct {
	ID      string `json:"id"`
	Address int    `json:"address"`
	Hex     string `json:"hex"`
}
type destinationRegion struct {
	ID      string `json:"id"`
	Indices []int  `json:"indices"`
}
type destinationProfile struct {
	BaseWriteCount   int                    `json:"base_write_count"`
	OutsideRoles     []string               `json:"outside_roles"`
	LongWriteCount   int                    `json:"long_write_count"`
	Auxiliary        []namedSource          `json:"auxiliary"`
	HUDSources       map[string]int         `json:"hud_sources"`
	ContextWords     int                    `json:"context_words"`
	Noncode          []int                  `json:"noncode"`
	RangeCount       int                    `json:"range_count"`
	RangeLongCount   int                    `json:"range_long_count"`
	RangeRejected    []destinationSignature `json:"range_rejected"`
	VRAMPaths        []destinationSites     `json:"vram_paths"`
	DMA              []namedSource          `json:"dma"`
	Indirect         []destinationSites     `json:"indirect"`
	IndirectRejected []destinationSignature `json:"indirect_rejected"`
	IndirectCount    *int                   `json:"indirect_count"`
	DescriptorABI    string                 `json:"descriptor_abi"`
}
type destinationFactsData struct {
	Profiles           map[string]destinationProfile     `json:"profiles"`
	HUDRegions         map[string][]destinationRegion    `json:"hud_regions"`
	VRAMDetails        map[string]IRObject               `json:"vram_details"`
	DMARoles           []string                          `json:"dma_roles"`
	DMADetails         map[string][]string               `json:"dma_details"`
	DescriptorABIs     map[string]IRObject               `json:"descriptor_abis"`
	DescriptorPatterns map[string][]destinationSignature `json:"descriptor_patterns"`
	DescriptorDetails  map[string]IRObject               `json:"descriptor_details"`
	IndirectDetails    map[string]IRObject               `json:"indirect_details"`
}

// Structural instruction/data-flow contracts only; no font tiles or prose.
//
//go:embed data/destination-profiles.json
var destinationProfileJSON []byte
var destinationFacts = func() destinationFactsData {
	var facts destinationFactsData
	if err := json.Unmarshal(destinationProfileJSON, &facts); err != nil {
		panic("invalid embedded destination facts: " + err.Error())
	}
	return facts
}()

// Copy declarative report fields recursively, so editing a diagnostic result
// cannot change the embedded facts used for subsequent extraction.
func cloneIR(value any) any {
	switch v := value.(type) {
	case IRObject:
		out := IRObject{}
		for key, child := range v {
			out[key] = cloneIR(child)
		}
		return out
	case map[string]any:
		return cloneIR(IRObject(v))
	case []any:
		out := make([]any, len(v))
		for i, child := range v {
			out[i] = cloneIR(child)
		}
		return out
	default:
		return value
	}
}
