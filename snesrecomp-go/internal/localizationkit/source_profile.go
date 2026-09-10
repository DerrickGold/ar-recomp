package localizationkit

import (
	_ "embed"
	"encoding/json"
)

type namedSource struct {
	ID      string `json:"id"`
	Address int    `json:"address"`
}
type sourceTable struct {
	namedSource
	Count     int `json:"count"`
	Increment int `json:"increment"`
}
type directSourceTable struct {
	Address   int      `json:"address"`
	IDs       []string `json:"ids"`
	Increment int      `json:"increment"`
}
type indexedSource struct {
	namedSource
	NativeIndex int      `json:"native_index"`
	Table       int      `json:"table"`
	IDs         []string `json:"ids"`
}
type sourceContinuation struct {
	Call   int `json:"call"`
	Parent int `json:"parent"`
	Source int `json:"source"`
}
type sourceWrapper struct {
	Entry      int    `json:"entry"`
	Kind       string `json:"kind"`
	Count      int    `json:"count"`
	SourceBank int    `json:"source_bank"`
}
type sourceRelay struct {
	Entry int `json:"entry"`
	Count int `json:"count"`
}
type sourceGroup struct {
	ID    string `json:"id"`
	Count int    `json:"count"`
}
type sourceFlow struct {
	SelectorCall     int   `json:"selector_call"`
	SampleCall       int   `json:"sample_call"`
	ChoiceYieldCall  *int  `json:"choice_yield_call"`
	ChoiceDescriptor *int  `json:"choice_descriptor"`
	ChoiceLoadSites  []int `json:"choice_load_sites"`
}
type dictionaryProof struct {
	Interactive   int  `json:"interactive"`
	Fixed         int  `json:"fixed"`
	UploadFlag    byte `json:"upload_flag"`
	TrailingSpace bool `json:"trailing_space"`
}
type sourceProfile struct {
	ID                   string               `json:"id"`
	InteractiveEntry     int                  `json:"interactive_entry"`
	InteractiveEnd       int                  `json:"interactive_end"`
	ComposerEntry        int                  `json:"composer_entry"`
	ComposerEnd          int                  `json:"composer_end"`
	InteractiveCallCount int                  `json:"interactive_call_count"`
	Nonadjacent          []string             `json:"nonadjacent"`
	BranchJoins          map[int]int          `json:"branch_joins"`
	Continuations        []sourceContinuation `json:"continuations"`
	DialogueBank         int                  `json:"dialogue_bank"`
	Wrappers             []sourceWrapper      `json:"wrappers"`
	Relay                sourceRelay          `json:"relay"`
	HandlerTable         int                  `json:"handler_table"`
	OfferingTable        int                  `json:"offering_table"`
	EndingTable          *int                 `json:"ending_table"`
	MenuStart            int                  `json:"menu_start"`
	MenuEnd              int                  `json:"menu_end"`
	Tables               []sourceTable        `json:"tables"`
	DirectTable          *directSourceTable   `json:"direct_table"`
	IndexedDirect        []indexedSource      `json:"indexed_direct"`
	Dynamic              []namedSource        `json:"dynamic"`
	Numeric              *int                 `json:"numeric"`
	NameEntry            []namedSource        `json:"name_entry"`
	Groups               []sourceGroup        `json:"groups"`
	Flow                 *sourceFlow          `json:"flow"`
	DictionaryProof      *dictionaryProof     `json:"dictionary_proof"`
	SpaceDelimitedWords  bool                 `json:"space_delimited_words"`
}

// Generated structural facts only: no discovered pointers, call sites, or prose.
//
//go:embed data/source-profiles.json
var sourceProfileJSON []byte

var sourceProfiles = func() map[string]sourceProfile {
	var profiles []sourceProfile
	if err := json.Unmarshal(sourceProfileJSON, &profiles); err != nil {
		panic("invalid embedded source profiles: " + err.Error())
	}
	result := make(map[string]sourceProfile, len(profiles))
	for _, profile := range profiles {
		if _, exists := result[profile.ID]; exists {
			panic("duplicate source profile")
		}
		result[profile.ID] = profile
	}
	return result
}()
