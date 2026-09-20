package texttemplate

import (
	"encoding/json"
	"os"
	"reflect"
	"strings"
	"testing"
)

func TestSharedInlineContract(t *testing.T) {
	data, err := os.ReadFile("../../../tests/fixtures/text_template.json")
	if err != nil {
		t.Fatal(err)
	}
	var cases []struct {
		Name, Input string
		Invalid     bool
		Runs        []Run
	}
	if err := json.Unmarshal(data, &cases); err != nil {
		t.Fatal(err)
	}
	for _, fixture := range cases {
		t.Run(fixture.Name, func(t *testing.T) {
			runs, err := ParseInline(fixture.Input)
			if fixture.Invalid {
				if err == nil {
					t.Fatal("accepted invalid markup")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			for i := range runs {
				runs[i].SourceOffset = 0
			}
			if !reflect.DeepEqual(runs, fixture.Runs) {
				t.Fatalf("got %#v; want %#v", runs, fixture.Runs)
			}
		})
	}
}

func TestBoundsAndSourceOffsets(t *testing.T) {
	valid := strings.Repeat("<i>", MaximumDepth) + "x" + strings.Repeat("</i>", MaximumDepth)
	if _, err := ParseInline(valid); err != nil {
		t.Fatal(err)
	}
	for _, text := range []string{"<i>" + valid + "</i>", strings.Repeat("x", MaximumBytes+1), "\xff", "\x00", strings.Repeat("{value}", MaximumRuns+1)} {
		if _, err := ParseInline(text); err == nil {
			t.Fatal("accepted out-of-bounds input")
		}
	}
	runs, err := ParseInline("é<i>{value}</i>x")
	if err != nil {
		t.Fatal(err)
	}
	if runs[0].SourceOffset != 0 || runs[1].SourceOffset != 5 || runs[2].SourceOffset != 16 {
		t.Fatalf("source offsets: %#v", runs)
	}
}

func TestLongEscapedLiteral(t *testing.T) {
	input := strings.Repeat("{{\\<\\\\", MaximumBytes/6)
	runs, err := ParseInline(input)
	if err != nil || len(runs) != 1 || runs[0].Text != strings.Repeat("{<\\", MaximumBytes/6) {
		t.Fatalf("escaped literal did not stay in one run: %v", err)
	}
}

func FuzzInline(f *testing.F) {
	for _, text := range []string{"", "<i>{hp:02}</i>", "<span scale=\"80%\">日本語</span>", "{{literal}}"} {
		f.Add(text)
	}
	f.Fuzz(func(t *testing.T, text string) {
		runs, err := ParseInline(text)
		if err != nil {
			return
		}
		for _, run := range runs {
			if run.SourceOffset < 0 || run.SourceOffset >= len(text) {
				t.Fatal("invalid source offset")
			}
			if run.Value != "" && !Identifier(run.Value) {
				t.Fatal("invalid value reference")
			}
		}
	})
}
