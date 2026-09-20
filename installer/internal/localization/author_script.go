package localization

import (
	"fmt"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
)

const (
	MaxAuthorScriptBytes     = 16 << 20
	MaxAuthorMessages        = 16384
	MaxAuthorOperations      = 4096
	MaxAuthorPages           = 64
	MaxAuthorTextBytes       = 256 << 10
	MaxAuthorWaitFrames      = 600
	MaxAuthorTotalWaitFrames = 3600
)

// AuthorError retains the file and physical line for editor diagnostics.
type AuthorError struct {
	Path    string
	Line    int
	Message string
}

func (e *AuthorError) Error() string {
	if e.Line > 0 {
		return fmt.Sprintf("%s:%d: %s", e.Path, e.Line, e.Message)
	}
	return e.Path + ": " + e.Message
}

func authorError(path string, line int, format string, args ...any) error {
	return &AuthorError{path, line, fmt.Sprintf(format, args...)}
}

// AuthorOperation is portable presentation data, never a ROM operation/address.
type AuthorOperation struct {
	Op            string             `json:"op"`
	Value         string             `json:"value,omitempty"`
	Name          string             `json:"name,omitempty"`
	ID            string             `json:"id,omitempty"`
	Arguments     string             `json:"arguments,omitempty"`
	Frames        int                `json:"frames,omitempty"`
	MinimumDigits int                `json:"minimum_digits,omitempty"`
	SourceLine    int                `json:"source_line"`
	Style         texttemplate.Style `json:"style,omitzero"`
}

// AuthorTextAppearance keeps message defaults beside its wording. The semantic
// presentation contract still decides which layout and values the game allows.
type AuthorTextAppearance struct {
	Layout   string             `json:"layout,omitempty"`
	Numerals string             `json:"numerals,omitempty"`
	Style    texttemplate.Style `json:"style"`
}

type AuthorMessage struct {
	ID         string               `json:"id"`
	Alias      string               `json:"alias,omitempty"`
	SourceLine int                  `json:"source_line"`
	Operations []AuthorOperation    `json:"operations"`
	Appearance AuthorTextAppearance `json:"appearance,omitzero"`
}

type authorSpan struct{ bodyStart, end int }

// AuthorScript owns an immutable parsed snapshot and the original source bytes.
// Keeping the source, rather than re-emitting operations on save, preserves
// comments, whitespace and translator notes in messages that were not edited.
// It is syntax-checked only: ValidateAuthorScripts applies semantic contracts
// and resolves aliases across all the files in a pack.
type AuthorScript struct {
	path, text string
	messages   []AuthorMessage
	spans      []authorSpan
	byID       map[string]int
	version    int
	treatments []AuthorTreatment
}

func (s *AuthorScript) Text() string { return s.text }
func (s *AuthorScript) Path() string { return s.path }
func (s *AuthorScript) Messages() []AuthorMessage {
	out := make([]AuthorMessage, len(s.messages))
	for i, message := range s.messages {
		out[i] = message
		out[i].Operations = append([]AuthorOperation{}, message.Operations...)
	}
	return out
}

func (s *AuthorScript) Body(id string) (string, bool) {
	i, ok := s.byID[id]
	if !ok {
		return "", false
	}
	return s.text[s.spans[i].bodyStart:s.spans[i].end], true
}

// ReplaceBody returns a new syntax-checked snapshot; it never modifies s.
// Callers must validate the entire candidate pack before committing the edit,
// because changing a message can invalidate another file's alias contract.
func (s *AuthorScript) ReplaceBody(id, body string) (*AuthorScript, error) {
	i, ok := s.byID[id]
	if !ok {
		return nil, authorError(s.path, 0, "unknown message %q", id)
	}
	if body != "" && !strings.HasSuffix(body, "\n") && !strings.HasSuffix(body, "\r") {
		body += "\n"
	}
	span := s.spans[i]
	if len(body) > MaxAuthorScriptBytes-(len(s.text)-(span.end-span.bodyStart)) {
		return nil, authorError(s.path, 0, "script exceeds size limit")
	}
	next, err := ParseAuthorScriptVersion(s.text[:span.bodyStart]+body+s.text[span.end:], s.path, s.version)
	if err != nil {
		return nil, err
	}
	if len(next.messages) != len(s.messages) {
		return nil, authorError(s.path, 0, "message edit cannot introduce or remove message headers")
	}
	for n := range s.messages {
		if next.messages[n].ID != s.messages[n].ID {
			return nil, authorError(s.path, 0, "message edit cannot change message identities")
		}
	}
	return next, nil
}

func authorIdentifier(value string) bool {
	letter := func(c byte) bool { return c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' }
	if value == "" || !letter(value[0]) {
		return false
	}
	for i := 1; i < len(value); i++ {
		c := value[i]
		if !letter(c) && !(c >= '0' && c <= '9') && c != '_' && c != '.' && c != '-' {
			return false
		}
	}
	return true
}

// The runtime's physical separators are CR, LF and CRLF, not Unicode paragraph
// or nonbreaking whitespace. Source offsets always refer to original bytes.
func authorLines(text string, visit func(line string, number, start, next int) error) error {
	for start, number := 0, 1; start < len(text); number++ {
		end := start + strings.IndexAny(text[start:], "\r\n")
		if end < start {
			end = len(text)
		}
		next := end
		if next < len(text) {
			next++
			if text[end] == '\r' && next < len(text) && text[next] == '\n' {
				next++
			}
		}
		if err := visit(text[start:end], number, start, next); err != nil {
			return err
		}
		start = next
	}
	return nil
}

type authorParser struct {
	script                  *AuthorScript
	current                 *AuthorMessage
	ended, previousText     bool
	textBytes, pages, waits int
	defaults                map[string]bool
	styledText              strings.Builder
	styledSources           []styledSourceLine
}

func (p *authorParser) add(op AuthorOperation) error {
	if len(p.current.Operations) >= MaxAuthorOperations {
		return authorError(p.script.path, op.SourceLine, "message has too many operations")
	}
	p.current.Operations = append(p.current.Operations, op)
	return nil
}

func (p *authorParser) text(value string, line int) error {
	if value == "" {
		return nil
	}
	if len(value) > MaxAuthorTextBytes-p.textBytes {
		return authorError(p.script.path, line, "message text is too large")
	}
	p.textBytes += len(value)
	ops := p.current.Operations
	if n := len(ops); n != 0 && ops[n-1].Op == "text" && ops[n-1].SourceLine == line && ops[n-1].Style == (texttemplate.Style{}) {
		ops[n-1].Value += value
		return nil
	}
	return p.add(AuthorOperation{Op: "text", Value: value, SourceLine: line})
}

func (p *authorParser) inline(value string, line int) error {
	if p.script.version == 2 {
		return p.styledInline(value, line)
	}
	var literal strings.Builder
	flush := func() error { err := p.text(literal.String(), line); literal.Reset(); return err }
	for i := 0; i < len(value); {
		c := value[i]
		if c != '{' && c != '}' {
			literal.WriteByte(c)
			i++
			continue
		}
		if i+1 < len(value) && value[i+1] == c {
			literal.WriteByte(c)
			i += 2
			continue
		}
		if c == '}' {
			return authorError(p.script.path, line, "unmatched '}' (write '}}' for a literal)")
		}
		end := strings.IndexByte(value[i+1:], '}')
		if end < 0 {
			return authorError(p.script.path, line, "unclosed placeholder")
		}
		end += i + 1
		name := value[i+1 : end]
		if len(name) >= 256 {
			return authorError(p.script.path, line, "invalid placeholder")
		}
		digits := 0
		if colon := strings.IndexByte(name, ':'); colon >= 0 {
			spec := name[colon+1:]
			if len(spec) != 2 || spec[0] != '0' || spec[1] < '1' || spec[1] > '9' {
				return authorError(p.script.path, line, "number format must be 01 through 09")
			}
			digits, name = int(spec[1]-'0'), name[:colon]
		}
		if !authorIdentifier(name) {
			return authorError(p.script.path, line, "invalid placeholder %q", name)
		}
		if err := flush(); err != nil {
			return err
		}
		if err := p.add(AuthorOperation{Op: "placeholder", Name: name, MinimumDigits: digits, SourceLine: line}); err != nil {
			return err
		}
		i = end + 1
	}
	return flush()
}

// POSIX-style quoting is syntax only. Nothing is executed or interpolated.
func authorCommandTokens(line string) ([]string, error) {
	var tokens []string
	for i := 0; i < len(line); {
		for i < len(line) && (line[i] == ' ' || line[i] == '\t') {
			i++
		}
		if i == len(line) {
			break
		}
		if len(tokens) == 128 {
			return nil, fmt.Errorf("command has too many arguments")
		}
		var token strings.Builder
		var quote byte
		for i < len(line) {
			c := line[i]
			i++
			if quote == 0 && (c == ' ' || c == '\t') {
				break
			}
			if quote == 0 && (c == '\'' || c == '"') {
				quote = c
				continue
			}
			if quote != 0 && c == quote {
				quote = 0
				continue
			}
			if c == '\\' && quote != '\'' {
				if i == len(line) {
					return nil, fmt.Errorf("trailing command escape")
				}
				if quote == '"' && line[i] != '"' && line[i] != '\\' {
					token.WriteByte(c)
				}
				c = line[i]
				i++
			}
			token.WriteByte(c)
		}
		if quote != 0 {
			return nil, fmt.Errorf("unterminated command quote")
		}
		tokens = append(tokens, token.String())
	}
	return tokens, nil
}

func (p *authorParser) command(line string, number int) error {
	tokens, err := authorCommandTokens(line)
	if err != nil {
		return authorError(p.script.path, number, "%s", err)
	}
	if len(tokens) == 0 {
		return nil
	}
	if p.script.version == 2 && isAppearanceCommand(tokens[0]) {
		return p.appearance(tokens, number)
	}
	op := AuthorOperation{SourceLine: number}
	switch tokens[0] {
	case "@line", "@preferred-line", "@paragraph", "@page", "@empty", "@end":
		if len(tokens) != 1 {
			return authorError(p.script.path, number, "%s takes no arguments", tokens[0])
		}
		op.Op = strings.ReplaceAll(tokens[0][1:], "-", "_")
		if op.Op == "page" {
			p.pages++
			if p.pages > MaxAuthorPages {
				return authorError(p.script.path, number, "too many authored pages")
			}
		}
		p.ended = op.Op == "end"
	case "@anchor", "@alias":
		if len(tokens) != 2 || !authorIdentifier(tokens[1]) {
			return authorError(p.script.path, number, "%s requires one stable identifier", tokens[0])
		}
		if tokens[0] == "@alias" {
			if len(p.current.Operations) != 0 || len(p.defaults) != 0 {
				return authorError(p.script.path, number, "@alias must be the only message content")
			}
			p.current.Alias, p.ended = tokens[1], true
			return nil
		}
		op.Op, op.ID = "anchor", tokens[1]
	case "@wait":
		if len(tokens) != 2 || strings.Trim(tokens[1], "0123456789") != "" || tokens[1] == "" {
			return authorError(p.script.path, number, "@wait requires a decimal frame count")
		}
		frames, err := strconv.Atoi(tokens[1])
		if err != nil || frames < 1 || frames > MaxAuthorWaitFrames {
			return authorError(p.script.path, number, "@wait must be 1-600 frames")
		}
		p.waits += frames
		if p.waits > MaxAuthorTotalWaitFrames {
			return authorError(p.script.path, number, "cumulative authored wait is too long")
		}
		op.Op, op.Frames = "wait", frames
	case "@event":
		if len(tokens) < 2 || !authorIdentifier(tokens[1]) {
			return authorError(p.script.path, number, "@event requires a valid event id")
		}
		op.Op, op.ID, op.Arguments = "event", tokens[1], strings.Join(tokens[2:], " ")
		if len(op.Arguments) > 1024 {
			return authorError(p.script.path, number, "@event arguments are too long")
		}
	default:
		return authorError(p.script.path, number, "unknown command %q", tokens[0])
	}
	return p.add(op)
}

func (p *authorParser) finish() error {
	if err := p.flushStyledText(); err != nil {
		return err
	}
	if p.current == nil {
		return nil
	}
	m := p.current
	for len(m.Operations) > 0 && m.Operations[len(m.Operations)-1].Op == "paragraph" {
		m.Operations = m.Operations[:len(m.Operations)-1]
	}
	if m.Alias != "" {
		return nil
	}
	if len(m.Operations) == 0 {
		return authorError(p.script.path, m.SourceLine, "message has no content; use @empty intentionally")
	}
	empty, bad := 0, false
	for _, op := range m.Operations {
		if op.Op == "empty" {
			empty++
		} else if op.Op != "anchor" && op.Op != "end" {
			bad = true
		}
	}
	if empty > 1 || empty == 1 && bad {
		return authorError(p.script.path, m.SourceLine, "@empty permits only required @anchor commands")
	}
	if m.Operations[len(m.Operations)-1].Op != "end" {
		return p.add(AuthorOperation{Op: "end", SourceLine: m.SourceLine})
	}
	return nil
}

func ParseAuthorScript(text, path string) (*AuthorScript, error) {
	return ParseAuthorScriptVersion(text, path, 1)
}

// ParseAuthorScriptVersion never guesses a format from markup. Legacy text
// containing angle brackets stays literal until an explicit builder upgrade.
func ParseAuthorScriptVersion(text, path string, version int) (*AuthorScript, error) {
	if version != 1 && version != 2 {
		return nil, authorError(path, 0, "unsupported template version %d", version)
	}
	if len(text) > MaxAuthorScriptBytes {
		return nil, authorError(path, 0, "script exceeds size limit")
	}
	if !utf8.ValidString(text) {
		return nil, authorError(path, 0, "script is not UTF-8")
	}
	if strings.IndexByte(text, 0) >= 0 {
		return nil, authorError(path, 0, "script contains a NUL byte")
	}
	s := &AuthorScript{path: path, text: text, byID: make(map[string]int), version: version}
	p := authorParser{script: s}
	err := authorLines(text, func(physical string, number, start, next int) error {
		if number == 1 {
			physical = strings.TrimPrefix(physical, "\ufeff")
		}
		stripped := strings.Trim(physical, " \t")
		if strings.HasPrefix(stripped, "@define-style") && p.current == nil {
			return p.defineTreatment(stripped, number)
		}
		if strings.HasPrefix(stripped, "::") {
			if err := p.finish(); err != nil {
				return err
			}
			id := strings.Trim(stripped[2:], " \t")
			if !authorIdentifier(id) {
				return authorError(path, number, "invalid semantic message id %q", id)
			}
			if _, exists := s.byID[id]; exists {
				return authorError(path, number, "duplicate message %q", id)
			}
			if len(s.messages) >= MaxAuthorMessages {
				return authorError(path, number, "too many messages")
			}
			if len(s.spans) != 0 {
				s.spans[len(s.spans)-1].end = start
			}
			s.byID[id] = len(s.messages)
			s.messages = append(s.messages, AuthorMessage{ID: id, SourceLine: number, Operations: []AuthorOperation{}})
			s.spans = append(s.spans, authorSpan{next, len(text)})
			p.current = &s.messages[len(s.messages)-1]
			p.ended, p.previousText, p.textBytes, p.pages, p.waits = false, false, 0, 1, 0
			p.defaults = make(map[string]bool)
			return nil
		}
		comment := strings.HasPrefix(stripped, "#") || strings.HasPrefix(stripped, ";")
		if p.current == nil || p.ended {
			if stripped == "" || comment {
				return nil
			}
			return authorError(path, number, "content appears outside a message body")
		}
		if stripped == "" {
			if err := p.flushStyledText(); err != nil {
				return err
			}
			p.previousText = false
			if ops := p.current.Operations; len(ops) != 0 {
				last := ops[len(ops)-1].Op
				if last != "line" && last != "preferred_line" &&
					last != "paragraph" && last != "page" {
					return p.add(AuthorOperation{Op: "paragraph", SourceLine: number})
				}
			}
			return nil
		}
		if comment {
			return nil
		}
		if strings.HasPrefix(physical, "@@") || strings.HasPrefix(physical, "\\#") || strings.HasPrefix(physical, "\\;") {
			physical = physical[1:]
		} else if strings.HasPrefix(stripped, "@") {
			if err := p.flushStyledText(); err != nil {
				return err
			}
			p.previousText = false
			return p.command(stripped, number)
		}
		if s.version == 2 {
			return p.queueStyledText(physical, number)
		}
		if p.previousText {
			if err := p.text(" ", number); err != nil {
				return err
			}
		}
		if err := p.inline(physical, number); err != nil {
			return err
		}
		p.previousText = true
		return nil
	})
	if err == nil {
		err = p.finish()
	}
	if err != nil {
		return nil, err
	}
	if len(s.messages) == 0 && len(s.treatments) == 0 {
		return nil, authorError(path, 0, "script contains no messages")
	}
	return s, nil
}
