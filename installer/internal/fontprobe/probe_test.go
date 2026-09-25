package fontprobe

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	lk "github.com/DerrickGold/ar-recomp/installer/internal/localization"
)

func TestResponseLineEndingsAndValidation(t *testing.T) {
	for _, test := range []struct {
		name, output string
		valid        bool
	}{
		{"LF", "0041\t1\n0042\t0\n", true},
		{"CRLF", "0041\t1\r\n0042\t0\r\n", true},
		{"mixed endings", "0041\t1\r\n0042\t0\n", true},
		{"no final newline", "0041\t1\n0042\t0", true},
		{"bare CR", "0041\t1\n0042\t0\r", false},
		{"extra CR", "0041\t1\r\r\n0042\t0\n", false},
		{"spaces", "0041\t1 \n0042\t0\n", false},
		{"wrong scalar", "0042\t1\n0041\t0\n", false},
		{"bad coverage", "0041\t2\n0042\t0\n", false},
		{"extra field", "0041\t1\textra\n0042\t0\n", false},
		{"missing record", "0041\t1\n", false},
		{"extra record", "0041\t1\n0042\t0\n\n", false},
	} {
		t.Run(test.name, func(t *testing.T) {
			got, err := parseResponse(test.output, []rune{'A', 'B'})
			if test.valid {
				if err != nil || !reflect.DeepEqual(got, []bool{true, false}) {
					t.Fatalf("coverage %v, error %v", got, err)
				}
			} else if !errors.Is(err, ErrProtocol) || got != nil {
				t.Fatalf("malformed response published coverage: %v, %v", got, err)
			}
		})
	}
	if got, err := parseResponse("", nil); err != nil || len(got) != 0 {
		t.Fatalf("empty request: %v, %v", got, err)
	}
	_, err := parseResponse("0041\t1\n0042\tbad\n", []rune{'A', 'B'})
	if !strings.Contains(err.Error(), "record 2 for U+0042") {
		t.Fatal("missing diagnostic location", err)
	}
}

func TestUnavailableExecutableRetainsCause(t *testing.T) {
	dir := t.TempDir()
	font := filepath.Join(dir, "font.ttf")
	if err := os.WriteFile(font, []byte("immutable font fixture"), 0600); err != nil {
		t.Fatal(err)
	}
	_, err := Run(context.Background(), filepath.Join(dir, "missing-game"), font,
		[]lk.FontCoverageSource{{Reference: "builtin:actraiser-sans"}}, []rune{'A'})
	if !errors.Is(err, ErrUnavailable) || !errors.Is(err, os.ErrNotExist) {
		t.Fatal("missing executable lost its category or cause", err)
	}
}
