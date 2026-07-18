package tooling

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

var (
	metadataFunctionRE = regexp.MustCompile(`^RecompReturn (bank_([0-9A-Fa-f]{2})_([0-9A-Fa-f]+))(_M(\d)X(\d))\(CpuState \*cpu\) \{`)
	metadataLabelRE    = regexp.MustCompile(`^  L_([0-9A-Fa-f]{4})_M(\d)X(\d):`)
	metadataTailRE     = regexp.MustCompile(`tail-call past end: into (bank_[0-9A-Fa-f]{2}_[0-9A-Fa-f]+_M\dX\d) at \$([0-9A-Fa-f]+)`)
	metadataBankRE     = regexp.MustCompile(`(?i)bank([0-9a-f]+)\.cfg$`)
)

type MetadataDirective struct {
	Bank string `json:"bank"`
	Line int    `json:"line"`
	Text string `json:"text"`
}

type MetadataTailCall struct {
	Source   string `json:"src"`
	Target   string `json:"tgt_fn"`
	TargetPC string `json:"tgt_pc24"`
}

type GeneratedMetadata struct {
	GeneratedAt string                         `json:"generated_at"`
	Functions   map[string][]string            `json:"functions"`
	Labels      map[string][]string            `json:"labels"`
	TailCalls   []MetadataTailCall             `json:"tailcalls"`
	CFG         map[string][]MetadataDirective `json:"cfg"`
}

type MetadataReport struct {
	Functions int
	Labels    int
	TailCalls int
	CFGCounts map[string]int
}

// GenerateMetadata scrapes generated C and raw cfg directives into the static
// sidecar consumed by trace_slice.py and resolve_miss.py.
func GenerateMetadata(genDir, cfgDir, outputPath string, now time.Time) (MetadataReport, error) {
	functionSets := make(map[string][]string)
	labelSets := make(map[string]map[string]struct{})
	var tailCalls []MetadataTailCall
	paths, err := filepath.Glob(filepath.Join(genDir, "bank*_v2.c"))
	if err != nil {
		return MetadataReport{}, err
	}
	sort.Strings(paths)
	for _, path := range paths {
		file, openErr := os.Open(path)
		if openErr != nil {
			return MetadataReport{}, openErr
		}
		scanner := bufio.NewScanner(file)
		scanner.Buffer(make([]byte, 64*1024), 4*1024*1024)
		current, currentBank := "", ""
		for scanner.Scan() {
			line := scanner.Text()
			if match := metadataFunctionRE.FindStringSubmatch(line); match != nil {
				current = match[1] + match[4]
				currentBank = strings.ToUpper(match[2])
				key := strings.ToUpper(match[2] + match[3])
				functionSets[key] = append(functionSets[key], match[4])
				continue
			}
			if current == "" {
				continue
			}
			if match := metadataLabelRE.FindStringSubmatch(line); match != nil {
				key := currentBank + strings.ToUpper(match[1])
				if labelSets[key] == nil {
					labelSets[key] = make(map[string]struct{})
				}
				labelSets[key][current] = struct{}{}
				continue
			}
			if match := metadataTailRE.FindStringSubmatch(line); match != nil {
				tailCalls = append(tailCalls, MetadataTailCall{Source: current, Target: match[1], TargetPC: currentBank + strings.ToUpper(match[2])})
			}
		}
		scanErr := scanner.Err()
		closeErr := file.Close()
		if scanErr != nil {
			return MetadataReport{}, scanErr
		}
		if closeErr != nil {
			return MetadataReport{}, closeErr
		}
	}
	labels := make(map[string][]string, len(labelSets))
	for key, values := range labelSets {
		for value := range values {
			labels[key] = append(labels[key], value)
		}
		sort.Strings(labels[key])
	}
	for key := range functionSets {
		sort.Strings(functionSets[key])
	}

	directives := []string{"func", "rts_dispatch", "indirect_dispatch", "exit_mx_at"}
	cfgData := make(map[string][]MetadataDirective, len(directives))
	for _, directive := range directives {
		cfgData[directive] = []MetadataDirective{}
	}
	cfgPaths, err := filepath.Glob(filepath.Join(cfgDir, "bank*.cfg"))
	if err != nil {
		return MetadataReport{}, err
	}
	sort.Strings(cfgPaths)
	for _, path := range cfgPaths {
		match := metadataBankRE.FindStringSubmatch(filepath.Base(path))
		if match == nil {
			continue
		}
		bank := strings.ToUpper(match[1])
		if len(bank) < 2 {
			bank = strings.Repeat("0", 2-len(bank)) + bank
		}
		file, openErr := os.Open(path)
		if openErr != nil {
			return MetadataReport{}, openErr
		}
		scanner := bufio.NewScanner(file)
		lineNumber := 0
		for scanner.Scan() {
			lineNumber++
			trimmed := strings.TrimSpace(scanner.Text())
			if trimmed == "" || strings.HasPrefix(trimmed, "#") {
				continue
			}
			word := strings.Fields(trimmed)[0]
			if _, found := cfgData[word]; found {
				cfgData[word] = append(cfgData[word], MetadataDirective{Bank: bank, Line: lineNumber, Text: trimmed})
			}
		}
		scanErr := scanner.Err()
		closeErr := file.Close()
		if scanErr != nil {
			return MetadataReport{}, scanErr
		}
		if closeErr != nil {
			return MetadataReport{}, closeErr
		}
	}

	metadata := GeneratedMetadata{GeneratedAt: now.Format("2006-01-02 15:04:05"), Functions: functionSets, Labels: labels, TailCalls: tailCalls, CFG: cfgData}
	encoded, err := json.Marshal(metadata)
	if err != nil {
		return MetadataReport{}, err
	}
	if err := os.MkdirAll(filepath.Dir(outputPath), 0o755); err != nil {
		return MetadataReport{}, err
	}
	if err := os.WriteFile(outputPath, encoded, 0o644); err != nil {
		return MetadataReport{}, err
	}
	report := MetadataReport{Functions: len(functionSets), Labels: len(labels), TailCalls: len(tailCalls), CFGCounts: make(map[string]int)}
	for _, directive := range directives {
		report.CFGCounts[directive] = len(cfgData[directive])
	}
	return report, nil
}

func FormatMetadataReport(report MetadataReport, outputPath string, elapsed time.Duration) string {
	return fmt.Sprintf("gen_meta: %d func entries, %d label pcs, %d tailcall sites, cfg: func=%d, rts_dispatch=%d, indirect_dispatch=%d, exit_mx_at=%d -> %s (%.1fs)", report.Functions, report.Labels, report.TailCalls, report.CFGCounts["func"], report.CFGCounts["rts_dispatch"], report.CFGCounts["indirect_dispatch"], report.CFGCounts["exit_mx_at"], outputPath, elapsed.Seconds())
}
