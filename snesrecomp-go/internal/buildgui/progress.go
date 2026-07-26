package buildgui

import (
	"html"
	"strconv"
	"strings"
)

// The builder's phase model.
//
// The build already narrates itself on stdout: project.Regenerate emits
// "\n=== Title ===\n" banners (its step() helper) and project.HermeticBuild
// emits "hermetic: ..." lines. Rather than thread a progress callback through
// both packages -- which would put UI concerns inside the build internals and
// give the GUI a second, redundant source of truth -- the GUI derives its
// phase and percentage from that same narration.
//
// The parser is deliberately a pure function over the accumulated log so it is
// directly unit-testable without a toolchain, a ROM, or an HTTP server. It is
// also intentionally forgiving: an unrecognised log line only means the bar
// does not advance, never a wrong or backwards reading.

// phase is one user-visible step of a build.
type phase struct {
	// id is the stable key the page uses to render the step list.
	id string
	// label is the human-readable step name.
	label string
	// banner is the `=== ... ===` step-banner title prefix that means this
	// phase has STARTED. Empty when the phase is not announced by a banner
	// (the hermetic half of the build narrates itself differently) and is
	// instead entered by an explicit rule in computeProgress.
	banner string
	// weight is this phase's share of the overall bar. Weights are relative;
	// they need not sum to any particular value. They approximate observed
	// wall-clock share, since a build's phases differ by orders of magnitude
	// (bank regeneration and compilation dominate; funcs.h is instant).
	weight float64
}

// buildPhases is the ordered phase list. The two long phases (regeneration and
// compilation) carry most of the weight, so the bar tracks perceived progress
// rather than step count.
//
// Banners must stay in sync with project.Regenerate's step() calls;
// TestPhaseBannersMatchBuildOutput pins the exact strings against the real
// source so a rename upstream fails a test here instead of silently
// flattening the bar.
var buildPhases = []phase{
	{id: "regen", label: "Regenerating banks from your ROM", banner: "Regenerating banks", weight: 30},
	{id: "funcs", label: "Syncing function declarations", banner: "Syncing funcs.h", weight: 1},
	{id: "metadata", label: "Refreshing generated-code metadata", banner: "Refreshing generated-code metadata", weight: 2},
	{id: "rts", label: "Auditing return-site coverage", banner: "RTS-web census", weight: 3},
	{id: "stubs", label: "Checking for unimplemented code", banner: "Hard stub census", weight: 2},
	{id: "compile", label: "Compiling the game", banner: "", weight: 50},
	{id: "link", label: "Linking the executable", banner: "", weight: 8},
	{id: "install", label: "Installing your playable game", banner: "", weight: 4},
}

// progress is the GUI's view of how far a build has got. It is recomputed from
// the log on every status poll, so it holds no state of its own.
type progress struct {
	// PhaseID is the phase currently running (or the last one reached).
	PhaseID string `json:"phaseId"`
	// PhaseLabel is that phase's human-readable name.
	PhaseLabel string `json:"phaseLabel"`
	// PhaseIndex is its 0-based position, and PhaseCount the total, so the
	// page can render "step 3 of 8" without duplicating the phase table.
	PhaseIndex int `json:"phaseIndex"`
	PhaseCount int `json:"phaseCount"`
	// Percent is the overall completion estimate, 0..100.
	Percent int `json:"percent"`
	// Detail is an optional sub-status for the current phase, e.g. the
	// compiled-object count. Empty when there is nothing extra to say.
	Detail string `json:"detail,omitempty"`
	// Units and UnitsTotal expose real sub-progress within the compile
	// phase (compiled translation units vs. the total). Both are 0 when the
	// current phase has no countable work.
	Units      int `json:"units,omitempty"`
	UnitsTotal int `json:"unitsTotal,omitempty"`
	// Completed lists the phase ids already finished, so the page can tick
	// them off without inferring completion from PhaseIndex (the two differ
	// when a build fails partway).
	Completed []string `json:"completed"`
}

// renderStepList builds the static step-list markup injected into the page, so
// the phase table has exactly one definition (this file) instead of being
// duplicated in JavaScript. Labels are our own constants, but they are escaped
// anyway -- the cost is nil and it keeps the invariant "nothing reaches the
// page unescaped" true without a reader having to verify each label.
func renderStepList() string {
	var builder strings.Builder
	for _, item := range buildPhases {
		builder.WriteString(`<li class="step" data-step="`)
		builder.WriteString(html.EscapeString(item.id))
		builder.WriteString(`"><span class="tick" aria-hidden="true"></span><span class="step-name">`)
		builder.WriteString(html.EscapeString(item.label))
		builder.WriteString(`</span></li>`)
	}
	return builder.String()
}

// totalPhaseWeight is the denominator for the overall percentage.
func totalPhaseWeight() float64 {
	total := 0.0
	for _, item := range buildPhases {
		total += item.weight
	}
	return total
}

// bannerTitle returns the title inside a "=== Title ===" step banner, or ""
// when the line is not a banner.
func bannerTitle(line string) string {
	trimmed := strings.TrimSpace(line)
	if !strings.HasPrefix(trimmed, "=== ") || !strings.HasSuffix(trimmed, " ===") {
		return ""
	}
	return strings.TrimSpace(trimmed[4 : len(trimmed)-4])
}

// compileTotals reads the translation-unit counts out of hermetic.go's
// "hermetic: N translation units (C cached, T to compile, J jobs)" line.
// Returns ok=false for any other line. `cached` units are already done, so
// they count as completed work rather than remaining work.
func compileTotals(line string) (total, cached, toCompile int, ok bool) {
	const prefix = "hermetic: "
	const middle = " translation units ("
	trimmed := strings.TrimSpace(line)
	if !strings.HasPrefix(trimmed, prefix) {
		return 0, 0, 0, false
	}
	rest := trimmed[len(prefix):]
	index := strings.Index(rest, middle)
	if index < 0 {
		return 0, 0, 0, false
	}
	total, err := strconv.Atoi(rest[:index])
	if err != nil || total < 0 {
		return 0, 0, 0, false
	}
	fields := strings.Fields(rest[index+len(middle):])
	// Expect: "C cached, T to compile, J jobs)"
	if len(fields) < 4 || fields[1] != "cached," {
		return 0, 0, 0, false
	}
	cached, err = strconv.Atoi(fields[0])
	if err != nil || cached < 0 {
		return 0, 0, 0, false
	}
	toCompile, err = strconv.Atoi(fields[2])
	if err != nil || toCompile < 0 {
		return 0, 0, 0, false
	}
	return total, cached, toCompile, true
}

// phaseIndexByID finds a phase's position, or -1.
func phaseIndexByID(id string) int {
	for index, item := range buildPhases {
		if item.id == id {
			return index
		}
	}
	return -1
}

// computeProgress derives the phase and percentage from the accumulated build
// log. It is a pure function: same log in, same progress out.
//
// state is the application's own build state ("idle", "building",
// "succeeded", "failed"), which resolves what the log alone cannot -- a log
// ending mid-compile looks identical whether the build is still running or
// died there, and a succeeded build must read 100% even if its final phase
// printed nothing recognisable.
func computeProgress(log, state string) progress {
	current := 0
	// Sub-progress within the compile phase.
	unitsTotal, unitsCached := 0, 0
	compiled := 0
	detail := ""
	installing := false

	for _, line := range strings.Split(log, "\n") {
		if title := bannerTitle(line); title != "" {
			// "Regeneration complete" is regen.go's terminal banner; it
			// means the whole regeneration half is done, so the next
			// phase (compile) is imminent but not yet started.
			if strings.HasPrefix(title, "Regeneration complete") {
				if index := phaseIndexByID("compile"); index > current {
					current = index
				}
				continue
			}
			for index, item := range buildPhases {
				if item.banner != "" && strings.HasPrefix(title, item.banner) {
					if index > current {
						current = index
					}
					break
				}
			}
			continue
		}

		trimmed := strings.TrimSpace(line)
		if !strings.HasPrefix(trimmed, "hermetic:") && !strings.HasPrefix(trimmed, "cc ") &&
			!strings.HasPrefix(trimmed, "Playable game installed") &&
			!strings.HasPrefix(trimmed, "No local Zig toolchain") {
			continue
		}

		// Any hermetic line means compilation has begun.
		if strings.HasPrefix(trimmed, "hermetic:") || strings.HasPrefix(trimmed, "No local Zig toolchain") {
			if index := phaseIndexByID("compile"); index > current {
				current = index
			}
		}
		if total, cached, _, ok := compileTotals(trimmed); ok {
			unitsTotal, unitsCached = total, cached
		}
		// Verbose builds print one "  cc <source>" per translation unit.
		if strings.HasPrefix(trimmed, "cc ") {
			compiled++
		}
		if strings.Contains(trimmed, "compile done in") || strings.Contains(trimmed, "; linking") {
			if index := phaseIndexByID("link"); index > current {
				current = index
			}
		}
		if strings.HasPrefix(trimmed, "hermetic: built ") ||
			strings.HasPrefix(trimmed, "Playable game installed") {
			installing = true
			if index := phaseIndexByID("install"); index > current {
				current = index
			}
		}
	}

	if current >= len(buildPhases) {
		current = len(buildPhases) - 1
	}

	// Fraction of the CURRENT phase that is complete, 0..1. Only the compile
	// phase can measure itself; every other phase reports 0 (just started)
	// because there is nothing honest to count.
	fraction := 0.0
	units, total := 0, 0
	if buildPhases[current].id == "compile" && unitsTotal > 0 {
		units = unitsCached + compiled
		if units > unitsTotal {
			units = unitsTotal
		}
		total = unitsTotal
		// A fully cached build reports "0 to compile" and prints no cc
		// lines, which this ratio already resolves to 1 -- the cached units
		// ARE the finished work, so no special case is needed.
		fraction = float64(units) / float64(unitsTotal)
		detail = strconv.Itoa(units) + " of " + strconv.Itoa(unitsTotal) + " translation units"
	}
	// The installer's own log line is the last thing printed before the build
	// function returns, so once it appears the final phase's work is done even
	// though app.state is still "building" until the goroutine stores the
	// result. Counting it keeps the bar from stalling at 96% on the last step.
	if installing && buildPhases[current].id == "install" {
		fraction = 1
	}

	// Sum the weight of every completed phase, plus the measured fraction of
	// the current one.
	done := 0.0
	completed := make([]string, 0, len(buildPhases))
	for index := 0; index < current; index++ {
		done += buildPhases[index].weight
		completed = append(completed, buildPhases[index].id)
	}
	done += buildPhases[current].weight * fraction

	percent := int(done / totalPhaseWeight() * 100)
	if percent < 0 {
		percent = 0
	}
	if percent > 99 {
		// Reserve 100% for an actually-finished build so the bar never
		// sits full while work continues.
		percent = 99
	}

	switch state {
	case "succeeded":
		percent = 100
		completed = completed[:0]
		for _, item := range buildPhases {
			completed = append(completed, item.id)
		}
		current = len(buildPhases) - 1
		detail = ""
	case "idle":
		if log == "" {
			return progress{
				PhaseID: buildPhases[0].id, PhaseLabel: buildPhases[0].label,
				PhaseIndex: 0, PhaseCount: len(buildPhases),
				Percent: 0, Completed: []string{},
			}
		}
	}

	return progress{
		PhaseID:    buildPhases[current].id,
		PhaseLabel: buildPhases[current].label,
		PhaseIndex: current,
		PhaseCount: len(buildPhases),
		Percent:    percent,
		Detail:     detail,
		Units:      units,
		UnitsTotal: total,
		Completed:  completed,
	}
}
