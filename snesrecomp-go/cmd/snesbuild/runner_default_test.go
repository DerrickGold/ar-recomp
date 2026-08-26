package main

import (
	"flag"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/project"
)

func TestBuildCommandsDefaultToIndependentRunner(t *testing.T) {
	buildFlags := flag.NewFlagSet("build", flag.ContinueOnError)
	build := addBuildFlags(buildFlags)
	if err := buildFlags.Parse(nil); err != nil {
		t.Fatal(err)
	}
	if build.runner != project.RunnerNext {
		t.Fatalf("build runner = %q, want %q", build.runner, project.RunnerNext)
	}

	allFlags := flag.NewFlagSet("all", flag.ContinueOnError)
	all := addBuildFlagsForAll(allFlags)
	if err := allFlags.Parse(nil); err != nil {
		t.Fatal(err)
	}
	if all.runner != project.RunnerNext {
		t.Fatalf("all runner = %q, want %q", all.runner, project.RunnerNext)
	}
}
