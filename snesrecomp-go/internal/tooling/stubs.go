package tooling

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
)

var (
	stubGotoRE     = regexp.MustCompile(`cpu_trace_unresolved_goto_trap\(cpu,\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),\s*"([^"]+)",\s*"([^"]+)"\)`)
	stubDispatchRE = regexp.MustCompile(`cpu_trace_dispatch_oob\(cpu,\s*0x([0-9A-Fa-f]+)`)
	stubVariantRE  = regexp.MustCompile(`_M[01]X[01]$`)
)

type StubCensusReport struct {
	LogicalGotos      int
	GotoEmissions     int
	LogicalDispatches int
	DispatchEmissions int
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
	sort.Strings(paths)
	gotos := make(map[gotoStubKey]map[string]struct{})
	dispatches := make(map[uint32]map[string]struct{})
	for _, path := range paths {
		file, err := os.Open(path)
		if err != nil {
			return StubCensusReport{}, err
		}
		scanner := bufio.NewScanner(file)
		lineNumber := 0
		for scanner.Scan() {
			lineNumber++
			line := scanner.Text()
			if match := stubGotoRE.FindStringSubmatch(line); match != nil {
				site, _ := strconv.ParseUint(match[1], 16, 32)
				target, _ := strconv.ParseUint(match[2], 16, 32)
				key := gotoStubKey{Site: uint32(site), Target: uint32(target), Function: stubVariantRE.ReplaceAllString(match[3], "")}
				if gotos[key] == nil {
					gotos[key] = make(map[string]struct{})
				}
				gotos[key][fmt.Sprintf("%s:%d", filepath.Base(path), lineNumber)] = struct{}{}
			}
			if match := stubDispatchRE.FindStringSubmatch(line); match != nil {
				site, _ := strconv.ParseUint(match[1], 16, 32)
				key := uint32(site)
				if dispatches[key] == nil {
					dispatches[key] = make(map[string]struct{})
				}
				dispatches[key][fmt.Sprintf("%s:%d", filepath.Base(path), lineNumber)] = struct{}{}
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
	report := StubCensusReport{LogicalGotos: len(gotos), LogicalDispatches: len(dispatches)}
	for _, locations := range gotos {
		report.GotoEmissions += len(locations)
	}
	for _, locations := range dispatches {
		report.DispatchEmissions += len(locations)
	}
	fmt.Fprintln(output, "=== STUB CENSUS ===")
	fmt.Fprintf(output, "  unresolved goto traps : %d logical site(s) (%d variant emissions)\n", report.LogicalGotos, report.GotoEmissions)
	fmt.Fprintf(output, "  indirect-dispatch oob : %d logical site(s) (%d variant emissions)\n", report.LogicalDispatches, report.DispatchEmissions)
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
		fmt.Fprintln(output, "\n--- unresolved indirect dispatch (needs indirect_dispatch cfg) ---")
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
	total := report.LogicalGotos + report.LogicalDispatches
	if total == 0 {
		fmt.Fprintln(output, "\nCLEAN — no trap stubs in emitted output.")
	} else {
		fmt.Fprintf(output, "\n%d logical trap site(s) remain. Stubs are forbidden — resolve each at the gen path (decode coverage / indirect_dispatch cfg), do NOT silence.\n", total)
	}
	return report, nil
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
