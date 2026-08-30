package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"strings"

	"github.com/DerrickGold/snesrecomp-go/internal/tooling"
)

func runReplayBench(args []string) error {
	flags := flag.NewFlagSet("replay-bench", flag.ContinueOnError)
	root := flags.String("root", ".", "game project root")
	suite := flags.String("suite", "replay-bench.json", "benchmark suite manifest, relative to project root")
	binary := flags.String("binary", "", "candidate game binary, relative to project root")
	referenceBinary := flags.String("reference-binary", "", "optional frozen reference binary for adjacent A/B pairs")
	rom := flags.String("rom", "game.sfc", "ROM path, relative to project root")
	config := flags.String("config", "config.ini", "game configuration path, relative to project root")
	save := flags.String("save", "", "fallback save seed for workloads without save_seed")
	runs := flags.Int("runs", 7, "measured runs per workload")
	warmups := flags.Int("warmups", 1, "warmup runs per workload and binary")
	var workloads stringList
	flags.Var(&workloads, "workload", "workload name (repeatable; defaults come from the suite)")
	label := flags.String("label", "runner-baseline", "result label")
	output := flags.String("output", "", "optional JSON result path, relative to project root")
	compare := flags.String("compare", "", "optional prior JSON result to compare")
	maximumRegression := flags.Float64("max-regression-percent", 5, "maximum per-workload median regression")
	maximumSuiteRegression := flags.Float64("max-suite-regression-percent", 3, "maximum full-suite geometric-mean regression")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if strings.TrimSpace(*binary) == "" {
		return errors.New("replay-bench requires --binary")
	}
	if *runs < 1 || *warmups < 0 || *maximumRegression < 0 || *maximumSuiteRegression < 0 {
		return errors.New("replay-bench run counts and regression limits must be non-negative (runs at least 1)")
	}
	result, passed, err := tooling.BuildReplayBenchmark(tooling.ReplayBenchmarkOptions{
		Root: *root, SuitePath: *suite, BinaryPath: *binary, ReferenceBinaryPath: *referenceBinary,
		ROMPath: *rom, ConfigPath: *config, FallbackSavePath: *save,
		Runs: *runs, Warmups: *warmups, SelectedWorkloads: workloads,
		Label: *label, OutputPath: *output, ComparePath: *compare,
		MaxRegressionPercent: *maximumRegression, MaxSuiteRegressionPercent: *maximumSuiteRegression,
	}, os.Stdout)
	if err != nil {
		return err
	}
	tooling.WriteReplayBenchmarkSummary(result, passed, os.Stdout)
	if !passed {
		return fmt.Errorf("replay benchmark exceeded an equivalence or performance gate")
	}
	return nil
}
