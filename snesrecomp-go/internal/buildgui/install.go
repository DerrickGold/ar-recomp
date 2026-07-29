package buildgui

// Install-state detection: what this copy of the bundle can currently DO.
//
// The builder used to assume every session began with nothing built. It opened
// on a ROM picker, and "Launch" only became available after a build finished in
// that same process -- so reopening it beside a perfectly good game offered no
// way to play, and no way to tell that a game was already there.
//
// Two capabilities matter, and they are INDEPENDENT because they depend on
// different files:
//
//   - CAN LAUNCH: the output artifacts exist (the game binary and its
//     launcher). Nothing about the toolchain matters here.
//   - CAN REBUILD: the build INPUTS exist (the recompiler config, the authored
//     sources, the runtime, and snesbuild itself).
//
// They diverge, and that divergence is the point. A user who has finished
// building may reasonably delete the toolchain -- it is the overwhelming
// majority of the bundle's size and is needed only for future rebuilds. After
// that the game still runs, so the GUI must keep working as a LAUNCHER while
// hiding the build affordances entirely rather than offering a button that
// cannot work.
//
// The probes themselves are injected (see Options.Detect) so this package stays
// free of filesystem layout knowledge and remains unit-testable; the real
// implementation lives in cmd/snesbuild, next to the packaging rules that
// decide where those files go.

// InstallState is the GUI's view of what this copy of the bundle can do.
type InstallState struct {
	// CanLaunch is true when a previously built game is present and runnable.
	CanLaunch bool `json:"canLaunch"`
	// CanRebuild is true when every input a rebuild needs is present.
	CanRebuild bool `json:"canRebuild"`
	// CanSlim is true when build-only files are present AND removable, i.e.
	// there is something for the cleanup offer to actually delete. False once
	// the install is already lean, so the offer is not made twice.
	CanSlim bool `json:"canSlim"`
	// SlimBytes is the approximate size the cleanup would reclaim. Zero when
	// unknown; the page then omits the figure rather than claiming "0 B".
	SlimBytes int64 `json:"slimBytes,omitempty"`
	// Result describes the existing game, when CanLaunch. Carrying it here is
	// what lets Launch work on a freshly opened session: the launch handler
	// needs an OutputPath, and without a build in this process there is no
	// other way to obtain one.
	Result Result `json:"result"`
}

// mode names the shape the page should take. Derived rather than stored so the
// page and the server cannot disagree about which UI is showing.
//
// The ordering matters: a lean install (built, tools gone) must report
// "launcher" and not merely "a build state with rebuild disabled", because the
// build affordances are hidden entirely in that mode.
func (state InstallState) mode() string {
	switch {
	case state.CanLaunch && !state.CanRebuild:
		// The interesting case: a slimmed install. Play and read the manual;
		// nothing about building is shown, because nothing about it can work.
		return "launcher"
	case state.CanLaunch:
		return "ready"
	case state.CanRebuild:
		return "buildable"
	default:
		// Neither possible. Rare and worth stating plainly rather than
		// presenting a dead ROM picker: it means the copy is incomplete.
		return "unusable"
	}
}

// slimSummary renders the reclaimable size for the cleanup offer. Deliberately
// coarse -- this is a "worth doing?" figure, not an accounting statement -- and
// it degrades to an empty string when the size is unknown so the caller can
// omit the clause entirely instead of printing a misleading zero.
func slimSummary(bytes int64) string {
	const (
		mib = 1 << 20
		gib = 1 << 30
	)
	switch {
	case bytes >= gib:
		return formatDecimal(bytes, gib, "GB")
	case bytes >= mib:
		return formatDecimal(bytes, mib, "MB")
	default:
		return ""
	}
}

// formatDecimal renders bytes/unit with one decimal place, without importing a
// formatting dependency or rounding through float64 imprecision at these
// magnitudes.
func formatDecimal(value, unit int64, suffix string) string {
	whole := value / unit
	tenths := (value % unit) * 10 / unit
	out := itoa(whole)
	if tenths > 0 {
		out += "." + itoa(tenths)
	}
	return out + " " + suffix
}

func itoa(value int64) string {
	if value == 0 {
		return "0"
	}
	var digits [20]byte
	index := len(digits)
	for value > 0 {
		index--
		digits[index] = byte('0' + value%10)
		value /= 10
	}
	return string(digits[index:])
}
