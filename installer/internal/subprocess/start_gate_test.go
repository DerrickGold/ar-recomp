package subprocess

import (
	"strings"
	"testing"
)

func TestDesktopGateRequiresHostApproval(t *testing.T) {
	for _, input := range []string{"", "0", "\x01"} {
		err := WaitForDesktopGate(strings.NewReader(input))
		if (err == nil) != (input == "\x01") {
			t.Errorf("gate %q: %v", input, err)
		}
	}
}
