package tooling

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

type LinkAuditOptions struct {
	GenDir, SourceDir, RuntimeDir string
	ListOrphans, Verbose          bool
	Output                        io.Writer
}

var (
	linkNameRE    = `(?:bank_[0-9A-Fa-f]+_[0-9A-Fa-f]+|[A-Za-z]\w*)_M[01]X[01]`
	linkDefRE     = regexp.MustCompile(`^RecompReturn (` + linkNameRE + `)\(CpuState \*cpu\)\s*\{`)
	linkCallRE    = regexp.MustCompile(`\b(` + linkNameRE + `)\(cpu\)`)
	linkTokenRE   = regexp.MustCompile(`\b(` + linkNameRE + `)\b`)
	linkProtoRE   = regexp.MustCompile(`^RecompReturn ` + linkNameRE + `\(CpuState \*cpu\);\s*$`)
	linkVariantRE = regexp.MustCompile(`_M([01])X([01])$`)
)

func scanLinkFile(path string, visit func(string)) error {
	input, err := os.Open(path)
	if err != nil {
		return err
	}
	defer input.Close()
	scanner := bufio.NewScanner(input)
	buffer := make([]byte, 64*1024)
	scanner.Buffer(buffer, 4*1024*1024)
	for scanner.Scan() {
		visit(scanner.Text())
	}
	return scanner.Err()
}

func collectLinkReferences(path string, dispatch bool, referenced map[string]struct{}) error {
	return scanLinkFile(path, func(line string) {
		if linkProtoRE.MatchString(line) || linkDefRE.MatchString(line) {
			return
		}
		expression := linkCallRE
		if dispatch {
			expression = linkTokenRE
		}
		for _, match := range expression.FindAllStringSubmatch(line, -1) {
			referenced[match[1]] = struct{}{}
		}
	})
}

func sortedStringSet(values map[string]struct{}) []string {
	result := make([]string, 0, len(values))
	for value := range values {
		result = append(result, value)
	}
	sort.Strings(result)
	return result
}

func RunLinkAudit(options LinkAuditOptions) error {
	if options.Output == nil {
		options.Output = os.Stdout
	}
	bankFiles, err := filepath.Glob(filepath.Join(options.GenDir, "bank*_v2.c"))
	if err != nil {
		return err
	}
	if len(bankFiles) == 0 {
		return fmt.Errorf("link audit: no generated banks under %s", options.GenDir)
	}
	sort.Strings(bankFiles)
	defined, referenced := make(map[string]struct{}), make(map[string]struct{})
	traps := make(map[string]map[string]struct{})
	for _, path := range bankFiles {
		current := ""
		if err := scanLinkFile(path, func(line string) {
			if match := linkDefRE.FindStringSubmatch(line); match != nil {
				current = match[1]
				defined[current] = struct{}{}
				return
			}
			if current == "" {
				return
			}
			kind := ""
			if strings.Contains(line, "cpu_trace_unresolved_goto_trap") {
				kind = "goto"
			} else if strings.Contains(line, "cpu_trace_dispatch_oob") {
				kind = "dispatch"
			}
			if kind != "" {
				if traps[current] == nil {
					traps[current] = make(map[string]struct{})
				}
				traps[current][kind] = struct{}{}
			}
		}); err != nil {
			return err
		}
		if err := collectLinkReferences(path, false, referenced); err != nil {
			return err
		}
	}
	dispatchPath := filepath.Join(options.GenDir, "dispatch_v2.c")
	if _, err := os.Stat(dispatchPath); err == nil {
		if err := collectLinkReferences(dispatchPath, true, referenced); err != nil {
			return err
		}
	}
	for _, directory := range []string{options.SourceDir, options.RuntimeDir} {
		for _, pattern := range []string{"*.c", "*.h"} {
			paths, _ := filepath.Glob(filepath.Join(directory, pattern))
			for _, path := range paths {
				if err := collectLinkReferences(path, false, referenced); err != nil {
					return err
				}
				if err := collectLinkReferences(path, true, referenced); err != nil {
					return err
				}
			}
		}
	}
	orphans, unreferenced := make(map[string]struct{}), make(map[string]struct{})
	for name := range defined {
		if _, found := referenced[name]; !found {
			orphans[name], unreferenced[name] = struct{}{}, struct{}{}
		}
	}
	orphanTraps, liveTraps := make(map[string]struct{}), make(map[string]struct{})
	for name := range traps {
		if _, orphan := orphans[name]; orphan {
			orphanTraps[name] = struct{}{}
		} else {
			liveTraps[name] = struct{}{}
		}
	}
	reachable := 0
	for name := range defined {
		if _, found := referenced[name]; found {
			reachable++
		}
	}
	fmt.Fprintln(options.Output, "=== LINK AUDIT ===")
	fmt.Fprintf(options.Output, "  defined functions   : %d\n", len(defined))
	fmt.Fprintf(options.Output, "  reachable (referenced): %d\n", reachable)
	fmt.Fprintf(options.Output, "  ORPHANS (dead carves): %d\n", len(orphans))
	fmt.Fprintf(options.Output, "  unreferenced variants: %d\n", len(unreferenced))
	fmt.Fprintf(options.Output, "  functions with traps : %d  (orphan/garbage: %d, LIVE/must-fix: %d)\n", len(traps), len(orphanTraps), len(liveTraps))
	printTraps := func(title string, names map[string]struct{}) {
		if len(names) == 0 {
			return
		}
		fmt.Fprintf(options.Output, "\n--- %s ---\n", title)
		for _, name := range sortedStringSet(names) {
			fmt.Fprintf(options.Output, "  %s: %s\n", name, strings.Join(sortedStringSet(traps[name]), ","))
		}
	}
	printTraps("LIVE trap functions (reachable → must resolve)", liveTraps)
	printTraps("orphan trap functions (unreachable → safe to suppress decode)", orphanTraps)
	if options.ListOrphans {
		fmt.Fprintf(options.Output, "\n--- all %d orphan functions ---\n", len(orphans))
		for _, name := range sortedStringSet(orphans) {
			fmt.Fprintf(options.Output, "  %s\n", name)
		}
	} else if len(orphans) > 0 {
		fmt.Fprintf(options.Output, "\n  (%d orphans total; pass --orphans to list, or see trap orphans above)\n", len(orphans))
	}
	if options.Verbose {
		type coverage struct{ defined, referenced map[string]struct{} }
		byBase := make(map[string]*coverage)
		for name := range defined {
			base := linkVariantRE.ReplaceAllString(name, "")
			if byBase[base] == nil {
				byBase[base] = &coverage{make(map[string]struct{}), make(map[string]struct{})}
			}
			byBase[base].defined[linkVariantRE.FindString(name)] = struct{}{}
			if _, found := referenced[name]; found {
				byBase[base].referenced[linkVariantRE.FindString(name)] = struct{}{}
			}
		}
		var partial []string
		for base, item := range byBase {
			if len(item.referenced) > 0 && len(item.defined) > len(item.referenced) {
				partial = append(partial, base)
			}
		}
		sort.Strings(partial)
		fmt.Fprintf(options.Output, "\n--- PCs with defined-but-never-referenced variants (%d) ---\n", len(partial))
		for index, base := range partial {
			if index == 40 {
				break
			}
			item := byBase[base]
			var missing []string
			for variant := range item.defined {
				if _, found := item.referenced[variant]; !found {
					missing = append(missing, variant)
				}
			}
			fmt.Fprintf(options.Output, "  %s: defined=%v unreferenced=%v\n", base, sortedStringSet(item.defined), missing)
		}
	}
	if len(liveTraps) > 0 {
		return errors.New("link audit found live traps")
	}
	return nil
}
