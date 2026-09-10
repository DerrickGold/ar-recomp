package builder

import (
	"fmt"
	"html"
	"io"
	"strings"
)

// BuildProgress is reported by the ActRaiser orchestration layer after it has
// validated a versioned snesbuild event. The browser never parses subprocess
// log prose.
type BuildProgress struct {
	PhaseID   string
	Message   string
	Completed int
	Total     int
}

// ProgressReporter is implemented by the GUI's build-log writer. Keeping this
// as a narrow writer extension preserves the simple Build callback while
// giving external orchestrators a structured progress path.
type ProgressReporter interface {
	ReportBuildProgress(BuildProgress)
}

// ReportBuildProgress publishes structured progress when output belongs to a
// Builder session. Other writers still receive build logs without progress.
func ReportBuildProgress(output io.Writer, update BuildProgress) {
	if reporter, ok := output.(ProgressReporter); ok {
		reporter.ReportBuildProgress(update)
	}
}

type phase struct {
	id     string
	label  string
	weight float64
}

var buildPhases = []phase{
	{id: "localization", label: "Preparing the native US language source", weight: 5},
	{id: "regen", label: "Regenerating banks from your ROM", weight: 30},
	{id: "toolchain", label: "Preparing the build toolchain", weight: 5},
	{id: "compile", label: "Compiling and linking the game", weight: 55},
	{id: "install", label: "Installing your playable game", weight: 5},
}

type progress struct {
	PhaseID    string   `json:"phaseId"`
	PhaseLabel string   `json:"phaseLabel"`
	PhaseIndex int      `json:"phaseIndex"`
	PhaseCount int      `json:"phaseCount"`
	Percent    int      `json:"percent"`
	Detail     string   `json:"detail,omitempty"`
	Units      int      `json:"units,omitempty"`
	UnitsTotal int      `json:"unitsTotal,omitempty"`
	Completed  []string `json:"completed"`
}

func initialProgress() progress {
	return progress{
		PhaseID: buildPhases[0].id, PhaseLabel: buildPhases[0].label,
		PhaseCount: len(buildPhases), Completed: []string{},
	}
}

func renderStepList() string {
	var builder strings.Builder
	for _, item := range buildPhases {
		builder.WriteString(`<li class="step" data-step="`)
		builder.WriteString(html.EscapeString(item.id))
		builder.WriteString(`"><span class="tick" aria-hidden="true"></span><span class="step-name" data-i18n="builder.phase.`)
		builder.WriteString(html.EscapeString(item.id))
		builder.WriteString(`">`)
		builder.WriteString(html.EscapeString(item.label))
		builder.WriteString(`</span></li>`)
	}
	return builder.String()
}

func totalPhaseWeight() float64 {
	var total float64
	for _, item := range buildPhases {
		total += item.weight
	}
	return total
}

func progressFromEvent(previous progress, update BuildProgress) progress {
	index := -1
	for candidate, item := range buildPhases {
		if item.id == update.PhaseID {
			index = candidate
			break
		}
	}
	if index < 0 || index < previous.PhaseIndex {
		return previous
	}
	result := progress{
		PhaseID: update.PhaseID, PhaseLabel: buildPhases[index].label,
		PhaseIndex: index, PhaseCount: len(buildPhases), Completed: []string{},
	}
	for completed := 0; completed < index; completed++ {
		result.Completed = append(result.Completed, buildPhases[completed].id)
	}
	var done float64
	for completed := 0; completed < index; completed++ {
		done += buildPhases[completed].weight
	}
	fraction := 0.0
	if update.Total > 0 {
		completed := update.Completed
		if completed < 0 {
			completed = 0
		}
		if completed > update.Total {
			completed = update.Total
		}
		fraction = float64(completed) / float64(update.Total)
		result.Units, result.UnitsTotal = completed, update.Total
		result.Detail = fmt.Sprintf("%d of %d", completed, update.Total)
	}
	result.Percent = int((done + buildPhases[index].weight*fraction) / totalPhaseWeight() * 100)
	if result.Percent > 99 {
		result.Percent = 99
	}
	if result.Percent < previous.Percent {
		result.Percent = previous.Percent
	}
	return result
}

func completedProgress() progress {
	result := initialProgress()
	last := len(buildPhases) - 1
	result.PhaseID = buildPhases[last].id
	result.PhaseLabel = buildPhases[last].label
	result.PhaseIndex = last
	result.Percent = 100
	result.Completed = make([]string, 0, len(buildPhases))
	for _, item := range buildPhases {
		result.Completed = append(result.Completed, item.id)
	}
	return result
}
