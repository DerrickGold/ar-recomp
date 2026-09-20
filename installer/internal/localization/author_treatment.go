package localization

import (
	"strings"

	"github.com/DerrickGold/ar-recomp/installer/internal/texttemplate"
)

type AuthorTreatment struct {
	Definition texttemplate.Treatment `json:"definition"`
	SourcePath string                 `json:"sourcePath"`
	SourceLine int                    `json:"sourceLine"`
}

func (s *AuthorScript) Treatments() []AuthorTreatment {
	return append([]AuthorTreatment{}, s.treatments...)
}
func (p *AuthorPack) Treatments() []AuthorTreatment {
	var definitions []AuthorTreatment
	for _, script := range p.workspace.scripts {
		definitions = append(definitions, script.treatments...)
	}
	return definitions
}

func (p *authorParser) defineTreatment(command string, line int) error {
	fail := func(message string) error { return authorError(p.script.path, line, "%s", message) }
	tokens, err := authorCommandTokens(command)
	if err != nil {
		return fail(err.Error())
	}
	if p.script.version != 2 || len(tokens) < 4 || tokens[0] != "@define-style" || !texttemplate.Identifier(tokens[1]) || len(tokens[1]) > texttemplate.MaximumRoleBytes {
		return fail("v2 style definition requires a name, band and body inks")
	}
	if len(p.script.treatments) == 64 {
		return fail("too many style definitions")
	}
	for _, prior := range p.script.treatments {
		if prior.Definition.Name == tokens[1] {
			return fail("duplicate style definition: " + tokens[1])
		}
	}
	definition := texttemplate.Treatment{Name: tokens[1], Shadow: "none", Shape: "diagonal"}
	seen := map[string]bool{}
	for _, token := range tokens[2:] {
		name, value, ok := strings.Cut(token, "=")
		if !ok || seen[name] {
			return fail("expected unique style property=value")
		}
		seen[name] = true
		if err := definition.SetProperty(name, value); err != nil {
			return fail(err.Error())
		}
	}
	if !seen["band"] || !seen["body"] {
		return fail("style definition requires band and body inks")
	}
	p.script.treatments = append(p.script.treatments, AuthorTreatment{definition, p.script.path, line})
	return nil
}

func validateAuthorTreatments(w *AuthorWorkspace) error {
	knownInk := func(ink string) bool {
		if !strings.HasPrefix(ink, "native:") {
			return true
		}
		for _, binding := range authorContracts.nativeInks {
			if binding.Name == ink {
				return true
			}
		}
		return false
	}
	definitions := make(map[string]bool)
	for _, script := range w.scripts {
		for _, treatment := range script.treatments {
			for _, ink := range []string{treatment.Definition.Band, treatment.Definition.Body, treatment.Definition.Shadow} {
				if !knownInk(ink) {
					return authorError(script.path, treatment.SourceLine, "style %q has unknown native ink %q", treatment.Definition.Name, ink)
				}
			}
			name := treatment.Definition.Name
			if definitions[name] || len(definitions) == 64 {
				return authorError(script.path, treatment.SourceLine, "duplicate or excessive style definition %q", name)
			}
			definitions[name] = true
		}
	}
	defined := func(name string) bool { return name == "" || definitions[name] }
	for _, script := range w.scripts {
		for _, message := range script.messages {
			if !defined(message.Appearance.Style.Treatment) {
				return authorError(script.path, message.SourceLine, "undefined style %q", message.Appearance.Style.Treatment)
			}
			for _, op := range message.Operations {
				if !defined(op.Style.Treatment) {
					return authorError(script.path, op.SourceLine, "undefined style %q", op.Style.Treatment)
				}
			}
		}
	}
	return nil
}
