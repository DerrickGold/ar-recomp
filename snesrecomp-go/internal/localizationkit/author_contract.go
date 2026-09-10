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

// AuthorPresentation is how the game presents a route, so the editor can
// refuse content the runtime will never display. "flow" paginates; "fixed"
// shows only the first page of a card, label or menu row; "keyboard" is the
// paged name-entry alphabet; "inline" is a term substituted into another
// message. A zero limit means the catalog does not constrain it.
type AuthorPresentation struct {
	Shape                 string `json:"shape"`
	MaximumPages          int    `json:"maximum_pages"`
	MaximumLines          int    `json:"maximum_lines"`
	RequiredNonemptyLines int    `json:"required_nonempty_lines"`
}

type authorRoute struct {
	Optional     bool                `json:"optional"`
	ID           string              `json:"id"`
	Allowed      []string            `json:"allowed_placeholders"`
	Canonical    string              `json:"canonical_profile"`
	Anchors      map[string][]string `json:"anchors"`
	Presentation AuthorPresentation  `json:"presentation"`
}

func (p AuthorPresentation) name() string {
	switch p.Shape {
	case "fixed":
		return "fixed field"
	case "keyboard":
		return "name-entry keyboard"
	case "inline":
		return "inline term"
	}
	return "dialogue"
}

// presentationScan counts authored shape the way the game reads it: a
// paragraph break advances two lines, a page break starts a page, and a line
// counts as content when it holds anything but spaces. Only lines that carry
// content count towards the field height, so the newline that ends the last
// authored line is not a second row.
type presentationScan struct {
	pages, line, linesUsed, nonempty int
	lineHasContent                   bool
}

func (s *presentationScan) content() {
	s.lineHasContent = true
	if s.line > s.linesUsed {
		s.linesUsed = s.line
	}
}

func (s *presentationScan) breakLines(advance int) {
	for i := 0; i < advance; i++ {
		if s.lineHasContent {
			s.nonempty++
		}
		s.lineHasContent = false
		s.line++
	}
}

func (s *presentationScan) text(value string) {
	for _, b := range []byte(value) {
		switch b {
		case '\n':
			s.breakLines(1)
		case ' ', '\t':
		default:
			s.content()
		}
	}
}

func (s *presentationScan) page() {
	if s.lineHasContent {
		s.nonempty++
	}
	s.lineHasContent = false
	s.pages++
	s.line = 1
}

func (s *presentationScan) operation(op AuthorOperation) {
	switch op.Op {
	case "text":
		s.text(op.Value)
	case "placeholder":
		s.content()
	case "line":
		s.breakLines(1)
	case "paragraph":
		s.breakLines(2)
	case "page":
		s.page()
	}
}

// check rejects content the game would silently never display: a page the
// fixed composer never advances to, a row past the native field, or a choice
// count the native menu cannot show.
func (s *presentationScan) check(p AuthorPresentation) error {
	if s.lineHasContent {
		s.nonempty++
	}
	s.lineHasContent = false
	if p.MaximumPages != 0 && s.pages > p.MaximumPages {
		return fmt.Errorf("this %s displays %d page(s); pages beyond that are never shown, so remove the extra page break(s)", p.name(), p.MaximumPages)
	}
	if p.MaximumLines != 0 && s.linesUsed > p.MaximumLines {
		return fmt.Errorf("this %s reserves %d line(s); the message has %d", p.name(), p.MaximumLines, s.linesUsed)
	}
	// A message with no content at all is the documented way to leave a route
	// to its native lettering; only a partly filled menu is a mistake.
	if p.RequiredNonemptyLines != 0 && s.nonempty != 0 && s.nonempty != p.RequiredNonemptyLines {
		return fmt.Errorf("this menu shows exactly %d choice(s); the message has %d", p.RequiredNonemptyLines, s.nonempty)
	}
	return nil
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
	// What the game does with this route, so an editor can size its field and
	// hide controls the runtime would ignore.
	Presentation AuthorPresentation `json:"presentation"`
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
		entry := AuthorReference{ID: route.ID, Anchors: append([]string{}, anchors...), NativeInProfile: native, Placeholders: []AuthorPlaceholder{}, Presentation: route.Presentation}
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
			if _, required := route.Anchors[profile]; required && !route.Optional {
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
		scan := presentationScan{pages: 1, line: 1}
		for _, op := range body.message.Operations {
			scan.operation(op)
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
		if err := scan.check(route.Presentation); err != nil {
			return zero, authorError(body.path, body.message.SourceLine, "%s: %s", id, err)
		}
	}
	return stats, nil
}
