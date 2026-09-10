package localization

import (
	"encoding/json"
	"fmt"
	"slices"
	"strings"
	"unicode"
	"unicode/utf8"
)

// BuildNativeAuthorPack extracts a local-only source pack from this identified
// ROM. It does not translate to the US contract, extract fonts, publish, install
// or write files. Complete means all author routes for the identified release;
// graphical extraction and the five-release coverage audit remain separate.
// Regional sources must remain reference-only. No Python is invoked.
func (d *Decoder) BuildNativeAuthorPack(metadata PackMetadata) (*AuthorPack, error) {
	if d == nil {
		return nil, fmt.Errorf("nil localization decoder")
	}
	if metadata.SourceProfile != d.profile.ID || metadata.Coverage != "complete" {
		return nil, fmt.Errorf("native source metadata must use identified profile %s and complete coverage", d.profile.ID)
	}
	manifest, err := NewPackManifest(metadata, PackFonts{Primary: "builtin:actraiser-sans"}, []string{"text/source.artext"})
	if err != nil {
		return nil, err
	}
	catalog, err := d.BuildNativeCatalog()
	if err != nil {
		return nil, err
	}
	reports, err := CheckNativeCoverage(catalog)
	if err != nil {
		return nil, err
	}
	// A player's extractor requires one ROM, not all five. Only the explicitly
	// missing cross-release comparison may be deferred here; local gaps fail.
	for _, blocker := range irRows(reports[0], "blockers") {
		if irString(blocker, "id") != "cross_release_semantic_alignment" {
			return nil, fmt.Errorf("native extraction blocked: %s", irString(blocker, "id"))
		}
	}
	if !catalog.SemanticRoutes.Complete {
		return nil, fmt.Errorf("native routes are incomplete")
	}
	labels := append(nativeHUDLabels(d.profile.ID), nativeCreditsMessages(catalog.Credits)...)
	return nativeAuthorPack(manifest, catalog.SemanticRoutes.Routes, catalog.source.NativeDialogueLayout, labels...)
}

func nativeAuthorPack(manifest *PackManifest, routes []*NativeSemanticRoute, layout IRObject, supplemental ...AuthorMessage) (*AuthorPack, error) {
	messages, err := nativeAuthorMessages(routes, layout)
	if err != nil {
		return nil, err
	}
	messages = append(messages, supplemental...)
	slices.SortFunc(messages, func(a, b AuthorMessage) int { return strings.Compare(a.ID, b.ID) })
	script, err := EmitAuthorScript(messages, "text/source.artext")
	if err != nil {
		return nil, err
	}
	var progress strings.Builder
	progress.WriteString("# format=actraiser-language-progress version=1\n# semantic-id<TAB>not_started|wip|done\n")
	for _, message := range messages {
		fmt.Fprintf(&progress, "%s\tnot_started\n", message.ID)
	}
	w, err := newAuthorWorkspace(manifest.metadata.SourceProfile, manifest.metadata.Coverage, []*AuthorScript{script}, progress.String())
	if err != nil {
		return nil, err
	}
	p := &AuthorPack{manifest: manifest, workspace: w, fonts: map[string][]byte{}, progressPresent: true}
	if p.byteSize() > MaxAuthorPackBytes {
		return nil, fmt.Errorf("source pack exceeds size limit")
	}
	return p, nil
}

func nativeAuthorMessages(routes []*NativeSemanticRoute, layout IRObject) ([]AuthorMessage, error) {
	if len(routes) == 0 || len(routes) > MaxAuthorMessages {
		return nil, fmt.Errorf("invalid native route count")
	}
	ordered := append([]*NativeSemanticRoute{}, routes...)
	for _, route := range ordered {
		if route == nil || !authorIdentifier(route.ID) {
			return nil, fmt.Errorf("invalid native route")
		}
	}
	slices.SortFunc(ordered, func(a, b *NativeSemanticRoute) int { return strings.Compare(a.ID, b.ID) })
	canonical := map[string]string{}
	messages := make([]AuthorMessage, 0, len(ordered))
	for i, route := range ordered {
		if i > 0 && ordered[i-1].ID == route.ID {
			return nil, fmt.Errorf("duplicate native route %s", route.ID)
		}
		ops, err := nativeAuthorOperations(route, layout)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", route.ID, err)
		}
		// Alias after reflow/table conversion, never by native byte/IR identity.
		// Exact encoding avoids hash collisions and retains operation boundaries.
		key, err := json.Marshal(ops)
		if err != nil {
			return nil, err
		}
		m := AuthorMessage{ID: route.ID, Operations: []AuthorOperation{}}
		if id, found := canonical[string(key)]; found {
			m.Alias = id
		} else {
			canonical[string(key)] = route.ID
			if !slices.ContainsFunc(ops, func(op AuthorOperation) bool { return op.Op == "text" || op.Op == "placeholder" }) {
				ops = append([]AuthorOperation{{Op: "empty"}}, ops...)
			}
			m.Operations = ops
		}
		messages = append(messages, m)
	}
	return messages, nil
}

func nativeReflow(category string) bool {
	return slices.Contains([]string{"angel_dialogue", "angel_dialogue_native", "town_dialogue", "town_dialogue_native", "offering_text", "offering_text_native", "ending_text", "post_offering_or_ending_native", "dialogue_consumer_seed"}, category)
}

func sourceSpace(c rune) bool { return unicode.IsSpace(c) || c >= 0x1c && c <= 0x1f }

func collapseNativeSpaces(value string) string {
	var b strings.Builder
	space := false
	for _, c := range value {
		if sourceSpace(c) {
			if !space {
				b.WriteByte(' ')
			}
			space = true
		} else {
			b.WriteRune(c)
			space = false
		}
	}
	return b.String()
}

func nativeInteger(op Operation, key string) (int, error) {
	value, exists := op[key]
	if !exists {
		return 0, nil
	}
	n, ok := value.(int)
	if !ok || n < 0 || n > 255 {
		return 0, fmt.Errorf("invalid native %s", key)
	}
	return n, nil
}

func nativeAuthorOperations(route *NativeSemanticRoute, layout IRObject) ([]AuthorOperation, error) {
	if len(route.Operations) == 0 || len(route.Operations) > MaxAuthorOperations {
		return nil, fmt.Errorf("invalid native operation count")
	}
	// Validate fields before geometry reads; unresolved IR never becomes text.
	textBytes := 0
	for _, source := range route.Operations {
		kind, ok := source["op"].(string)
		if !ok {
			return nil, fmt.Errorf("missing native operation kind")
		}
		switch kind {
		case "text", "format_number", "insert_indexed_text", "insert_icon":
			value, ok := source["value"].(string)
			if !ok || !utf8.ValidString(value) || strings.ContainsRune(value, 0) {
				return nil, fmt.Errorf("unresolved native %s", kind)
			}
			if len(value) > MaxAuthorTextBytes-textBytes {
				return nil, fmt.Errorf("native text exceeds size limit")
			}
			textBytes += len(value)
			if kind != "text" && !authorIdentifier(value) {
				return nil, fmt.Errorf("invalid native value %s", kind)
			}
		case "insert_master_name", "line_break", "page_break", "reset_text_cursor", "toggle_text_state", "delay", "yield", "end":
		default:
			return nil, fmt.Errorf("unrepresentable native operation %q", kind)
		}
		for _, field := range []string{"width", "part_index", "part_count"} {
			if _, err := nativeInteger(source, field); err != nil {
				return nil, err
			}
		}
	}
	reflow := nativeReflow(route.Category)
	hard := nativeHardBreaks(route, layout)
	ops := make([]AuthorOperation, 0, len(route.Operations))
	anchors := 0
	for i, source := range route.Operations {
		kind := source["op"].(string)
		switch kind {
		case "text":
			value := source["value"].(string)
			if reflow {
				value = collapseNativeSpaces(value)
			}
			if value != "" {
				ops = append(ops, AuthorOperation{Op: "text", Value: value})
			}
		case "line_break":
			if reflow && !hard[i] {
				if len(ops) > 0 && (ops[len(ops)-1].Op == "text" || ops[len(ops)-1].Op == "placeholder") {
					ops = append(ops, AuthorOperation{Op: "text", Value: " "})
				}
			} else {
				ops = append(ops, AuthorOperation{Op: "line"})
			}
		case "page_break":
			ops = append(ops, AuthorOperation{Op: "page"})
		case "insert_master_name":
			ops = append(ops, AuthorOperation{Op: "placeholder", Name: "master_name"})
		case "format_number", "insert_indexed_text", "insert_icon":
			name := source["value"].(string)
			digits, _ := nativeInteger(source, "width")
			if kind != "format_number" || digits&0x80 != 0 {
				digits = 0
			}
			if kind == "insert_icon" {
				part, _ := nativeInteger(source, "part_index")
				if part != 0 {
					continue
				} // Adjacent atlas parts belong to one object.
				name = "icon." + name
			}
			ops = append(ops, AuthorOperation{Op: "placeholder", Name: name, MinimumDigits: digits})
		case "reset_text_cursor", "toggle_text_state", "delay", "yield":
			ops = append(ops, AuthorOperation{Op: "anchor", ID: fmt.Sprintf("%s.%02d", kind, anchors)})
			anchors++
		case "end":
			ops = append(ops, AuthorOperation{Op: "end"})
		}
	}
	if reflow {
		ops = authorPresentation(ops)
		first, last := -1, -1
		for i := range ops {
			if ops[i].Op == "text" {
				ops[i].Value = collapseNativeSpaces(ops[i].Value)
			}
			if ops[i].Op == "text" || ops[i].Op == "placeholder" {
				if first < 0 {
					first = i
				}
				last = i
			}
		}
		if first >= 0 && ops[first].Op == "text" {
			ops[first].Value = strings.TrimLeftFunc(ops[first].Value, sourceSpace)
		}
		if last >= 0 && ops[last].Op == "text" {
			ops[last].Value = strings.TrimRightFunc(ops[last].Value, sourceSpace)
		}
		ops = slices.DeleteFunc(ops, func(op AuthorOperation) bool { return op.Op == "text" && op.Value == "" })
	}
	if len(ops) > 0 && strings.HasPrefix(route.ID, "status.report.") && ops[len(ops)-1].Op != "end" && ops[len(ops)-1].Op != "line" {
		return nil, fmt.Errorf("unterminated native report row")
	}
	return stripNativePadding(nativeAuthorTable(ops, route.ID)), nil
}

// Blank native cell runs clear the retail tilemap; they are not a paragraph or
// authored prose. Only a whole inline run of ASCII padding is removed. Preserve
// every line/control and all spaces adjoining actual text or a placeholder.
func stripNativePadding(ops []AuthorOperation) []AuthorOperation {
	result := make([]AuthorOperation, 0, len(ops))
	for start := 0; start < len(ops); {
		end, padding := start, true
		for end < len(ops) && (ops[end].Op == "text" || ops[end].Op == "placeholder") {
			padding = padding && ops[end].Op == "text" && strings.Trim(ops[end].Value, " \t") == ""
			end++
		}
		if end == start {
			result = append(result, ops[start])
			start++
		} else {
			if !padding {
				result = append(result, ops[start:end]...)
			}
			start = end
		}
	}
	return result
}

// Native-cell analysis is not a Unicode font measurer. Variable lookups and
// unrecognized combining marks keep hard breaks. Known native dakuten/accents
// share their base cell. Japanese and unprofiled ending geometry never reflow.
func nativeHardBreaks(route *NativeSemanticRoute, layout IRObject) map[int]bool {
	hard := map[int]bool{}
	for i, op := range route.Operations {
		if op["op"] == "line_break" {
			hard[i] = true
		}
	}
	columns, ok := layout["columns"].(int)
	if !ok || columns < 1 || columns > 32 || layout["space_delimited_words"] != true || !nativeReflow(route.Category) || route.Category == "ending_text" || route.Category == "post_offering_or_ending_native" {
		return hard
	}
	units := func(ops []Operation) []int {
		var result []int // 0 separator; -1 unknown width; positive native cells.
		for _, op := range ops {
			switch op["op"] {
			case "text":
				for _, c := range op["value"].(string) {
					if sourceSpace(c) {
						if len(result) > 0 && result[len(result)-1] != 0 {
							result = append(result, 0)
						}
					} else if c == 0x3099 || c == 0x309a || c >= 0x300 && c <= 0x36f && c != 0x34f {
						// Native accents do not advance another tile cell.
					} else if unicode.IsMark(c) {
						result = append(result, -1)
					} else {
						result = append(result, 1)
					}
				}
			case "format_number":
				width, _ := nativeInteger(op, "width")
				if width > 0 && width < 10 {
					result = append(result, width)
				} else {
					result = append(result, -1)
				}
			case "insert_icon":
				result = append(result, 1)
			case "insert_master_name", "insert_indexed_text":
				result = append(result, -1)
			}
		}
		for len(result) > 0 && result[len(result)-1] == 0 {
			result = result[:len(result)-1]
		}
		return result
	}
	width := func(items []int) int {
		n := 0
		for _, item := range items {
			if item < 0 {
				return -1
			}
			n += max(1, item)
		}
		return n
	}
	boundary := func(op Operation) bool {
		return slices.Contains([]string{"line_break", "page_break", "reset_text_cursor", "yield", "end"}, irString(IRObject(op), "op"))
	}
	start := 0
	for i, op := range route.Operations {
		if op["op"] == "line_break" {
			end := i + 1
			for end < len(route.Operations) && !boundary(route.Operations[end]) {
				end++
			}
			current, next := units(route.Operations[start:i]), units(route.Operations[i+1:end])
			if space := slices.Index(next, 0); space >= 0 {
				next = next[:space]
			}
			left, right := width(current), width(next)
			if len(current) > 0 && len(next) > 0 && left >= 0 && right >= 0 && left+1+right > columns {
				delete(hard, i)
			}
		}
		if boundary(op) {
			start = i + 1
		}
	}
	return hard
}
