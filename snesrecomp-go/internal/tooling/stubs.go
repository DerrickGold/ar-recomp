package tooling

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

var (
	stubGotoRE     = regexp.MustCompile(`cpu_trace_unresolved_goto_trap\(cpu,\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),\s*"([^"]+)",\s*"([^"]+)"\)`)
	stubDispatchRE = regexp.MustCompile(`cpu_trace_dispatch_oob\(cpu,\s*0x([0-9A-Fa-f]+)`)
	stubTargetRE   = regexp.MustCompile(`cpu_trace_unresolved_stub_trap\(cpu,\s*0x([0-9A-Fa-f]+)`)
	stubIndirectRE = regexp.MustCompile(`cpu_trace_unresolved_indirect_jump\(cpu,\s*0x([0-9A-Fa-f]+)`)
	stubVariantRE  = regexp.MustCompile(`_M[01]X[01]$`)
	stubFunctionRE = regexp.MustCompile(`^RecompReturn (\w+)\(CpuState \*cpu\) \{`)
	stubBlockRE    = regexp.MustCompile(`^\s*(L_[0-9A-Fa-f]+_M[01]X[01]):`)
)

// Entries describe emitted diagnostics, not proven reachable defects. A
// dispatch-domain guard or a missing-registry fallback can be intentional.
// Keeping the owning function/block-entry widths makes a baseline useful without
// mistaking unchanged totals for unchanged coverage.
type StubCensusEntry struct {
	Key       string   `json:"key"`
	Emissions int      `json:"emissions"`
	Contexts  []string `json:"contexts"`
}

type StubCensusReport struct {
	Version           int
	LogicalGotos      int
	GotoEmissions     int
	LogicalDispatches int
	DispatchEmissions int
	LogicalTargets    int
	TargetEmissions   int
	LogicalIndirects  int
	IndirectEmissions int
	Entries           []StubCensusEntry
}

func (report StubCensusReport) LogicalTotal() int {
	return report.LogicalGotos + report.LogicalDispatches +
		report.LogicalTargets + report.LogicalIndirects
}

type gotoStubKey struct {
	Site, Target uint32
	Function     string
}

func CensusStubs(genDir string, verbose bool, output io.Writer) (StubCensusReport, error) {
	if output == nil {
		output = io.Discard
	}
	info, err := os.Stat(genDir)
	if err != nil || !info.IsDir() {
		return StubCensusReport{}, fmt.Errorf("stub_census: %s not found (run from repo root)", genDir)
	}
	paths, err := filepath.Glob(filepath.Join(genDir, "bank*_v2.c"))
	if err != nil {
		return StubCensusReport{}, err
	}
	if len(paths) == 0 {
		return StubCensusReport{}, fmt.Errorf("stub_census: no generated bank sources in %s", genDir)
	}
	if path := filepath.Join(genDir, "unresolved_stubs_v2.c"); fileExists(path) {
		paths = append(paths, path)
	}
	sort.Strings(paths)
	gotos := make(map[gotoStubKey]map[string]struct{})
	dispatches := make(map[uint32]map[string]struct{})
	targets := make(map[uint32]map[string]struct{})
	indirects := make(map[uint32]map[string]struct{})
	details := make(map[string]*StubCensusEntry)
	contexts := make(map[string]map[string]bool)
	for _, path := range paths {
		file, err := os.Open(path)
		if err != nil {
			return StubCensusReport{}, err
		}
		scanner := bufio.NewScanner(file)
		lineNumber := 0
		function, block := "", ""
		for scanner.Scan() {
			lineNumber++
			line := scanner.Text()
			if match := stubFunctionRE.FindStringSubmatch(line); match != nil {
				function, block = match[1], "entry"
			}
			if match := stubBlockRE.FindStringSubmatch(line); match != nil {
				block = match[1]
			}
			addDetail := func(key string) {
				if details[key] == nil {
					details[key] = &StubCensusEntry{Key: key}
					contexts[key] = make(map[string]bool)
				}
				details[key].Emissions++
				contexts[key][function+"/"+block] = true
			}
			if match := stubGotoRE.FindStringSubmatch(line); match != nil {
				site, _ := strconv.ParseUint(match[1], 16, 32)
				target, _ := strconv.ParseUint(match[2], 16, 32)
				key := gotoStubKey{Site: uint32(site), Target: uint32(target), Function: stubVariantRE.ReplaceAllString(match[3], "")}
				addDetail(fmt.Sprintf("goto:%06X:%06X:%s", site, target, key.Function))
				if gotos[key] == nil {
					gotos[key] = make(map[string]struct{})
				}
				gotos[key][fmt.Sprintf("%s:%d", filepath.Base(path), lineNumber)] = struct{}{}
			}
			if match := stubDispatchRE.FindStringSubmatch(line); match != nil {
				site, _ := strconv.ParseUint(match[1], 16, 32)
				key := uint32(site)
				addDetail(fmt.Sprintf("dispatch:%06X", key))
				if dispatches[key] == nil {
					dispatches[key] = make(map[string]struct{})
				}
				dispatches[key][fmt.Sprintf("%s:%d", filepath.Base(path), lineNumber)] = struct{}{}
			}
			if match := stubTargetRE.FindStringSubmatch(line); match != nil {
				target, _ := strconv.ParseUint(match[1], 16, 32)
				key := uint32(target)
				addDetail(fmt.Sprintf("target:%06X", key))
				if targets[key] == nil {
					targets[key] = make(map[string]struct{})
				}
				targets[key][fmt.Sprintf("%s:%d", filepath.Base(path), lineNumber)] = struct{}{}
			}
			if match := stubIndirectRE.FindStringSubmatch(line); match != nil {
				site, _ := strconv.ParseUint(match[1], 16, 32)
				key := uint32(site)
				addDetail(fmt.Sprintf("indirect:%06X", key))
				if indirects[key] == nil {
					indirects[key] = make(map[string]struct{})
				}
				indirects[key][fmt.Sprintf("%s:%d", filepath.Base(path), lineNumber)] = struct{}{}
			}
		}
		scanErr := scanner.Err()
		closeErr := file.Close()
		if scanErr != nil {
			return StubCensusReport{}, scanErr
		}
		if closeErr != nil {
			return StubCensusReport{}, closeErr
		}
	}
	report := StubCensusReport{
		Version:      1,
		LogicalGotos: len(gotos), LogicalDispatches: len(dispatches),
		LogicalTargets: len(targets), LogicalIndirects: len(indirects),
	}
	for key, entry := range details {
		for context := range contexts[key] {
			entry.Contexts = append(entry.Contexts, context)
		}
		sort.Strings(entry.Contexts)
		report.Entries = append(report.Entries, *entry)
	}
	sort.Slice(report.Entries, func(i, j int) bool { return report.Entries[i].Key < report.Entries[j].Key })
	for _, locations := range gotos {
		report.GotoEmissions += len(locations)
	}
	for _, locations := range dispatches {
		report.DispatchEmissions += len(locations)
	}
	for _, locations := range targets {
		report.TargetEmissions += len(locations)
	}
	for _, locations := range indirects {
		report.IndirectEmissions += len(locations)
	}
	fmt.Fprintln(output, "=== STUB CENSUS ===")
	fmt.Fprintf(output, "  unresolved goto traps : %d logical site(s) (%d variant emissions)\n", report.LogicalGotos, report.GotoEmissions)
	fmt.Fprintf(output, "  indirect-dispatch oob : %d logical site(s) (%d variant emissions)\n", report.LogicalDispatches, report.DispatchEmissions)
	fmt.Fprintf(output, "  unresolved targets    : %d logical target(s) (%d emissions)\n", report.LogicalTargets, report.TargetEmissions)
	fmt.Fprintf(output, "  unresolved indirects  : %d logical site(s) (%d variant emissions)\n", report.LogicalIndirects, report.IndirectEmissions)
	if len(gotos) > 0 {
		fmt.Fprintln(output, "\n--- unresolved cross-fn / cross-bank gotos ---")
		keys := make([]gotoStubKey, 0, len(gotos))
		for key := range gotos {
			keys = append(keys, key)
		}
		sort.Slice(keys, func(i, j int) bool {
			if keys[i].Site != keys[j].Site {
				return keys[i].Site < keys[j].Site
			}
			if keys[i].Target != keys[j].Target {
				return keys[i].Target < keys[j].Target
			}
			return keys[i].Function < keys[j].Function
		})
		for _, key := range keys {
			fmt.Fprintf(output, "  site=$%06X -> target=$%06X  in %s\n", key.Site, key.Target, key.Function)
			if verbose {
				writeLocations(output, gotos[key])
			}
		}
	}
	if len(dispatches) > 0 {
		fmt.Fprintln(output, "\n--- indirect-dispatch domain guards (review selector/table coverage) ---")
		keys := make([]uint32, 0, len(dispatches))
		for key := range dispatches {
			keys = append(keys, key)
		}
		sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
		for _, key := range keys {
			fmt.Fprintf(output, "  site=$%06X  (%d variant(s))\n", key, len(dispatches[key]))
			if verbose {
				writeLocations(output, dispatches[key])
			}
		}
	}
	writeAddressLocations(output,
		"unresolved call targets (missing cfg coverage / HLE)", "target",
		targets, verbose)
	writeAddressLocations(output,
		"indirect-jump missing-body guards (review live target / registry coverage)", "site",
		indirects, verbose)
	total := report.LogicalTotal()
	if total == 0 {
		fmt.Fprintln(output, "\nCLEAN — no trap stubs in emitted output.")
	} else {
		fmt.Fprintf(output, "\n%d logical trap(s) remain. Stubs are forbidden — resolve each at the gen path (decode coverage / dispatch cfg / HLE), do NOT silence.\n", total)
	}
	return report, nil
}

// CompareStubBaseline is an additional regression check, not a waiver of the
// strict census gate. Removed diagnostics are reported; new sites, contexts,
// or more emissions require review even if the overall count decreases.
func CompareStubBaseline(report StubCensusReport, baselinePath string, output io.Writer) error {
	data, err := os.ReadFile(baselinePath)
	if err != nil {
		return err
	}
	var baseline StubCensusReport
	if err := json.Unmarshal(data, &baseline); err != nil {
		return err
	}
	if baseline.Version != 1 || len(baseline.Entries) != baseline.LogicalTotal() {
		return fmt.Errorf("stub baseline has missing or inconsistent entries")
	}
	old := make(map[string]StubCensusEntry)
	for _, entry := range baseline.Entries {
		if _, duplicate := old[entry.Key]; duplicate || entry.Key == "" || entry.Emissions < 1 || len(entry.Contexts) == 0 {
			return fmt.Errorf("invalid stub baseline entry %q", entry.Key)
		}
		old[entry.Key] = entry
	}
	var changes []string
	for _, entry := range report.Entries {
		previous, found := old[entry.Key]
		if !found {
			changes = append(changes, "new "+entry.Key)
			continue
		}
		delete(old, entry.Key)
		if entry.Emissions > previous.Emissions {
			changes = append(changes, fmt.Sprintf("%s emissions %d -> %d", entry.Key, previous.Emissions, entry.Emissions))
		}
		known := make(map[string]bool)
		for _, context := range previous.Contexts {
			known[context] = true
		}
		for _, context := range entry.Contexts {
			if !known[context] {
				changes = append(changes, entry.Key+" new context "+context)
			}
		}
	}
	removed := make([]string, 0, len(old))
	for key := range old {
		removed = append(removed, key)
	}
	sort.Strings(removed)
	for _, key := range removed {
		fmt.Fprintln(output, "removed diagnostic:", key)
	}
	if len(changes) != 0 {
		return fmt.Errorf("stub baseline requires review:\n%s", strings.Join(changes, "\n"))
	}
	fmt.Fprintln(output, "No new trap sites, emissions or CPU-width contexts; strict stub gate remains independent.")
	return nil
}

func fileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

func writeAddressLocations(output io.Writer, title, label string,
	entries map[uint32]map[string]struct{}, verbose bool) {
	if len(entries) == 0 {
		return
	}
	fmt.Fprintf(output, "\n--- %s ---\n", title)
	keys := make([]uint32, 0, len(entries))
	for key := range entries {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
	for _, key := range keys {
		fmt.Fprintf(output, "  %s=$%06X  (%d emission(s))\n",
			label, key, len(entries[key]))
		if verbose {
			writeLocations(output, entries[key])
		}
	}
}

func writeLocations(output io.Writer, set map[string]struct{}) {
	locations := make([]string, 0, len(set))
	for location := range set {
		locations = append(locations, location)
	}
	sort.Strings(locations)
	for _, location := range locations {
		fmt.Fprintf(output, "      %s\n", location)
	}
}
