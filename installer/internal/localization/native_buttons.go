package localization

import (
	_ "embed"
	"encoding/json"
	"regexp"
	"strings"
)

//go:embed data/sim-help.json
var nativeHelpData []byte

func nativeHelpMessages() []AuthorMessage {
	var rows []struct{ ID, Text string }
	if err := json.Unmarshal(nativeHelpData, &rows); err != nil {
		panic(err)
	}
	var out []AuthorMessage
	for _, row := range rows {
		// This inventory is project-authored prose, independent of a retail ROM.
		script, err := ParseAuthorScript(":: "+row.ID+"\n"+row.Text+"\n@end\n", "help.artext")
		if err != nil {
			panic(err)
		}
		out = append(out, script.Messages()...)
	}
	return out
}

var nativeButtonWords = regexp.MustCompile(`\b(B|Y|START|Start)\b|Startknopf`)

// Only this known native instruction route names controller buttons. Preserve
// punctuation, style and all gameplay controls; never rewrite arbitrary prose.
func nativeButtonReferences(ops []AuthorOperation, id string) []AuthorOperation {
	if id != "dialogue.event.wrapper_05.call_00.source_00" && id != "title.start_prompt" {
		return ops
	}
	var out []AuthorOperation
	for _, op := range ops {
		if op.Op != "text" {
			out = append(out, op)
			continue
		}
		at := 0
		for _, match := range nativeButtonWords.FindAllStringIndex(op.Value, -1) {
			word := op.Value[match[0]:match[1]]
			key := strings.ToLower(word)
			end := match[1]
			if word == "Startknopf" {
				key = "start"
				end = match[0] + 5
			}
			if match[0] > at {
				text := op
				text.Value = op.Value[at:match[0]]
				out = append(out, text)
			}
			value := op
			value.Op = "placeholder"
			value.Value = ""
			value.Name = "icon.button." + key
			out = append(out, value)
			at = end
		}
		if at < len(op.Value) {
			op.Value = op.Value[at:]
			out = append(out, op)
		}
	}
	return out
}
