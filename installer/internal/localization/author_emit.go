package localization

import (
	"fmt"
	"slices"
	"strings"
)

var authorBraceEscapes = strings.NewReplacer("{", "{{", "}", "}}")
var authorQuoteEscapes = strings.NewReplacer("\\", "\\\\", "\"", "\\\"")

// EmitAuthorScript constructs new source, not a save-time rewrite of an edited
// file. It reparses and compares presentation operations before returning: text
// that would become syntax, lose spacing or inject a message fails atomically.
// Physical source lines are regenerated; adjacent text operations may coalesce,
// and the format's implicit final end is materialized by the parser.
// Semantic contracts and aliases across files are validated by AuthorWorkspace.
func EmitAuthorScript(messages []AuthorMessage, path string) (*AuthorScript, error) {
	if len(messages) == 0 || len(messages) > MaxAuthorMessages {
		return nil, authorError(path, 0, "invalid message count")
	}
	var out strings.Builder
	line := func(value string) error {
		if len(value) >= MaxAuthorScriptBytes-out.Len() {
			return authorError(path, 0, "emitted script exceeds size limit")
		}
		out.WriteString(value)
		out.WriteByte('\n')
		return nil
	}
	for i, m := range messages {
		if !authorIdentifier(m.ID) || len(m.ID) > MaxAuthorTextBytes || len(m.Operations) > MaxAuthorOperations {
			return nil, authorError(path, 0, "invalid message identity or operation count")
		}
		if i != 0 {
			if err := line(""); err != nil {
				return nil, err
			}
		}
		if err := line(":: " + m.ID); err != nil {
			return nil, err
		}
		if m.Alias != "" {
			if !authorIdentifier(m.Alias) || len(m.Alias) > MaxAuthorTextBytes || len(m.Operations) != 0 {
				return nil, authorError(path, 0, "invalid alias %s", m.ID)
			}
			if err := line("@alias " + m.Alias); err != nil {
				return nil, err
			}
			continue
		}
		var inline strings.Builder
		flush := func() error {
			if inline.Len() == 0 {
				return nil
			}
			value := inline.String()
			if strings.HasPrefix(value, "@") {
				value = "@" + value
			} else if strings.HasPrefix(value, "#") || strings.HasPrefix(value, ";") {
				value = "\\" + value
			}
			inline.Reset()
			return line(value)
		}
		textBytes := 0
		for _, op := range m.Operations {
			var value string
			switch op.Op {
			case "text":
				if len(op.Value) > MaxAuthorTextBytes-textBytes || strings.ContainsAny(op.Value, "\r\n\x00") {
					return nil, authorError(path, 0, "%s: text cannot be emitted losslessly", m.ID)
				}
				textBytes += len(op.Value)
				inline.WriteString(authorBraceEscapes.Replace(op.Value))
				continue
			case "placeholder":
				if !authorIdentifier(op.Name) || len(op.Name) >= 256 || op.MinimumDigits < 0 || op.MinimumDigits > 9 {
					return nil, authorError(path, 0, "%s: invalid placeholder", m.ID)
				}
				inline.WriteByte('{')
				inline.WriteString(op.Name)
				if op.MinimumDigits != 0 {
					fmt.Fprintf(&inline, ":0%d", op.MinimumDigits)
				}
				inline.WriteByte('}')
				continue
			case "line", "paragraph", "page", "empty", "end":
				value = "@" + op.Op
			case "wait":
				value = fmt.Sprintf("@wait %d", op.Frames)
			case "anchor", "event":
				if !authorIdentifier(op.ID) || len(op.ID) >= 256 {
					return nil, authorError(path, 0, "%s: invalid control", m.ID)
				}
				value = "@" + op.Op + " " + op.ID
				if op.Op == "event" && op.Arguments != "" {
					if len(op.Arguments) > 1024 || strings.ContainsAny(op.Arguments, "\r\n\x00") {
						return nil, authorError(path, 0, "%s: invalid event arguments", m.ID)
					}
					value += " \"" + authorQuoteEscapes.Replace(op.Arguments) + "\""
				}
			default:
				return nil, authorError(path, 0, "%s: unknown operation %q", m.ID, op.Op)
			}
			if err := flush(); err != nil {
				return nil, err
			}
			if err := line(value); err != nil {
				return nil, err
			}
		}
		if err := flush(); err != nil {
			return nil, err
		}
	}
	s, err := ParseAuthorScript(out.String(), path)
	if err != nil {
		return nil, err
	}
	if len(s.messages) != len(messages) {
		return nil, authorError(path, 0, "emission changed message boundaries")
	}
	for i, m := range messages {
		got := s.messages[i]
		wantOps, gotOps := authorPresentation(m.Operations), authorPresentation(got.Operations)
		if len(wantOps) > 0 && wantOps[len(wantOps)-1].Op != "end" {
			wantOps = append(wantOps, AuthorOperation{Op: "end"})
		}
		if m.ID != got.ID || m.Alias != got.Alias || !slices.Equal(wantOps, gotOps) {
			at := 0
			for at < min(len(wantOps), len(gotOps)) && wantOps[at] == gotOps[at] {
				at++
			}
			return nil, authorError(path, got.SourceLine, "%s: operations cannot be emitted losslessly (operation %d, input=%d parsed=%d)", m.ID, at, len(wantOps), len(gotOps))
		}
	}
	return s, nil
}

func authorPresentation(operations []AuthorOperation) []AuthorOperation {
	result := make([]AuthorOperation, 0, len(operations))
	for i := 0; i < len(operations); {
		op := operations[i]
		op.SourceLine = 0
		i++
		if op.Op == "text" {
			var text strings.Builder
			text.WriteString(op.Value)
			base := op
			base.Value = ""
			for i < len(operations) {
				next := operations[i]
				next.SourceLine, next.Value = 0, ""
				if next != base {
					break
				}
				text.WriteString(operations[i].Value)
				i++
			}
			op.Value = text.String()
		}
		result = append(result, op)
	}
	return result
}
