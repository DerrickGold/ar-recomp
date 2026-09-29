package main

import (
	"flag"
	"io"
	"strings"
	"testing"
)

func TestBuildFlagsCarryTheMemoryBudget(t *testing.T) {
	parse := func(args ...string) (int64, error) {
		t.Helper()
		flags := flag.NewFlagSet("build", flag.ContinueOnError)
		values := addBuildFlags(flags)
		if err := flags.Parse(append([]string{"--zig", "zig"}, args...)); err != nil {
			t.Fatal(err)
		}
		options, err := values.hermeticOptionsWithWriters(io.Discard, io.Discard)
		return options.MemoryBudget, err
	}
	for args, want := range map[string]int64{"": 0, "--memory-budget=6GiB": 6 << 30, "--memory-budget=off": -1} {
		got, err := parse(strings.Fields(args)...)
		if err != nil || got != want {
			t.Errorf("%q: budget %d, %v; want %d", args, got, err, want)
		}
	}
	if _, err := parse("--memory-budget=lots"); err == nil || !strings.Contains(err.Error(), "memory budget") {
		t.Fatalf("an invalid budget was accepted: %v", err)
	}
}
