package localization

import (
	"fmt"
	"sort"
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
)

type styleProperty struct{ name, value string }

func validateAuthorFontRoles(m *PackManifest, w *AuthorWorkspace) error {
	roles := map[string]bool{"": true, "body": true}
	for _, role := range m.fonts.Roles {
		roles[role.Name] = true
	}
	for _, script := range w.scripts {
		for _, message := range script.messages {
			if !roles[message.Appearance.Style.Font] {
				return authorError(script.path, message.SourceLine, "undeclared font role %q", message.Appearance.Style.Font)
			}
			for _, op := range message.Operations {
				if !roles[op.Style.Font] {
					return authorError(script.path, op.SourceLine, "undeclared font role %q", op.Style.Font)
				}
			}
		}
	}
	return nil
}

func styleProperties(style texttemplate.Style) []styleProperty {
	var properties []styleProperty
	if style.Font != "" {
		properties = append(properties, styleProperty{"font", style.Font})
	}
	if style.Treatment != "" {
		properties = append(properties, styleProperty{"style", style.Treatment})
	}
	if style.Color != "" {
		properties = append(properties, styleProperty{"color", style.Color})
	}
	if style.Scale != 0 {
		properties = append(properties, styleProperty{"scale", fmt.Sprintf("%d%%", style.Scale)})
	}
	if style.Italic != 0 {
		properties = append(properties, styleProperty{"italic", fmt.Sprint(style.Italic == 2)})
	}
	return properties
}

func appearanceDirectives(appearance AuthorTextAppearance) []string {
	var lines []string
	if appearance.Layout != "" {
		lines = append(lines, "@layout "+appearance.Layout)
	}
	if appearance.Numerals != "" {
		lines = append(lines, "@numerals "+appearance.Numerals)
	}
	for _, property := range styleProperties(appearance.Style) {
		lines = append(lines, "@"+property.name+" "+property.value)
	}
	return lines
}

func inlineStyleTags(style texttemplate.Style) (string, string) {
	if style == (texttemplate.Style{Italic: 2}) {
		return "<i>", "</i>"
	}
	var tag strings.Builder
	tag.WriteString("<span")
	for _, property := range styleProperties(style) {
		fmt.Fprintf(&tag, " %s=\"%s\"", property.name, authorQuoteEscapes.Replace(property.value))
	}
	tag.WriteByte('>')
	return tag.String(), "</span>"
}

func isAppearanceCommand(command string) bool {
	switch command {
	case "@layout", "@numerals", "@font", "@style", "@color", "@scale", "@italic":
		return true
	}
	return false
}

func (p *authorParser) appearance(tokens []string, line int) error {
	fail := func(message string) error { return authorError(p.script.path, line, "%s", message) }
	if len(tokens) != 2 {
		return fail("presentation defaults require one value")
	}
	if len(p.current.Operations) != 0 {
		return fail("presentation defaults must precede message content")
	}
	property := strings.TrimPrefix(tokens[0], "@")
	if p.defaults[property] {
		return fail("duplicate presentation default: " + property)
	}
	p.defaults[property] = true
	appearance := &p.current.Appearance
	switch property {
	case "layout":
		if len(tokens[1]) > texttemplate.MaximumRoleBytes || !texttemplate.Identifier(tokens[1]) {
			return fail("layout requires a stable identifier")
		}
		appearance.Layout = tokens[1]
	case "numerals":
		if tokens[1] != "upright" && tokens[1] != "slanted-ascii" {
			return fail("numerals must be upright or slanted-ascii")
		}
		appearance.Numerals = tokens[1]
	default:
		if err := texttemplate.SetProperty(&appearance.Style, property, tokens[1]); err != nil {
			return fail(err.Error())
		}
	}
	return nil
}

type styledSourceLine struct{ offset, line int }

func (p *authorParser) queueStyledText(value string, line int) error {
	separator := 0
	if len(p.styledSources) != 0 {
		separator = 1
	}
	if len(value)+separator > texttemplate.MaximumBytes-p.styledText.Len() {
		return authorError(p.script.path, line, "inline text exceeds size limit")
	}
	p.styledSources = append(p.styledSources, styledSourceLine{p.styledText.Len(), line})
	if separator != 0 {
		p.styledText.WriteByte(' ')
	}
	p.styledText.WriteString(value)
	return nil
}

func (p *authorParser) styledSourceLine(offset int) int {
	i := sort.Search(len(p.styledSources), func(i int) bool { return p.styledSources[i].offset > offset })
	if i == 0 {
		return 0
	}
	return p.styledSources[i-1].line
}

func (p *authorParser) flushStyledText() error {
	if len(p.styledSources) == 0 {
		return nil
	}
	err := p.styledInline(p.styledText.String(), p.styledSources[0].line)
	p.styledText.Reset()
	p.styledSources = nil
	return err
}

func (p *authorParser) styledInline(value string, line int) error {
	runs, err := texttemplate.ParseInline(value)
	if err != nil {
		if detail, ok := err.(*texttemplate.Error); ok && len(p.styledSources) != 0 {
			line = p.styledSourceLine(detail.Offset)
		}
		return authorError(p.script.path, line, "%s", err)
	}
	for _, run := range runs {
		if len(p.styledSources) != 0 {
			line = p.styledSourceLine(run.SourceOffset)
		}
		op := AuthorOperation{SourceLine: line, Style: run.Style}
		if run.Value != "" {
			op.Op, op.Name, op.MinimumDigits = "placeholder", run.Value, run.MinimumDigits
		} else {
			if len(run.Text) > MaxAuthorTextBytes-p.textBytes {
				return authorError(p.script.path, line, "message text is too large")
			}
			p.textBytes += len(run.Text)
			op.Op, op.Value = "text", run.Text
		}
		if err := p.add(op); err != nil {
			return err
		}
	}
	return nil
}
