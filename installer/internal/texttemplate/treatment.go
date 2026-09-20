package texttemplate

import (
	"fmt"
	"strings"
)

// Treatment is a flat ink/shadow definition. Font and scale are deliberately
// absent: choosing a treatment does not change the text's geometry.
type Treatment struct {
	Name   string `json:"name"`
	Band   string `json:"band"`
	Body   string `json:"body"`
	Shadow string `json:"shadow"`
	Shape  string `json:"shape"`
}

func (t *Treatment) SetProperty(name, value string) error {
	if name == "shape" {
		if value != "diagonal" && value != "keyline" {
			return fmt.Errorf("shadow shape must be diagonal or keyline")
		}
		t.Shape = value
		return nil
	}
	if name != "band" && name != "body" && name != "shadow" {
		return fmt.Errorf("unknown treatment property %q", name)
	}
	switch {
	case name == "shadow" && value == "none":
	case strings.HasPrefix(value, "native:") && len(value[7:]) <= MaximumRoleBytes && Identifier(value[7:]):
	default:
		var style Style
		if err := SetProperty(&style, "color", value); err != nil {
			return fmt.Errorf("%s ink requires #RRGGBB or native:binding", name)
		}
		value = style.Color
	}
	switch name {
	case "band":
		t.Band = value
	case "body":
		t.Body = value
	case "shadow":
		t.Shadow = value
	}
	return nil
}
