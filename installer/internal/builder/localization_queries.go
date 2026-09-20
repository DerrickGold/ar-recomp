package builder

import (
	"fmt"
	"net/http"
	"slices"
	"strconv"
	"strings"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

func (work *localizationWork) localizationState() map[string]any {
	state := map[string]any{"project": nil, "root": work.root}
	if work.current != nil {
		p := work.current
		state["project"] = map[string]any{"formatVersion": p.Pack().Manifest().Version(), "metadata": p.Pack().Manifest().Metadata(), "revision": p.ProjectRevision(), "origin": p.Origin(), "notes": p.Notes(), "notices": p.Notices(), "fonts": p.Pack().Manifest().Fonts(), "treatments": p.Pack().Treatments(), "totals": p.Pack().Workspace().Children("")}
	}
	if work.reference != nil {
		state["reference"] = work.reference.Pack().Manifest().Metadata()
	}
	state["sourceAvailable"] = work.nativeLocalizationSource() != nil
	return state
}

func (work *localizationWork) localizationReference(id string) *lk.AuthorMessageView {
	view, _ := work.localizationReferenceWithMetadata(id)
	return view
}

func (work *localizationWork) localizationReferenceWithMetadata(id string) (*lk.AuthorMessageView, *lk.PackMetadata) {
	for _, p := range []*lk.AuthorProject{work.reference, work.nativeLocalizationSource()} {
		if p != nil {
			if v, ok := p.Pack().Workspace().Message(id); ok && v.Present {
				metadata := p.Pack().Manifest().Metadata()
				return &v, &metadata
			}
		}
	}
	return nil, nil
}

func (work *localizationWork) locationTree(parent string) []lk.AuthorTreeEntry {
	w := work.current.Pack().Workspace()
	refs, _ := lk.AuthorReferences(work.current.Pack().Manifest().Metadata().SourceProfile)
	groups := map[string]*lk.AuthorTreeEntry{}
	parents := map[string]string{}
	order := map[string]int{}
	rows := []lk.AuthorTreeEntry{}
	accumulate := func(id, label, key, parent string, row lk.AuthorTreeEntry) {
		if groups[id] == nil {
			groups[id] = &lk.AuthorTreeEntry{ID: id, Label: label, LabelKey: key, HasChildren: true}
			parents[id] = parent
		}
		g := groups[id]
		g.Total += row.Total
		g.Present += row.Present
		g.Done += row.Done
		g.WIP += row.WIP
		g.NotStarted += row.NotStarted
	}
	for _, ref := range refs {
		v, _ := w.Message(ref.ID)
		row := lk.AuthorTreeEntry{ID: ref.ID, IsMessage: true, Total: 1}
		if v.Present {
			row.Present = 1
		}
		switch v.Status {
		case lk.TranslationDone:
			row.Done = 1
		case lk.TranslationWIP:
			row.WIP = 1
		default:
			row.NotStarted = 1
		}
		locations := lk.AuthorMessageLocations(ref.ID)
		for _, loc := range locations {
			order[loc.Group] = loc.CategoryOrder
			if parent == loc.Group {
				row.Label = work.localizationMessageTitle(ref.ID, loc)
				row.LabelKey = loc.TitleKey
				row.Shared = loc.Shared
				rows = append(rows, row)
			}
			accumulate(loc.Root, loc.RootLabel, loc.RootKey, "", row)
			accumulate(loc.Group, loc.GroupLabel, loc.GroupKey, loc.Root, row)
		}
	}
	if parent == "" {
		for _, root := range lk.AuthorLocationRoots() {
			if group := groups["place."+root.ID]; group != nil {
				rows = append(rows, *group)
			}
		}
		return rows
	}
	for id, group := range groups {
		if parents[id] == parent {
			rows = append(rows, *group)
		}
	}
	slices.SortFunc(rows, func(a, b lk.AuthorTreeEntry) int {
		if a.IsMessage && b.IsMessage {
			return strings.Compare(a.ID, b.ID) // Preserve source-slot story order.
		}
		if delta := order[a.ID] - order[b.ID]; delta != 0 {
			return delta
		}
		if cmp := strings.Compare(a.Label, b.Label); cmp != 0 {
			return cmp
		}
		return strings.Compare(a.ID, b.ID)
	})
	return rows
}

// Opaque ROM-wrapper/slot names are useful IDs, not useful translator labels.
// Resolve only requested labels through the shared parser; keep prose out of
// collapsed groups and retain the exact semantic ID in the editor tooltip.
func (work *localizationWork) localizationMessageTitle(id string, location lk.AuthorLocation) string {
	if location.TitleKey != "" || !(strings.Contains(id, ".wrapper_") || strings.Contains(id, ".slot_")) {
		return location.Title
	}
	for _, project := range []*lk.AuthorProject{work.nativeLocalizationSource(), work.current} {
		if project == nil {
			continue
		}
		ops, err := project.Pack().MessageOperations(id)
		if err != nil {
			continue
		}
		var text strings.Builder
		for _, op := range ops {
			if op.Op == "text" {
				text.WriteString(op.Value)
			} else if op.Op == "line" || op.Op == "preferred_line" || op.Op == "paragraph" {
				text.WriteByte(' ')
			} else if op.Op == "placeholder" {
				text.WriteString("{" + op.Name + "}")
			}
			if text.Len() >= 90 || op.Op == "page" {
				break
			}
		}
		snippet := []rune(strings.Join(strings.Fields(text.String()), " "))
		if len(snippet) > 72 {
			snippet = append(snippet[:72], '…')
		}
		if len(snippet) > 0 {
			return string(snippet)
		}
	}
	return location.Title
}

func (work *localizationWork) readLocalization(w *localizationReply, r *http.Request, endpoint string) error {
	switch endpoint {
	case "state":
		work.refreshNativeLocalizationSource()
		w.json(200, work.localizationState())
		return nil
	case "projects":
		rows, err := work.store.List()
		if err != nil {
			return err
		}
		w.json(200, rows)
		return nil
	case "catalog":
		rows, err := work.localizationCatalog()
		if err != nil {
			return err
		}
		w.json(200, rows)
		return nil
	}
	if work.current == nil {
		return fmt.Errorf("open or create a project first")
	}
	if r.URL.Query().Get("projectID") != work.current.Pack().Manifest().Metadata().ID || r.URL.Query().Get("revision") != work.current.ProjectRevision() {
		return fmt.Errorf("%w: project changed; reopen before continuing", lk.ErrProjectConflict)
	}
	workspace := work.current.Pack().Workspace()
	switch endpoint {
	case "tree":
		if r.URL.Query().Get("view") == "locations" {
			w.json(200, work.locationTree(r.URL.Query().Get("parent")))
			return nil
		}
		w.json(200, workspace.Children(r.URL.Query().Get("parent")))
	case "message":
		id := r.URL.Query().Get("id")
		view, ok := workspace.Message(id)
		if !ok {
			return fmt.Errorf("unknown message")
		}
		location := lk.AuthorMessageLocation(id)
		location.Title = work.localizationMessageTitle(id, location)
		result := map[string]any{"message": view, "location": location}
		if ref, metadata := work.localizationReferenceWithMetadata(id); ref != nil {
			result["reference"] = ref
			result["referenceMetadata"] = metadata
		}
		w.json(200, result)
	case "search":
		q := r.URL.Query()
		needle, status := strings.ToLower(q.Get("q")), q.Get("status")
		if len(needle) > 256 {
			return fmt.Errorf("search is too long")
		}
		offset, _ := strconv.Atoi(q.Get("offset"))
		if offset < 0 {
			return fmt.Errorf("invalid result offset")
		}
		refs, _ := lk.AuthorReferences(work.current.Pack().Manifest().Metadata().SourceProfile)
		rows := []map[string]any{}
		total := 0
		for _, ref := range refs {
			v, _ := workspace.Message(ref.ID)
			loc := lk.AuthorMessageLocation(ref.ID)
			if status != "" && string(v.Status) != status {
				continue
			}
			if needle != "" {
				haystack := ref.ID + " " + v.Body
				for _, place := range lk.AuthorMessageLocations(ref.ID) {
					haystack += " " + place.Title + " " + place.RootLabel + " " + place.GroupLabel + " " + place.Context
					// Search built-in navigation in every UI language. No query or
					// project locale changes the route IDs, results or source text.
					for _, key := range []string{place.RootKey, place.GroupKey, place.TitleKey, place.ContextKey} {
						haystack += " " + navigationSearchText(key)
					}
				}
				if source := work.localizationReference(ref.ID); source != nil {
					haystack += " " + source.Body
				}
				if !strings.Contains(strings.ToLower(haystack), needle) {
					continue
				}
			}
			if total >= offset && len(rows) < 60 {
				rows = append(rows, map[string]any{"id": ref.ID, "title": work.localizationMessageTitle(ref.ID, loc), "label_key": loc.TitleKey, "shared": loc.Shared, "group": loc.GroupLabel, "status": v.Status, "present": v.Present})
			}
			total++
		}
		w.json(200, map[string]any{"rows": rows, "total": total, "offset": offset})
	default:
		return fmt.Errorf("unknown localization endpoint")
	}
	return nil
}
