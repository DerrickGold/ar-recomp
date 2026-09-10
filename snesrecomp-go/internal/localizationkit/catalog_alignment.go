package localizationkit

import (
	"fmt"
	"slices"
)

type NativeRouteAvailability struct {
	ID           string   `json:"semantic_route_id"`
	Releases     []string `json:"available_releases"`
	Availability string   `json:"availability"`
}

// NativeRouteUnion joins logical invocation IDs, never record ordinals or
// equal text. Complete describes route alignment only, not whole-ROM coverage.
type NativeRouteUnion struct {
	Status          string                    `json:"status"`
	Complete        bool                      `json:"complete"`
	IdentityUnit    string                    `json:"identity_unit"`
	Expected        []string                  `json:"expected_releases"`
	Present         []string                  `json:"present_releases"`
	Missing         []string                  `json:"missing_releases"`
	RouteCount      int                       `json:"semantic_route_count"`
	AllReleaseCount int                       `json:"all_release_route_count"`
	VariantCount    int                       `json:"regional_or_release_variant_route_count"`
	Routes          []NativeRouteAvailability `json:"routes"`
}

// AlignNativeCatalogs makes a deterministic diagnostic union without changing
// its inputs. Missing releases produce an incomplete report; invalid/duplicate
// inputs return no report. Release identity comes from extraction, not a
// caller-supplied locale that could accidentally relabel one ROM as another.
func AlignNativeCatalogs(catalogs ...*NativeCatalog) (*NativeRouteUnion, error) {
	expected := []string{"us", "eu-en", "de", "fr", "jp"}
	byRelease := map[string]*NativeCatalog{}
	for _, catalog := range catalogs {
		if catalog == nil || catalog.SemanticRoutes == nil || !slices.Contains(expected, catalog.releaseID) {
			return nil, fmt.Errorf("route alignment requires an identified native catalogue")
		}
		if byRelease[catalog.releaseID] != nil {
			return nil, fmt.Errorf("duplicate catalogue release %q", catalog.releaseID)
		}
		byRelease[catalog.releaseID] = catalog
	}
	result := &NativeRouteUnion{Expected: expected, Present: []string{}, Missing: []string{}, Routes: []NativeRouteAvailability{},
		Complete: true, IdentityUnit: "logical_consumer_route_not_physical_record_ordinal"}
	releases := map[string][]string{}
	for _, id := range expected {
		catalog := byRelease[id]
		if catalog == nil {
			result.Missing = append(result.Missing, id)
			result.Complete = false
			continue
		}
		result.Present = append(result.Present, id)
		result.Complete = result.Complete && catalog.SemanticRoutes.Complete
		seen := map[string]bool{}
		for _, route := range catalog.SemanticRoutes.Routes {
			if route == nil || route.ID == "" || seen[route.ID] {
				return nil, fmt.Errorf("%s: missing or duplicate local route identity", id)
			}
			seen[route.ID] = true
			releases[route.ID] = append(releases[route.ID], id)
		}
	}
	ids := make([]string, 0, len(releases))
	for id := range releases {
		ids = append(ids, id)
	}
	slices.Sort(ids)
	for _, id := range ids {
		availability := "regional_or_release_variant"
		if len(releases[id]) == len(expected) {
			availability = "all_supported_releases"
			result.AllReleaseCount++
		}
		result.Routes = append(result.Routes, NativeRouteAvailability{id, releases[id], availability})
	}
	result.RouteCount, result.VariantCount = len(ids), len(ids)-result.AllReleaseCount
	result.Status = "incomplete_logical_route_union"
	if result.Complete {
		result.Status = "complete_logical_route_union"
	}
	return result, nil
}
