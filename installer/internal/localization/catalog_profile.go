package localization

import (
	_ "embed"
	"encoding/json"
)

type catalogLookup struct {
	ID          string   `json:"id"`
	Offset      int      `json:"offset"`
	SemanticIDs []string `json:"semantic_ids"`
}
type catalogProfile struct {
	ID                string           `json:"id"`
	Japanese          bool             `json:"japanese"`
	TownTable         int              `json:"town_table"`
	EnemyTable        int              `json:"enemy_table"`
	AngelStart        int              `json:"angel_start"`
	AngelEnd          int              `json:"angel_end"`
	HandlerTable      int              `json:"handler_table"`
	TownStart         int              `json:"town_start"`
	OfferingTable     int              `json:"offering_table"`
	EndingTable       *int             `json:"ending_table"`
	PostTextEnd       *int             `json:"post_text_end"`
	StageTable        int              `json:"stage_table"`
	TitleStart        int              `json:"title_start"`
	TitleEnd          int              `json:"title_end"`
	ActionStart       int              `json:"action_start"`
	ActionEnd         int              `json:"action_end"`
	SoundStart        int              `json:"sound_start"`
	SoundEnd          int              `json:"sound_end"`
	LookupTables      []catalogLookup  `json:"lookup_tables"`
	TitleRouteIDs     []string         `json:"title_route_ids"`
	RegionalTitle     bool             `json:"regional_title"`
	InteractiveRoutes map[int][]string `json:"interactive_routes"`
}
type catalogFactsData struct {
	Profiles              []catalogProfile    `json:"profiles"`
	CityIDs               []string            `json:"city_ids"`
	CityKeys              []string            `json:"city_keys"`
	EnemyIDs              []string            `json:"enemy_ids"`
	ActionIDs             []string            `json:"action_ids"`
	ComposerPointerRoutes map[string][]string `json:"composer_pointer_routes"`
}

// Structural catalogue bounds and semantic contracts; never extracted prose.
//
//go:embed data/catalog-profiles.json
var catalogProfileJSON []byte
var catalogFacts = func() catalogFactsData {
	var facts catalogFactsData
	if err := json.Unmarshal(catalogProfileJSON, &facts); err != nil {
		panic("invalid embedded catalogue profiles: " + err.Error())
	}
	return facts
}()
