package localization

import "strings"

// Reporting only: acceptance still uses ValidateAuthorScripts. "Provided" is
// not a claim of translation quality, review, font coverage or runtime fit.
// Contract entries include shared terms and references, not just screen labels.
type AuthorCoverageCount struct {
	Total    int      `json:"total"`
	Provided int      `json:"provided"`
	Missing  []string `json:"missing"`
}

type AuthorSurfaceCoverage struct {
	Surface string `json:"surface"`
	AuthorCoverageCount
	NotStarted      int `json:"notStarted"`
	WIP             int `json:"wip"`
	Done            int `json:"done"`
	UnchangedSource int `json:"unchangedSource"`
}

type AuthorCoverageReport struct {
	Profile          string                  `json:"profile"`
	Runtime          bool                    `json:"runtime"`
	ContractComplete bool                    `json:"contractComplete"`
	Required         AuthorCoverageCount     `json:"required"`
	LiveOptional     AuthorCoverageCount     `json:"liveOptional"`
	Surfaces         []AuthorSurfaceCoverage `json:"surfaces"`
	Dormant          []string                `json:"dormant"`
}

type RuntimeCoverageCount struct {
	Provided int `json:"provided"`
	Total    int `json:"total"`
}

type RuntimeSurfaceCoverage struct {
	Surface string `json:"surface"`
	RuntimeCoverageCount
}

type RuntimeTextCoverage struct {
	RuntimeCoverageCount
	Surfaces []RuntimeSurfaceCoverage `json:"surfaces"`
}

var coverageSurfaces = [...]string{"dialogue", "menus", "keyboard", "terms", "hud", "credits"}

// RuntimeTextCoverage measures the installed archive against the live US
// catalogue. Opening a publication can synthesize editor progress, so this
// report only checks presence, including aliases and intentional empty entries.
func (p *AuthorPack) RuntimeTextCoverage() RuntimeTextCoverage {
	counts := make(map[string]RuntimeCoverageCount, len(coverageSurfaces))
	var report RuntimeTextCoverage
	for _, ref := range p.workspace.references {
		if !ref.NativeInProfile || (ref.USRuntimeUsage != "contract" && ref.USRuntimeUsage != "live_optional") {
			continue
		}
		surface := coverageSurface(ref)
		count := counts[surface]
		count.Total++
		report.Total++
		if _, present := p.workspace.messageScript[ref.ID]; present {
			count.Provided++
			report.Provided++
		}
		counts[surface] = count
	}
	for _, surface := range coverageSurfaces {
		if count := counts[surface]; count.Total != 0 {
			report.Surfaces = append(report.Surfaces, RuntimeSurfaceCoverage{surface, count})
		}
	}
	return report
}

func coverageSurface(ref AuthorReference) string {
	switch {
	case strings.HasPrefix(ref.ID, "credits."):
		return "credits"
	case strings.HasPrefix(ref.ID, "action.hud.") || strings.HasPrefix(ref.ID, "sim_sky.hud."):
		return "hud"
	case ref.Presentation.Shape == "keyboard":
		return "keyboard"
	case ref.Presentation.Shape == "inline":
		return "terms"
	case ref.Presentation.Shape == "fixed":
		return "menus"
	default:
		return "dialogue"
	}
}

func (c *AuthorCoverageCount) add(id string, present bool) {
	c.Total++
	if present {
		c.Provided++
	} else {
		c.Missing = append(c.Missing, id)
	}
}

// Coverage is a detached inventory for explicit author review/CLI use, never
// a per-frame/per-message runtime scan. Status and baseline comparisons remain
// separate from supplied content, including @empty and resolved aliases.
func (p *AuthorProject) Coverage() AuthorCoverageReport { return p.coverageFor(nil) }

// Publication passes the actual filtered selection while retaining the
// author's original review status; WIP is not relabelled Done in this report.
func (p *AuthorProject) coverageFor(included map[string]bool) AuthorCoverageReport {
	m := p.pack.manifest.metadata
	r := AuthorCoverageReport{Profile: m.SourceProfile, Runtime: m.Target == "us-runtime" && m.SourceProfile == "us"}
	refs, _ := AuthorReferences(m.SourceProfile)
	indices := map[string]int{}
	for _, surface := range coverageSurfaces {
		indices[surface] = len(r.Surfaces)
		r.Surfaces = append(r.Surfaces, AuthorSurfaceCoverage{Surface: surface})
	}
	for _, ref := range refs {
		if !ref.NativeInProfile {
			continue
		}
		_, present := p.pack.workspace.messageScript[ref.ID]
		if included != nil {
			present = included[ref.ID]
		}
		if ref.RequiredForComplete {
			r.Required.add(ref.ID, present)
		}
		if r.Runtime && ref.USRuntimeUsage == "live_optional" {
			r.LiveOptional.add(ref.ID, present)
		}
		if r.Runtime && ref.USRuntimeUsage == "dormant" {
			r.Dormant = append(r.Dormant, ref.ID)
		}
		surface := &r.Surfaces[indices[coverageSurface(ref)]]
		surface.add(ref.ID, present)
		if !present {
			continue
		}
		switch p.pack.workspace.progress.statuses[ref.ID] {
		case TranslationDone:
			surface.Done++
		case TranslationWIP:
			surface.WIP++
		default:
			surface.NotStarted++
		}
		if baseline, ok := p.info.Baseline[ref.ID]; ok {
			if ops, err := resolvedAuthorOperations(p.pack.workspace, ref.ID); err == nil && presentationDigest(ops) == baseline {
				surface.UnchangedSource++
			}
		}
	}
	r.ContractComplete = r.Required.Total == r.Required.Provided
	return r
}
