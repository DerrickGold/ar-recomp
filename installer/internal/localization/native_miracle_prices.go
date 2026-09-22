package localization

import (
	"fmt"
	"strconv"
	"strings"
)

// Retail prose embeds a literal SP price. Author packs use a live number so
// language and gameplay region can be selected independently. The lossless
// extraction IR remains unchanged; only the playable author representation is
// normalized. Limit substitution to an identified route and its one numeric
// field, never a global string replacement in arbitrary translator prose.
func nativeMiraclePrice(ops []AuthorOperation, id string) ([]AuthorOperation, error) {
	if !strings.HasPrefix(id, "sim.miracle.") || !strings.HasSuffix(id, ".insufficient_sp") {
		return ops, nil
	}
	name := strings.TrimSuffix(strings.TrimPrefix(id, "sim.miracle."), ".insufficient_sp")
	prices := map[string][2]int{"lightning": {10, 12}, "rain": {20, 16}, "sun": {30, 18}, "wind": {80, 24}, "earthquake": {160, 60}}
	price, ok := prices[name]
	if !ok {
		return nil, fmt.Errorf("unknown miracle price route %s", id)
	}
	if name == "sun" {
		name = "sunlight"
	}
	var out []AuthorOperation
	count := 0
	for _, op := range ops {
		if op.Op != "text" {
			out = append(out, op)
			continue
		}
		text := op.Value
		start := strings.IndexAny(text, "0123456789")
		if start < 0 {
			out = append(out, op)
			continue
		}
		end := start
		for end < len(text) && text[end] >= '0' && text[end] <= '9' {
			end++
		}
		number, err := strconv.Atoi(text[start:end])
		if err != nil || (number != price[0] && number != price[1]) || strings.ContainsAny(text[end:], "0123456789") {
			return nil, fmt.Errorf("unexpected numeric field in %s", id)
		}
		if start > 0 {
			out = append(out, AuthorOperation{Op: "text", Value: text[:start]})
		}
		out = append(out, AuthorOperation{Op: "placeholder", Name: "miracle_" + name + "_sp"})
		if end < len(text) {
			out = append(out, AuthorOperation{Op: "text", Value: text[end:]})
		}
		count++
	}
	if count != 1 {
		return nil, fmt.Errorf("expected one miracle price in %s, found %d", id, count)
	}
	return out, nil
}
