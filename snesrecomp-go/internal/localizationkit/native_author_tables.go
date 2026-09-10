package localizationkit

import "strings"

// Only verified native report roles interpret padding as columns. Authored
// translations use explicit cell separators; runtime never splits their words.
func nativeAuthorTable(ops []AuthorOperation, id string) []AuthorOperation {
	if id == "sound_test.menu.labels" {
		var out []AuthorOperation
		for _, op := range ops {
			if op.Op == "placeholder" && (op.Name == "sound_music_id" || op.Name == "sound_effect_id") {
				out = append(out, AuthorOperation{Op: "text", Value: " | "})
			}
			out = append(out, op)
		}
		return out
	}
	if id == "system.message_speed.scale_labels" {
		ops = append([]AuthorOperation{}, ops...)
		if len(ops) > 0 && ops[0].Op == "text" {
			digits := strings.TrimFunc(ops[0].Value, sourceSpace)
			if digits != "" && strings.Trim(digits, "0123456789") == "" {
				cells := make([]string, len(digits))
				for i := range digits {
					cells[i] = digits[i : i+1]
				}
				ops[0].Value = strings.Join(cells, " | ")
			}
		}
		var out []AuthorOperation
		for _, op := range ops {
			direction := op.Op == "placeholder" && op.Name == "icon.ui.speed_direction"
			if direction {
				out = append(out, AuthorOperation{Op: "text", Value: " | "})
			}
			out = append(out, op)
			if direction {
				out = append(out, AuthorOperation{Op: "text", Value: " | "})
			}
		}
		return out
	}
	if !strings.HasPrefix(id, "status.report.") {
		return ops
	}
	var result, row []AuthorOperation
	rowIndex := 0
	master, score := id == "status.report.master_report", id == "status.report.score_report"
	for _, op := range ops {
		if op.Op != "line" && op.Op != "end" {
			row = append(row, op)
			continue
		}
		split := !master && rowIndex != 2 && rowIndex != 5 || master && (rowIndex == 3 || rowIndex == 5 || rowIndex == 7 || rowIndex == 9)
		placeholder := false
		for _, cell := range row {
			placeholder = placeholder || cell.Op == "placeholder"
		}
		if rowIndex == 0 && !placeholder {
			split = false
		}
		minimum := 1
		if rowIndex == 0 || score && rowIndex == 3 {
			minimum = 2
		}
		if rowIndex == 1 && !master {
			minimum = 2
			for i := range row {
				if row[i].Op == "text" && strings.TrimFunc(row[i].Value, sourceSpace) != "" {
					row[i].Value = strings.Join(strings.FieldsFunc(row[i].Value, sourceSpace), " ") + "  "
				}
			}
		}
		cells := [][]AuthorOperation{{}}
		for _, item := range row {
			last := len(cells) - 1
			if !master && rowIndex <= 1 && item.Op == "placeholder" && (item.Name == "total_population" || item.Name == "total_score") && len(cells[last]) > 0 {
				tail := &cells[last][len(cells[last])-1]
				if tail.Op == "text" {
					tail.Value = strings.TrimRightFunc(tail.Value, sourceSpace)
				}
				cells = append(cells, nil)
				last++
			}
			if !split || item.Op != "text" {
				cells[last] = append(cells[last], item)
				continue
			}
			for _, part := range nativeColumnParts(item.Value, minimum) {
				if part == "" {
					if len(cells[len(cells)-1]) > 0 {
						cells = append(cells, nil)
					}
				} else {
					cells[len(cells)-1] = append(cells[len(cells)-1], AuthorOperation{Op: "text", Value: part})
				}
			}
		}
		written := false
		for _, cell := range cells {
			if len(cell) == 0 {
				continue
			}
			if written {
				result = append(result, AuthorOperation{Op: "text", Value: " | "})
			}
			result = append(result, cell...)
			written = true
		}
		result = append(result, op)
		row, rowIndex = nil, rowIndex+1
	}
	return result
}

// Empty entries mark separating whitespace runs. Single spaces within a
// multiword title are preserved when its native row requires two-cell padding.
func nativeColumnParts(text string, minimum int) []string {
	var parts []string
	start, run, count := 0, -1, 0
	flush := func(end int) {
		if count >= minimum {
			if run > start {
				parts = append(parts, text[start:run])
			}
			parts = append(parts, "")
			start = end
		}
		run, count = -1, 0
	}
	for i, c := range text {
		if sourceSpace(c) {
			if run < 0 {
				run = i
			}
			count++
		} else if run >= 0 {
			flush(i)
		}
	}
	if run >= 0 {
		flush(len(text))
	}
	if start < len(text) {
		parts = append(parts, text[start:])
	}
	return parts
}
