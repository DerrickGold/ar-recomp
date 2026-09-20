// Package texttemplate implements the game-independent inline template grammar.
// Values remain typed references here; substituting a value never parses it as
// markup. Game catalogs decide which references and presentation roles exist.
package texttemplate

import (
	"fmt"
	"strconv"
	"strings"
	"unicode/utf8"
)

const (
	MaximumDepth     = 16
	MaximumRuns      = 4096
	MaximumBytes     = 256 << 10
	MaximumRoleBytes = 96
	MinimumScale     = 25
	MaximumScale     = 400
)

// Style is an inline override, not a complete appearance. Zero values inherit
// the message defaults. Nested scales replace the enclosing inline scale;
// neither parsing nor nesting multiplies them. Italic is 0=inherited, 1=upright,
// 2=italic, so an explicit upright span is distinguishable from no override.
type Style struct {
	Font      string `json:"font,omitempty"`
	Treatment string `json:"style,omitempty"`
	Color     string `json:"color,omitempty"`
	Scale     int    `json:"scale,omitempty"`
	Italic    int    `json:"italic,omitempty"`
}

type Run struct {
	Text          string `json:"text,omitempty"`
	Value         string `json:"value,omitempty"`
	MinimumDigits int    `json:"minimum_digits,omitempty"`
	Style         Style  `json:"style"`
	SourceOffset  int    `json:"source_offset"`
}

type Error struct {
	Offset  int
	Message string
}

func (e *Error) Error() string { return fmt.Sprintf("byte %d: %s", e.Offset+1, e.Message) }

func Identifier(s string) bool {
	letter := func(c byte) bool { return c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' }
	if s == "" || !letter(s[0]) {
		return false
	}
	for i := 1; i < len(s); i++ {
		c := s[i]
		if !letter(c) && !(c >= '0' && c <= '9') && c != '_' && c != '-' && c != '.' {
			return false
		}
	}
	return true
}

// SetProperty validates one explicit property. A failed change leaves style
// unchanged. This also serves message defaults and the authoring UI.
func SetProperty(style *Style, name, value string) error {
	if len(value) > MaximumRoleBytes {
		return fmt.Errorf("style attribute is too long")
	}
	switch name {
	case "font", "style":
		if len(value) > MaximumRoleBytes || !Identifier(value) {
			return fmt.Errorf("%s requires a stable role identifier", name)
		}
		if name == "font" {
			style.Font = value
		} else {
			style.Treatment = value
		}
	case "color":
		if len(value) != 7 || value[0] != '#' {
			return fmt.Errorf("color must be #RRGGBB")
		}
		if _, err := strconv.ParseUint(value[1:], 16, 24); err != nil {
			return fmt.Errorf("color must be #RRGGBB")
		}
		style.Color = strings.ToUpper(value)
	case "scale":
		digits := strings.TrimSuffix(value, "%")
		if digits == value || digits == "" || strings.Trim(digits, "0123456789") != "" {
			return fmt.Errorf("scale must be an integer percentage")
		}
		n, err := strconv.Atoi(digits)
		if err != nil || n < MinimumScale || n > MaximumScale {
			return fmt.Errorf("scale must be %d%% through %d%%", MinimumScale, MaximumScale)
		}
		style.Scale = n
	case "italic":
		if value != "true" && value != "false" {
			return fmt.Errorf("italic must be true or false")
		}
		style.Italic = 1
		if value == "true" {
			style.Italic = 2
		}
	default:
		return fmt.Errorf("unknown style property %q", name)
	}
	return nil
}

type scope struct {
	tag   string
	style Style
}

// ParseInline parses one structurally bounded text segment. Physical soft line
// joins are performed by the script reader; explicit line/page/cell boundaries
// are outside this function. Every tag must close within the segment.
func ParseInline(input string) ([]Run, error) {
	if len(input) > MaximumBytes {
		return nil, &Error{0, "inline text exceeds size limit"}
	}
	if !utf8.ValidString(input) || strings.ContainsAny(input, "\x00\r\n") {
		return nil, &Error{0, "inline text must be one UTF-8 segment"}
	}
	p := inlineParser{input: input, scopes: []scope{{}}, runs: []Run{}}
	for p.at < len(input) {
		start := p.at
		switch input[p.at] {
		case '<':
			if err := p.tag(); err != nil {
				return nil, err
			}
		case '{', '}':
			if p.at+1 < len(input) && input[p.at+1] == input[p.at] {
				p.at += 2
				if err := p.text(input[start:start+1], start); err != nil {
					return nil, err
				}
			} else if err := p.value(); err != nil {
				return nil, err
			}
		case '\\':
			p.at++
			if p.at == len(input) || input[p.at] != '<' && input[p.at] != '\\' {
				return nil, p.fail(start, "expected \\< or \\\\ escape")
			}
			p.at++
			if err := p.text(input[start+1:p.at], start); err != nil {
				return nil, err
			}
		default:
			for p.at < len(input) && !strings.ContainsRune("<{}\\", rune(input[p.at])) {
				if input[p.at] == '|' && len(p.scopes) != 1 {
					return nil, p.fail(p.at, "close style tags before a cell separator")
				}
				p.at++
			}
			if err := p.text(input[start:p.at], start); err != nil {
				return nil, err
			}
		}
	}
	if len(p.scopes) != 1 {
		return nil, p.fail(p.at, "unclosed style tag")
	}
	p.flushText()
	return p.runs, nil
}

type inlineParser struct {
	input   string
	at      int
	scopes  []scope
	runs    []Run
	literal strings.Builder
	// A tag is a boundary even if it happens to repeat the enclosing style.
	boundary bool
}

func (p *inlineParser) fail(at int, message string) error { return &Error{at, message} }
func (p *inlineParser) style() Style                      { return p.scopes[len(p.scopes)-1].style }
func (p *inlineParser) flushText() {
	if p.literal.Len() != 0 {
		p.runs[len(p.runs)-1].Text = p.literal.String()
		p.literal.Reset()
	}
}
func (p *inlineParser) append(run Run) error {
	p.flushText()
	if len(p.runs) == MaximumRuns {
		return p.fail(p.at, "too many inline runs")
	}
	p.runs = append(p.runs, run)
	p.boundary = false
	return nil
}
func (p *inlineParser) text(text string, at int) error {
	if n := len(p.runs); n > 0 && !p.boundary && p.runs[n-1].Value == "" && p.runs[n-1].Style == p.style() {
		p.literal.WriteString(text)
		return nil
	}
	if err := p.append(Run{Style: p.style(), SourceOffset: at}); err != nil {
		return err
	}
	p.literal.WriteString(text)
	return nil
}
func (p *inlineParser) value() error {
	start := p.at
	if p.input[start] == '}' {
		return p.fail(start, "unmatched '}' (write '}}' for a literal)")
	}
	end := strings.IndexByte(p.input[start+1:], '}')
	if end < 0 {
		return p.fail(start, "unclosed placeholder")
	}
	end += start + 1
	name, digits := p.input[start+1:end], 0
	if len(name) >= 256 {
		return p.fail(start, "invalid placeholder")
	}
	if colon := strings.IndexByte(name, ':'); colon >= 0 {
		format := name[colon+1:]
		if len(format) != 2 || format[0] != '0' || format[1] < '1' || format[1] > '9' {
			return p.fail(start, "number format must be 01 through 09")
		}
		digits, name = int(format[1]-'0'), name[:colon]
	}
	if !Identifier(name) {
		return p.fail(start, "invalid placeholder")
	}
	p.at = end + 1
	return p.append(Run{Value: name, MinimumDigits: digits, Style: p.style(), SourceOffset: start})
}
func (p *inlineParser) spaces() {
	for p.at < len(p.input) && (p.input[p.at] == ' ' || p.input[p.at] == '\t') {
		p.at++
	}
}
func (p *inlineParser) name() string {
	start := p.at
	for p.at < len(p.input) && p.input[p.at] >= 'a' && p.input[p.at] <= 'z' {
		p.at++
	}
	return p.input[start:p.at]
}
func (p *inlineParser) tag() error {
	start := p.at
	p.at++
	closing := p.at < len(p.input) && p.input[p.at] == '/'
	if closing {
		p.at++
	}
	name := p.name()
	if name != "i" && name != "span" {
		return p.fail(start, "unknown style tag (use \\< for literal '<')")
	}
	if closing {
		if p.at == len(p.input) || p.input[p.at] != '>' || len(p.scopes) == 1 || p.scopes[len(p.scopes)-1].tag != name {
			return p.fail(start, "mismatched closing style tag")
		}
		p.at++
		p.scopes = p.scopes[:len(p.scopes)-1]
		p.boundary = true
		return nil
	}
	if len(p.scopes) > MaximumDepth {
		return p.fail(start, "style nesting exceeds limit")
	}
	style := p.style()
	if name == "i" {
		style.Italic = 2
	}
	seen := make(map[string]bool)
	for p.at < len(p.input) && p.input[p.at] != '>' {
		before := p.at
		p.spaces()
		if p.at < len(p.input) && p.input[p.at] == '>' {
			break
		}
		if before == p.at || name != "span" {
			return p.fail(p.at, "expected style attribute or '>'")
		}
		at, key := p.at, p.name()
		if key == "" || seen[key] {
			return p.fail(at, "missing or duplicate style attribute")
		}
		seen[key] = true
		p.spaces()
		if p.at == len(p.input) || p.input[p.at] != '=' {
			return p.fail(p.at, "expected '=' after style attribute")
		}
		p.at++
		p.spaces()
		if p.at == len(p.input) || p.input[p.at] != '"' {
			return p.fail(p.at, "style values require double quotes")
		}
		p.at++
		valueAt := p.at
		for p.at < len(p.input) && p.input[p.at] != '"' {
			p.at++
		}
		if p.at == len(p.input) {
			return p.fail(valueAt, "unclosed style attribute")
		}
		if err := SetProperty(&style, key, p.input[valueAt:p.at]); err != nil {
			return p.fail(at, err.Error())
		}
		p.at++
	}
	if p.at == len(p.input) {
		return p.fail(start, "unclosed style tag")
	}
	if name == "span" && len(seen) == 0 {
		return p.fail(start, "span requires at least one style attribute")
	}
	p.at++
	p.scopes = append(p.scopes, scope{name, style})
	p.boundary = true
	return nil
}
