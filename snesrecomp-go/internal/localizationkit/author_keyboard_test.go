package localizationkit

import (
	"fmt"
	"os"
	"reflect"
	"strings"
	"testing"
)

const authorKeyboardPage = "Test keyboard\n@line\n{master_name}\n@line\n--------\n@line\n" +
	"A B C D E F G H I J K L M\n@line\nN O P Q R S T U V W X Y Z\n@line\n" +
	"a b c d e f g h i j k l m\n@line\nn o p q r s t u v w x y z\n@line\n" +
	"0 1 2 3 4 5 6 7 8 9 . {icon.name_entry.backspace} {icon.name_entry.finish}\n"

func TestAuthorKeyboardPages(t *testing.T) {
	probe := os.Getenv("AR_AUTHOR_RUNTIME_PROBE")
	cases := []struct{ body, diagnostic string }{
		{authorKeyboardPage, ""},
		{"@empty\n", ""},
		{authorKeyboardPage + "@page\n" + authorKeyboardPage, ""},
		{strings.ReplaceAll(authorKeyboardPage, "@line\n", "@paragraph\n"), ""},
		{strings.Replace(authorKeyboardPage, "A B C", "AB C", 1), "requires exactly 13"}, // the renderer needs a gutter between keys
		{strings.Replace(authorKeyboardPage, "A B C", "A B", 1), "page 1 row 1 requires exactly 13"},
		{authorKeyboardPage + "@page\n" + strings.Replace(authorKeyboardPage, "a b c", "a b", 1), "page 2 row 3 requires exactly 13"},
		{strings.Replace(authorKeyboardPage, "{master_name}", "Forgotten", 1), "put {master_name} alone"},
		{strings.Replace(authorKeyboardPage, "--------", "Oops: erased text", 1), "dash-only underline slot"},
		{strings.Replace(authorKeyboardPage, ". {icon.name_entry.backspace}", "{icon.name_entry.backspace} .", 1), "final two key positions"},
		{strings.Replace(authorKeyboardPage, "{icon.name_entry.finish}", ".", 1), "final two key positions"},
		{strings.Replace(authorKeyboardPage, "{master_name}", "{master_name}{master_name}", 1), "repeats {master_name}"},
		{"@page\n" + authorKeyboardPage, "page 1: requires"},
		{authorKeyboardPage + "@page\n", "page 2: requires"},
		{strings.Repeat("Preface\n@line\n", 56) + authorKeyboardPage, "documented line limit"},
		{strings.Repeat("x", 3073) + authorKeyboardPage, "exceeds 3072"},
	}
	for _, key := range []string{"é", "e\u0301", "نَ", "각", "का", "؀ن", "क्ष", "👩‍👩‍👧", "👩🏽‍💻", "🇨🇦", "1️⃣", "字", "\ufffc"} {
		cases = append(cases, struct{ body, diagnostic string }{strings.Replace(authorKeyboardPage, "A B", key+" B", 1), ""})
		cases = append(cases, struct{ body, diagnostic string }{strings.Replace(authorKeyboardPage, "A B", key+"X B", 1), "requires exactly 13"})
	}
	for i, tc := range cases {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			sources := map[string]string{"text/keys.artext": ":: name_entry.prompt_and_alphabet\n" + tc.body}
			_, err := NewAuthorWorkspace("us", "partial", sources, "")
			if tc.diagnostic == "" && err != nil || tc.diagnostic != "" && (err == nil || !strings.Contains(err.Error(), tc.diagnostic)) {
				t.Fatalf("expected %q, got %v", tc.diagnostic, err)
			}
			if probe != "" {
				authorRuntimeCheck(t, probe, "us", "partial", sources)
			}
		})
	}
}

func TestAuthorGraphemeBoundaries(t *testing.T) {
	// Same UAX #29 boundary cases as the portable game's Unicode test. The
	// keyboard tests above compare acceptance with that implementation directly.
	for _, tc := range []struct {
		text string
		ends []int
	}{
		{"abc", []int{1, 2, 3}}, {"\r\na", []int{2, 3}}, {"e\u0301x", []int{3, 4}},
		{"نَص", []int{4, 6}}, {"각x", []int{9, 10}}, {"काx", []int{6, 7}},
		{"؀نx", []int{4, 5}}, {"क्षx", []int{9, 10}}, {"👩‍👩‍👧x", []int{18, 19}},
		{"👩🏽‍💻x", []int{15, 16}}, {"🇨🇦🇫🇷x", []int{8, 16, 17}}, {"1️⃣x", []int{7, 8}},
	} {
		var ends []int
		for offset := 0; offset < len(tc.text); {
			next, ok := nextGrapheme(tc.text, offset)
			if !ok {
				t.Fatal("unexpected invalid UTF-8")
			}
			ends = append(ends, next)
			offset = next
		}
		if !reflect.DeepEqual(ends, tc.ends) {
			t.Fatalf("%q: %v != %v", tc.text, ends, tc.ends)
		}
	}
	for _, text := range []string{"\xc0\xaf", "\xed\xa0\x80", "\xf0\x9f\x91", "x\xff"} {
		if _, ok := nextGrapheme(text, 0); ok {
			t.Fatal("invalid UTF-8 accepted")
		}
	}
}
