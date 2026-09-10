package localization

import (
	"fmt"
	"slices"
)

// NativeCatalog is unfinalized single-ROM extraction evidence. Its whole-game
// flag stays false: CheckNativeCoverage returns the separate final reports
// after evaluating all five releases. Local ownership/graphics completeness
// alone does not certify an author-format export. No runtime loads this IR.
type NativeCatalog struct {
	WholeGameCoverageComplete bool                     `json:"whole_game_coverage_complete"`
	Inventory                 IRObject                 `json:"inventory"`
	Messages                  []*NativeMessage         `json:"messages"`
	PointerSets               []*NativePointerSet      `json:"pointer_sets"`
	Menu                      *NativeMenuCatalog       `json:"menu_source_catalog"`
	ReferenceSeeds            IRObject                 `json:"source_reference_seeds"`
	ReferenceResolution       IRObject                 `json:"source_reference_resolution"`
	SeedExpansion             IRObject                 `json:"source_seed_expansion"`
	DynamicText               IRObject                 `json:"dynamic_text_census"`
	Ownership                 IRObject                 `json:"language_source_ownership"`
	SemanticRoutes            *NativeSemanticCatalog   `json:"semantic_route_catalog"`
	Destinations              *NativeDestinationCensus `json:"destination_census,omitempty"`
	Graphics                  IRObject                 `json:"graphical_text_census,omitempty"`
	Credits                   []NativeCreditsPage      `json:"credits_text,omitempty"`
	releaseID                 string
	source                    *NativeSourceCensus
	locale, romSHA256         string
}

// BuildNativeCatalog discovers and decodes sources from the decoder's private,
// identified ROM. It returns nothing on an error and performs no file writes.
func (d *Decoder) BuildNativeCatalog() (*NativeCatalog, error) {
	if d == nil {
		return nil, fmt.Errorf("nil localization decoder")
	}
	var profile *catalogProfile
	for i := range catalogFacts.Profiles {
		if catalogFacts.Profiles[i].ID == d.profile.ID {
			profile = &catalogFacts.Profiles[i]
			break
		}
	}
	if profile == nil {
		return nil, fmt.Errorf("no catalogue profile for %q", d.profile.ID)
	}
	census, err := d.DiscoverNativeSources()
	if err != nil {
		return nil, err
	}
	destinationProfile, ok := destinationFacts.Profiles[d.profile.ID]
	if !ok {
		return nil, fmt.Errorf("no destination proof for %q", d.profile.ID)
	}
	entries, err := d.assetScript()
	if err != nil {
		return nil, err
	}
	destinations, err := d.destinationCensus(destinationProfile, sourceProfiles[d.profile.ID], entries)
	if err != nil {
		return nil, err
	}
	graphics, err := d.graphicalCensus(destinations, entries)
	if err != nil {
		return nil, err
	}
	census.destinations = destinations
	catalog, err := d.catalogFromSources(*profile, sourceProfiles[d.profile.ID], census)
	if err != nil {
		return nil, err
	}
	catalog.Destinations, catalog.Graphics = destinations, graphics
	catalog.Credits, err = d.nativeCredits(entries)
	if err != nil {
		return nil, err
	}
	return catalog, nil
}
func (d *Decoder) catalogFromSources(profile catalogProfile, source sourceProfile, census *NativeSourceCensus) (*NativeCatalog, error) {
	b := newCatalogBuilder(d, profile)
	inventory, err := b.structuredInventory()
	if err != nil {
		return nil, err
	}
	menu, err := d.fixedCatalog(source, census)
	if err != nil {
		return nil, err
	}
	resolution, expansion, err := b.expandSeeds(census, menu)
	if err != nil {
		return nil, err
	}
	records := append(append([]*NativeMessage{}, b.messages...), menu.Segments...)
	dynamic := b.dynamicCensus(records)
	ownership, err := b.localOwnership(census, menu, resolution)
	if err != nil {
		return nil, err
	}
	routes, err := b.semanticRoutes(census, menu)
	if err != nil {
		return nil, err
	}
	slices.SortStableFunc(b.messages, func(a, c *NativeMessage) int { return a.start - c.start })
	return &NativeCatalog{releaseID: profile.ID, source: census, locale: d.profile.Locale, romSHA256: d.profile.SHA256,
		Inventory: inventory, Messages: b.messages, PointerSets: b.pointers, Menu: menu,
		ReferenceSeeds: census.SourceReferenceSeeds, ReferenceResolution: resolution, SeedExpansion: expansion,
		DynamicText: dynamic, Ownership: ownership, SemanticRoutes: routes}, nil
}
