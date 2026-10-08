package localization

import (
	_ "embed"
	"encoding/json"
	"regexp"
	"slices"
	"strings"
)

//go:embed data/ability-names.json
var abilityNameData []byte

type abilityName struct {
	Placeholder string   `json:"placeholder"`
	MessageID   string   `json:"message_id"`
	NativeName  string   `json:"native_name"`
	Patterns    []string `json:"patterns"`
	Routes      []string `json:"routes"`
	pattern     *regexp.Regexp
}

var abilityNames = func() []abilityName {
	var rows []abilityName
	if err := json.Unmarshal(abilityNameData, &rows); err != nil {
		panic(err)
	}
	for i := range rows {
		rows[i].pattern = regexp.MustCompile(`(?i)\b(?:` + strings.Join(rows[i].Patterns, "|") + `)\b`)
	}
	return rows
}()

// AbilityNameSource identifies the existing menu label defining a shared name.
func AbilityNameSource(name string) string {
	for _, row := range abilityNames {
		if row.Placeholder == name {
			return row.MessageID
		}
	}
	return ""
}
func AbilityNameDefault(name string) string {
	for _, row := range abilityNames {
		if row.Placeholder == name {
			return row.NativeName
		}
	}
	return ""
}

// Native names are substituted only on audited consumer routes. Weather prose
// elsewhere remains ordinary text. Soft native wraps within a name may vanish;
// gameplay controls, hard breaks, styling and all surrounding text stay intact.
func nativeAbilityReferences(ops []AuthorOperation, id string) []AuthorOperation {
	route, ok := authorContracts.routes[id]
	if !ok {
		return ops
	}
	var names []abilityName
	for _, row := range abilityNames {
		if slices.Contains(route.Allowed, row.Placeholder) {
			names = append(names, row)
		}
	}
	if len(names) == 0 {
		return ops
	}
	type part struct {
		op         AuthorOperation
		start, end int
	}
	type match struct {
		start, end int
		name       string
	}
	var out []AuthorOperation
	for i := 0; i < len(ops); {
		if ops[i].Op != "text" && ops[i].Op != "preferred_line" {
			out = append(out, ops[i])
			i++
			continue
		}
		begin := i
		var text strings.Builder
		var parts []part
		for i < len(ops) && (ops[i].Op == "text" || ops[i].Op == "preferred_line") {
			a := text.Len()
			if ops[i].Op == "text" {
				text.WriteString(ops[i].Value)
			} else {
				text.WriteByte(' ')
			}
			parts = append(parts, part{ops[i], a, text.Len()})
			i++
		}
		run := text.String()
		var matches []match
		for _, name := range names {
			for _, at := range name.pattern.FindAllStringIndex(run, -1) {
				matches = append(matches, match{at[0], at[1], name.Placeholder})
			}
		}
		if len(matches) == 0 {
			out = append(out, ops[begin:i]...)
			continue
		}
		slices.SortFunc(matches, func(a, b match) int { return a.start - b.start })
		appendRange := func(a, b int) {
			for _, p := range parts {
				first, last := max(a, p.start), min(b, p.end)
				if first >= last {
					continue
				}
				op := p.op
				if op.Op == "text" {
					op.Value = run[first:last]
				}
				out = append(out, op)
			}
		}
		at := 0
		for _, m := range matches {
			if m.start < at {
				continue
			}
			appendRange(at, m.start)
			var value AuthorOperation
			for _, p := range parts {
				if p.start <= m.start && p.end > m.start {
					value = p.op
					break
				}
			}
			value.Op = "placeholder"
			value.Value = ""
			value.Name = m.name
			out = append(out, value)
			at = m.end
		}
		appendRange(at, len(run))
	}
	return out
}
