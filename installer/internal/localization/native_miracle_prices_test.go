package localization

import (
	"strings"
	"testing"
)

func TestNativeMiraclePrice(t *testing.T) {
	for name, literals := range map[string][]string{
		"lightning": {"10", "12"}, "rain": {"20", "16"}, "sun": {"30", "18"},
		"wind": {"80", "24"}, "earthquake": {"160", "60"},
	} {
		for _, number := range literals {
			id := "sim.miracle." + name + ".insufficient_sp"
			input := []AuthorOperation{{Op: "text", Value: "Cost " + number + " SP"}, {Op: "end"}}
			out, err := nativeMiraclePrice(input, id)
			if err != nil || len(out) != 4 || out[1].Op != "placeholder" || !strings.HasSuffix(out[1].Name, "_sp") {
				t.Fatal(name, out, err)
			}
			if input[0].Value != "Cost "+number+" SP" {
				t.Fatal("mutated extraction input")
			}
			for _, broken := range []string{"no number", "42", number + " and " + number} {
				if _, err := nativeMiraclePrice([]AuthorOperation{{Op: "text", Value: broken}}, id); err == nil {
					t.Fatal("accepted ambiguous price", broken)
				}
			}
		}
	}
	input := []AuthorOperation{{Op: "text", Value: "Cost 10 SP"}}
	out, err := nativeMiraclePrice(input, "sim.miracle.lightning.description")
	if err != nil || len(out) != 1 || out[0] != input[0] {
		t.Fatal("rewrote unrelated text")
	}
}
