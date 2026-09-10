package localizationkit

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"slices"
	"strings"
)

// Generated from the same ROM-free semantic registry as the C runtime.
//
//go:embed data/author-contracts.json
var authorContractJSON []byte

type authorRoute struct {
	ID        string              `json:"id"`
	Allowed   []string            `json:"allowed_placeholders"`
	Canonical string              `json:"canonical_profile"`
	Anchors   map[string][]string `json:"anchors"`
}

type authorContractRegistry struct {
	placeholders map[string]string
	routes       map[string]authorRoute
	ordered      []authorRoute
}

var authorContracts = func() authorContractRegistry {
	var data struct {
		Placeholders map[string]string `json:"placeholders"`
		Routes       []authorRoute     `json:"routes"`
	}
	if err := json.Unmarshal(authorContractJSON, &data); err != nil {
		panic("invalid author contracts: " + err.Error())
	}
	routes := make(map[string]authorRoute, len(data.Routes))
	for _, route := range data.Routes {
		routes[route.ID] = route
	}
	return authorContractRegistry{data.Placeholders, routes, data.Routes}
}()

func authorProfile(profile string) bool {
	return slices.Contains([]string{"us", "eu-en", "de", "fr", "jp"}, profile)
}

type AuthorPlaceholder struct {
	Name string `json:"name"`
	Kind string `json:"kind"`
}

type AuthorReference struct {
	ID              string              `json:"id"`
	Placeholders    []AuthorPlaceholder `json:"placeholders"`
	Anchors         []string            `json:"anchors"`
	NativeInProfile bool                `json:"native_in_profile"`
}

// AuthorReferences supplies the editor's tree and contextual pickers. Results
// are detached copies; callers cannot modify the shared semantic registry.
func AuthorReferences(profile string) ([]AuthorReference, error) {
	if !authorProfile(profile) {
		return nil, fmt.Errorf("unsupported author source profile %q", profile)
	}
	result := make([]AuthorReference, 0, len(authorContracts.ordered))
	for _, route := range authorContracts.ordered {
		anchors, native := route.Anchors[profile]
		if !native {
			anchors = route.Anchors[route.Canonical]
		}
		entry := AuthorReference{ID: route.ID, Anchors: append([]string{}, anchors...), NativeInProfile: native, Placeholders: []AuthorPlaceholder{}}
		for _, name := range route.Allowed {
			entry.Placeholders = append(entry.Placeholders, AuthorPlaceholder{name, authorContracts.placeholders[name]})
		}
		result = append(result, entry)
	}
	return result, nil
}

type AuthorValidationStats struct {
	MessageCount   int `json:"message_count"`
	AliasCount     int `json:"alias_count"`
	OperationCount int `json:"operation_count"`
}

// ValidateAuthorScripts validates one pack, including cross-file aliases and
// reverse dependents of edited messages. Syntax/size checks belong to parsing;
// this pass checks runtime contracts without ROM access, I/O, or GUI defaults.
// It does not prove font coverage, keyboard geometry or runtime layout budgets.
func ValidateAuthorScripts(profile, coverage string, scripts ...*AuthorScript) (AuthorValidationStats, error) {
	var zero AuthorValidationStats
	if !authorProfile(profile) {
		return zero, fmt.Errorf("unsupported author source profile %q", profile)
	}
	if coverage != "partial" && coverage != "complete" {
		return zero, fmt.Errorf("coverage must be partial or complete")
	}
	if len(scripts) == 0 || len(scripts) > 64 {
		return zero, fmt.Errorf("pack requires 1-64 script sources")
	}
	type located struct {
		message *AuthorMessage
		path    string
	}
	index := make(map[string]located)
	var order []string
	stats := AuthorValidationStats{}
	for _, script := range scripts {
		if script == nil || len(script.messages) == 0 {
			return zero, fmt.Errorf("pack contains an unparsed script")
		}
		for i := range script.messages {
			message := &script.messages[i]
			if _, ok := index[message.ID]; ok {
				return zero, authorError(script.path, message.SourceLine, "duplicate message %q", message.ID)
			}
			if _, ok := authorContracts.routes[message.ID]; !ok {
				return zero, authorError(script.path, message.SourceLine, "unknown semantic message %q", message.ID)
			}
			index[message.ID] = located{message, script.path}
			order = append(order, message.ID)
			stats.OperationCount += len(message.Operations)
			if message.Alias != "" {
				stats.AliasCount++
			}
		}
	}
	stats.MessageCount = len(index)
	if len(index) > MaxAuthorMessages {
		return zero, fmt.Errorf("pack has too many messages")
	}
	if coverage == "complete" {
		for _, route := range authorContracts.ordered {
			if _, required := route.Anchors[profile]; required {
				if _, present := index[route.ID]; !present {
					return zero, fmt.Errorf("complete pack is missing message %s", route.ID)
				}
			}
		}
	}
	// Iterative, memoized graph walk: long alias chains cannot exhaust the stack
	// or turn each keystroke into repeated quadratic graph searches.
	resolved := make(map[string]located, len(index))
	for _, id := range order {
		if _, done := resolved[id]; done {
			continue
		}
		var chain []string
		seen := make(map[string]bool)
		cursor := id
		var body located
		for {
			if value, done := resolved[cursor]; done {
				body = value
				break
			}
			if seen[cursor] {
				return zero, authorError(index[id].path, index[id].message.SourceLine, "%s: alias cycle detected", id)
			}
			seen[cursor] = true
			value, ok := index[cursor]
			if !ok {
				return zero, authorError(index[id].path, index[id].message.SourceLine, "%s: alias target %q is not included in this pack", id, cursor)
			}
			chain = append(chain, cursor)
			if value.message.Alias == "" {
				body = value
				break
			}
			cursor = value.message.Alias
		}
		for _, member := range chain {
			resolved[member] = body
		}
	}
	for _, id := range order {
		route := authorContracts.routes[id]
		anchors, ok := route.Anchors[profile]
		if !ok {
			anchors = route.Anchors[route.Canonical]
		}
		anchorIndex, yielded := 0, false
		body := resolved[id]
		for _, op := range body.message.Operations {
			fail := func(format string, args ...any) (AuthorValidationStats, error) {
				return zero, authorError(body.path, op.SourceLine, id+": "+format, args...)
			}
			if yielded && op.Op != "end" && op.Op != "empty" {
				return fail("content after a menu yield is unreachable; place it before the yield anchor")
			}
			switch op.Op {
			case "placeholder":
				if !slices.Contains(route.Allowed, op.Name) {
					return fail("placeholder {%s} is unavailable on this route", op.Name)
				}
				if op.MinimumDigits != 0 && authorContracts.placeholders[op.Name] != "number" {
					return fail("number format requires a numeric placeholder")
				}
			case "anchor":
				if anchorIndex >= len(anchors) || op.ID != anchors[anchorIndex] {
					return fail("locked anchors changed at anchor %d", anchorIndex)
				}
				anchorIndex++
				yielded = strings.HasPrefix(op.ID, "yield.")
			case "event":
				return fail("event %q is not allow-listed", op.ID)
			}
		}
		if anchorIndex != len(anchors) {
			return zero, authorError(index[id].path, index[id].message.SourceLine, "%s: locked anchors changed; expected %d, found %d", id, len(anchors), anchorIndex)
		}
	}
	return stats, nil
}
