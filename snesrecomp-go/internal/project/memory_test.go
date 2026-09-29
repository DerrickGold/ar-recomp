package project

import (
	"runtime"
	"testing"
)

func TestParseMemoryBudget(t *testing.T) {
	for input, want := range map[string]int64{
		"":           0,
		"auto":       0,
		"AUTO":       0,
		"off":        -1,
		"0":          -1,
		"none":       -1,
		"6442450944": 6442450944,
		"6G":         6 << 30,
		"6g":         6 << 30,
		"6GiB":       6 << 30,
		"6GB":        6 << 30,
		"1.5G":       3 << 29,
		"6144M":      6 << 30,
		"512MiB":     512 << 20,
		"64K":        64 << 10,
		"1T":         1 << 40,
		"4096B":      4096,
		" 2G ":       2 << 30,
	} {
		got, err := ParseMemoryBudget(input)
		if err != nil || got != want {
			t.Errorf("ParseMemoryBudget(%q) = %d, %v; want %d", input, got, err, want)
		}
	}
	for _, input := range []string{"-1", "-2G", "six", "6X", "6GiBs", "G", "0.1B", "1e30T", "NaN"} {
		if got, err := ParseMemoryBudget(input); err == nil {
			t.Errorf("ParseMemoryBudget(%q) = %d, want an error", input, got)
		}
	}
}

func TestDefaultCompileMemoryBudgetIsHalfOfPhysicalMemory(t *testing.T) {
	budget, physical := defaultCompileMemoryBudget()
	switch runtime.GOOS {
	case "darwin", "linux", "windows":
		if physical <= 0 {
			t.Fatalf("physical memory undetected on %s", runtime.GOOS)
		}
	default:
		if physical != 0 || budget != 0 {
			t.Fatalf("budget %d from undetectable memory %d", budget, physical)
		}
		return
	}
	if budget != physical/2 {
		t.Fatalf("budget %d from physical %d", budget, physical)
	}
	// No real build machine has less than 256 MiB or more than 64 TiB.
	if physical < 256<<20 || physical > 64<<40 {
		t.Fatalf("implausible physical memory %d", physical)
	}
}
