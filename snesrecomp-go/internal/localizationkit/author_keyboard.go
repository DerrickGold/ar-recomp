package localizationkit

import (
	"fmt"
	"strings"
)

type AuthorKeyboardShape struct {
	Rows             int `json:"rows"`
	Columns          int `json:"columns"`
	MaximumLines     int `json:"maximum_lines"`
	MaximumPageBytes int `json:"maximum_page_bytes"`
}

type keyboardPage struct {
	text         []byte
	pendingSpace bool
	markers      [3]int // name/backspace/finish offsets + 1; zero means absent
}

func (p *keyboardPage) append(b byte, limit int) bool {
	if b == ' ' || b == '\t' || b == '\r' {
		p.pendingSpace = len(p.text) > 0 && p.text[len(p.text)-1] != '\n'
		return true
	}
	if b == '\n' {
		p.pendingSpace = false
		if len(p.text) == 0 || p.text[len(p.text)-1] == '\n' {
			return true
		}
	} else if p.pendingSpace {
		if len(p.text) == limit {
			return false
		}
		p.text = append(p.text, ' ')
		p.pendingSpace = false
	}
	if len(p.text) == limit {
		return false
	}
	p.text = append(p.text, b)
	return true
}

func keyboardKeys(text string, start, end int) ([]int, bool) {
	var offsets []int
	for offset := start; offset < end; {
		next, ok := nextGrapheme(text, offset)
		if !ok || next > end {
			return nil, false
		}
		if next != offset+1 || text[offset] != ' ' && text[offset] != '\t' {
			if next < end && text[next] != ' ' && text[next] != '\t' {
				return nil, false
			}
			offsets = append(offsets, offset)
		}
		offset = next
	}
	return offsets, true
}

func (p *keyboardPage) check(shape *AuthorKeyboardShape, index int, allowEmpty bool) error {
	text := strings.TrimRight(string(p.text), "\n")
	if text == "" && allowEmpty {
		return nil
	}
	lines := strings.Split(text, "\n")
	fail := func(reason string) error { return fmt.Errorf("keyboard page %d: %s", index, reason) }
	if len(lines) < shape.Rows+2 || len(lines) > shape.MaximumLines {
		return fail("requires a name field, underline slot and five keyboard rows (within the documented line limit)")
	}
	starts := make([]int, len(lines))
	for i := 1; i < len(lines); i++ {
		starts[i] = starts[i-1] + len(lines[i-1]) + 1
	}
	first := len(lines) - shape.Rows
	if p.markers[0] == 0 || p.markers[0]-1 != starts[first-2] || len(lines[first-2]) != 3 {
		return fail("put {master_name} alone on the line above the underline slot")
	}
	if strings.Trim(lines[first-1], "-") != "" {
		return fail("keep a dash-only underline slot immediately before the keys")
	}
	var lastKeys []int
	for row := 0; row < shape.Rows; row++ {
		line := first + row
		keys, ok := keyboardKeys(text, starts[line], starts[line]+len(lines[line]))
		if !ok || len(keys) != shape.Columns {
			return fmt.Errorf("keyboard page %d row %d requires exactly %d grapheme keys separated by ASCII spaces", index, row+1, shape.Columns)
		}
		lastKeys = keys
	}
	for action := 1; action <= 2; action++ {
		start := lastKeys[shape.Columns-3+action]
		end, ok := nextGrapheme(text, start)
		if !ok || p.markers[action] != start+1 || end != start+3 {
			return fail("keep {icon.name_entry.backspace} and {icon.name_entry.finish} in the final two key positions")
		}
	}
	return nil
}

func validateAuthorKeyboard(shape *AuthorKeyboardShape, ops []AuthorOperation) error {
	if shape == nil {
		return nil
	}
	p := keyboardPage{text: make([]byte, 0, shape.MaximumPageBytes)}
	index := 1
	for _, op := range ops {
		appended := true
		switch op.Op {
		case "text":
			for i := 0; i < len(op.Value); i++ {
				if !p.append(op.Value[i], shape.MaximumPageBytes) {
					appended = false
					break
				}
			}
		case "line", "paragraph":
			appended = p.append('\n', shape.MaximumPageBytes)
		case "placeholder":
			marker := 2
			if op.Name == "master_name" {
				marker = 0
			} else if op.Name == "icon.name_entry.backspace" {
				marker = 1
			}
			if p.markers[marker] != 0 {
				return fmt.Errorf("keyboard page %d repeats {%s}", index, op.Name)
			}
			start := len(p.text)
			if p.pendingSpace {
				start++
			}
			appended = p.append(0xef, shape.MaximumPageBytes) && p.append(0xbf, shape.MaximumPageBytes) && p.append(0xbc, shape.MaximumPageBytes)
			p.markers[marker] = start + 1
		case "page":
			if err := p.check(shape, index, false); err != nil {
				return err
			}
			index++
			p = keyboardPage{text: p.text[:0]}
		}
		if !appended {
			return fmt.Errorf("keyboard page %d exceeds %d normalized UTF-8 bytes", index, shape.MaximumPageBytes)
		}
	}
	return p.check(shape, index, index == 1)
}
