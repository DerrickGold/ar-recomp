package project

import (
	"fmt"
	"math"
	"strconv"
	"strings"
)

// defaultCompileMemoryBudget is half of the detected physical memory (or of a
// smaller container limit): the other half is left to the operating system,
// the linker, and whatever else is running. It returns zero, meaning no
// budget, when memory cannot be determined on this platform.
func defaultCompileMemoryBudget() (budget, physical int64) {
	total, ok := physicalMemoryBytes()
	if !ok || total <= 0 {
		return 0, 0
	}
	return total / 2, total
}

// ParseMemoryBudget reads a --memory-budget value into HermeticOptions form:
// "auto" is 0 (the default budget), "off" or "0" is -1 (no budget), and
// anything else is a positive size in bytes with an optional binary suffix,
// K, M, G or T, alone or followed by "iB" or "B"; every suffix is a power of
// 1024. Examples: "6G", "6GiB", "6144M", "6442450944".
func ParseMemoryBudget(text string) (int64, error) {
	value := strings.TrimSpace(text)
	switch strings.ToLower(value) {
	case "", "auto":
		return 0, nil
	case "off", "none", "0":
		return -1, nil
	}
	number := strings.TrimRight(value, "BbIi")
	shift := 0
	if number != "" {
		switch number[len(number)-1] {
		case 'K', 'k':
			shift = 10
		case 'M', 'm':
			shift = 20
		case 'G', 'g':
			shift = 30
		case 'T', 't':
			shift = 40
		}
	}
	suffix := value[len(number):]
	if shift != 0 {
		number = number[:len(number)-1]
		switch strings.ToLower(suffix) {
		case "", "b", "ib":
		default:
			return 0, fmt.Errorf("memory budget %q: unknown size suffix", text)
		}
	} else if suffix != "" && !strings.EqualFold(suffix, "b") {
		return 0, fmt.Errorf("memory budget %q: unknown size suffix", text)
	}
	amount, err := strconv.ParseFloat(strings.TrimSpace(number), 64)
	if err != nil || amount <= 0 || math.IsInf(amount, 0) || math.IsNaN(amount) {
		return 0, fmt.Errorf("memory budget %q: want auto, off, or a positive size such as 6GiB", text)
	}
	bytes := amount * float64(uint64(1)<<shift)
	if bytes >= math.MaxInt64 {
		return 0, fmt.Errorf("memory budget %q is too large", text)
	}
	if bytes < 1 {
		return 0, fmt.Errorf("memory budget %q is smaller than one byte", text)
	}
	return int64(bytes), nil
}
