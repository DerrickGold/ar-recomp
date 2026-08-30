package tooling

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"time"
)

const replayBenchmarkVersion = 1

type ReplayBenchmarkManifest struct {
	Version                  int                       `json:"version"`
	Command                  []string                  `json:"command"`
	Environment              map[string]string         `json:"environment,omitempty"`
	CleanEnvironmentPrefixes []string                  `json:"clean_environment_prefixes,omitempty"`
	Artifacts                []string                  `json:"artifacts"`
	HardDiagnostics          []string                  `json:"hard_diagnostics,omitempty"`
	SaveEnvironment          string                    `json:"save_environment,omitempty"`
	ReplayEnvironment        string                    `json:"replay_environment,omitempty"`
	QuitFramesEnvironment    string                    `json:"quit_frames_environment,omitempty"`
	SettingsEnvironment      string                    `json:"settings_environment,omitempty"`
	FinalFrameArtifact       string                    `json:"final_frame_artifact,omitempty"`
	FinalFramePattern        string                    `json:"final_frame_pattern,omitempty"`
	TimeoutSeconds           int                       `json:"timeout_seconds,omitempty"`
	DefaultWorkloads         []string                  `json:"default_workloads,omitempty"`
	Workloads                []ReplayBenchmarkWorkload `json:"workloads"`
}

type ReplayBenchmarkWorkload struct {
	Name           string            `json:"name"`
	Description    string            `json:"description,omitempty"`
	Replay         string            `json:"replay,omitempty"`
	Frames         int               `json:"frames"`
	Settings       string            `json:"settings,omitempty"`
	SaveSeed       string            `json:"save_seed,omitempty"`
	RequiredOutput []string          `json:"required_output,omitempty"`
	Environment    map[string]string `json:"environment,omitempty"`
}

type ReplayBenchmarkOptions struct {
	Root, SuitePath, BinaryPath, ReferenceBinaryPath string
	ROMPath, ConfigPath, FallbackSavePath            string
	Runs, Warmups                                    int
	SelectedWorkloads                                []string
	Label, OutputPath, ComparePath                   string
	MaxRegressionPercent, MaxSuiteRegressionPercent  float64
}

type ReplayBenchmarkFile struct {
	Path   string `json:"path"`
	Bytes  int64  `json:"bytes"`
	SHA256 string `json:"sha256"`
}

type ReplayBenchmarkPerformance struct {
	DurationsSeconds               []float64 `json:"durations_seconds"`
	MedianSeconds                  float64   `json:"median_seconds"`
	MeanSeconds                    float64   `json:"mean_seconds"`
	StandardDeviationSeconds       float64   `json:"stdev_seconds"`
	MedianAbsoluteDeviationSeconds float64   `json:"median_absolute_deviation_seconds"`
	MinimumSeconds                 float64   `json:"min_seconds"`
	MaximumSeconds                 float64   `json:"max_seconds"`
	MedianEmulatedFPS              float64   `json:"median_emulated_fps"`
}

type ReplayBenchmarkWorkloadResult struct {
	Input               map[string]any             `json:"input"`
	Performance         ReplayBenchmarkPerformance `json:"performance"`
	FinalArtifactSHA256 map[string]string          `json:"final_artifact_sha256"`
}

type ReplayBenchmarkReference struct {
	Binary                     ReplayBenchmarkFile                      `json:"binary"`
	Workloads                  map[string]ReplayBenchmarkWorkloadResult `json:"workloads"`
	SuiteGeometricMeanFPS      float64                                  `json:"suite_geometric_mean_emulated_fps"`
	MedianAdjacentDeltaPercent map[string]float64                       `json:"median_adjacent_delta_percent"`
	SuiteRegressionPercent     float64                                  `json:"suite_regression_percent"`
}

type ReplayBenchmarkResult struct {
	Version               int                                      `json:"schema_version"`
	Label                 string                                   `json:"label"`
	CapturedAtUTC         string                                   `json:"captured_at_utc"`
	Host                  map[string]any                           `json:"host"`
	Method                map[string]any                           `json:"method"`
	Suite                 ReplayBenchmarkFile                      `json:"suite"`
	Binary                ReplayBenchmarkFile                      `json:"binary"`
	Inputs                map[string]ReplayBenchmarkFile           `json:"inputs"`
	Workloads             map[string]ReplayBenchmarkWorkloadResult `json:"workloads"`
	SuiteGeometricMeanFPS float64                                  `json:"suite_geometric_mean_emulated_fps"`
	PairedReference       *ReplayBenchmarkReference                `json:"paired_reference,omitempty"`
}

type replayBenchmarkObservation struct {
	Elapsed   float64
	Artifacts map[string]string
	Output    string
}

func LoadReplayBenchmarkManifest(path string) (ReplayBenchmarkManifest, ReplayBenchmarkFile, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return ReplayBenchmarkManifest{}, ReplayBenchmarkFile{}, fmt.Errorf("read replay benchmark suite %s: %w", path, err)
	}
	var manifest ReplayBenchmarkManifest
	if err := json.Unmarshal(content, &manifest); err != nil {
		return manifest, ReplayBenchmarkFile{}, fmt.Errorf("parse replay benchmark suite %s: %w", path, err)
	}
	if manifest.Version != replayBenchmarkVersion {
		return manifest, ReplayBenchmarkFile{}, fmt.Errorf("replay benchmark suite version is %d (want %d)", manifest.Version, replayBenchmarkVersion)
	}
	if len(manifest.Command) == 0 {
		manifest.Command = []string{"{binary}", "{rom}", "--config", "{config}"}
	}
	if len(manifest.Artifacts) == 0 || len(manifest.Workloads) == 0 {
		return manifest, ReplayBenchmarkFile{}, fmt.Errorf("replay benchmark suite needs artifacts and workloads")
	}
	seen := make(map[string]bool)
	for _, workload := range manifest.Workloads {
		if strings.TrimSpace(workload.Name) == "" || seen[workload.Name] || workload.Frames <= 0 {
			return manifest, ReplayBenchmarkFile{}, fmt.Errorf("replay benchmark suite has invalid/duplicate workload %q", workload.Name)
		}
		seen[workload.Name] = true
	}
	digest := sha256.Sum256(content)
	return manifest, ReplayBenchmarkFile{Path: path, Bytes: int64(len(content)), SHA256: hex.EncodeToString(digest[:])}, nil
}

func replayBenchmarkFileIdentity(path string) (ReplayBenchmarkFile, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return ReplayBenchmarkFile{}, fmt.Errorf("read benchmark input %s: %w", path, err)
	}
	digest := sha256.Sum256(content)
	return ReplayBenchmarkFile{Path: path, Bytes: int64(len(content)), SHA256: hex.EncodeToString(digest[:])}, nil
}

func replayBenchmarkSeed(path string) ([]byte, ReplayBenchmarkFile, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, ReplayBenchmarkFile{}, fmt.Errorf("read replay benchmark seed %s: %w", path, err)
	}
	identityDigest := sha256.Sum256(content)
	identity := ReplayBenchmarkFile{Path: path, Bytes: int64(len(content)), SHA256: hex.EncodeToString(identityDigest[:])}
	if strings.EqualFold(filepath.Ext(path), ".b64") {
		content, err = base64.StdEncoding.DecodeString(strings.Join(strings.Fields(string(content)), ""))
		if err != nil {
			return nil, identity, fmt.Errorf("decode replay benchmark seed %s: %w", path, err)
		}
	}
	return content, identity, nil
}

func replayBenchmarkEnvironment(prefixes []string) []string {
	var environment []string
	for _, item := range os.Environ() {
		name, _, _ := strings.Cut(item, "=")
		drop := false
		for _, prefix := range prefixes {
			if strings.HasPrefix(name, prefix) {
				drop = true
				break
			}
		}
		if !drop {
			environment = append(environment, item)
		}
	}
	return environment
}

func expandReplayBenchmark(value string, replacements map[string]string) string {
	for key, replacement := range replacements {
		value = strings.ReplaceAll(value, "{"+key+"}", replacement)
	}
	return value
}

func replayBenchmarkInput(root, path string) string {
	if strings.TrimSpace(path) == "" || filepath.IsAbs(path) {
		return path
	}
	return filepath.Join(root, path)
}

func replayBenchmarkDisplayPath(root, path string) string {
	relative, err := filepath.Rel(root, path)
	if err == nil && relative != ".." && !strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
		return filepath.ToSlash(relative)
	}
	return path
}

func replayBenchmarkDisplayFile(root string, identity ReplayBenchmarkFile) ReplayBenchmarkFile {
	identity.Path = replayBenchmarkDisplayPath(root, identity.Path)
	return identity
}

func runReplayBenchmarkOnce(manifest ReplayBenchmarkManifest, root, binary, romPath, configPath, fallbackSave string, workload ReplayBenchmarkWorkload) (replayBenchmarkObservation, error) {
	workdir, err := os.MkdirTemp("", "snesrecomp-replay-bench-")
	if err != nil {
		return replayBenchmarkObservation{}, fmt.Errorf("create replay benchmark directory: %w", err)
	}
	defer os.RemoveAll(workdir)
	replayPath := replayBenchmarkInput(root, workload.Replay)
	settingsPath := replayBenchmarkInput(root, workload.Settings)
	seedPath := replayBenchmarkInput(root, workload.SaveSeed)
	if seedPath == "" {
		seedPath = fallbackSave
	}
	savePath := ""
	if seedPath != "" {
		seed, _, seedErr := replayBenchmarkSeed(seedPath)
		if seedErr != nil {
			return replayBenchmarkObservation{}, seedErr
		}
		savePath = filepath.Join(workdir, "benchmark-seed.srm")
		if err := os.WriteFile(savePath, seed, 0o644); err != nil {
			return replayBenchmarkObservation{}, fmt.Errorf("write isolated replay benchmark seed: %w", err)
		}
	}
	replacements := map[string]string{
		"root": root, "binary": binary, "rom": romPath, "config": configPath,
		"workdir": workdir, "replay": replayPath, "settings": settingsPath,
		"save": savePath, "frames": strconv.Itoa(workload.Frames), "workload": workload.Name,
	}
	environment := replayBenchmarkEnvironment(manifest.CleanEnvironmentPrefixes)
	values := make(map[string]string)
	for key, value := range manifest.Environment {
		values[key] = expandReplayBenchmark(value, replacements)
	}
	for key, value := range workload.Environment {
		values[key] = expandReplayBenchmark(value, replacements)
	}
	if manifest.SaveEnvironment != "" && savePath != "" {
		values[manifest.SaveEnvironment] = savePath
	}
	if manifest.ReplayEnvironment != "" && replayPath != "" {
		values[manifest.ReplayEnvironment] = replayPath
	}
	if manifest.QuitFramesEnvironment != "" {
		values[manifest.QuitFramesEnvironment] = strconv.Itoa(workload.Frames)
	}
	if manifest.SettingsEnvironment != "" && settingsPath != "" {
		values[manifest.SettingsEnvironment] = settingsPath
	}
	keys := make([]string, 0, len(values))
	for key := range values {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	for _, key := range keys {
		environment = append(environment, key+"="+values[key])
	}
	command := make([]string, len(manifest.Command))
	for index, value := range manifest.Command {
		command[index] = expandReplayBenchmark(value, replacements)
	}
	if len(command) == 0 || command[0] == "" {
		return replayBenchmarkObservation{}, fmt.Errorf("replay benchmark command is empty")
	}
	ctx := context.Background()
	cancel := func() {}
	if manifest.TimeoutSeconds > 0 {
		ctx, cancel = context.WithTimeout(ctx, time.Duration(manifest.TimeoutSeconds)*time.Second)
	}
	defer cancel()
	process := exec.CommandContext(ctx, command[0], command[1:]...)
	process.Dir, process.Env = workdir, environment
	started := time.Now()
	combined, runErr := process.CombinedOutput()
	elapsed := time.Since(started).Seconds()
	if ctx.Err() == context.DeadlineExceeded {
		return replayBenchmarkObservation{}, fmt.Errorf("workload %s timed out after %d seconds", workload.Name, manifest.TimeoutSeconds)
	}
	if runErr != nil {
		return replayBenchmarkObservation{}, fmt.Errorf("workload %s failed after %.3fs: %w\n%s", workload.Name, elapsed, runErr, tailReplayOutput(combined))
	}
	text := string(combined)
	for _, marker := range manifest.HardDiagnostics {
		if strings.Contains(text, marker) {
			return replayBenchmarkObservation{}, fmt.Errorf("workload %s reached hard diagnostic %q\n%s", workload.Name, marker, tailReplayOutput(combined))
		}
	}
	for _, marker := range workload.RequiredOutput {
		if !strings.Contains(text, marker) {
			return replayBenchmarkObservation{}, fmt.Errorf("workload %s did not reach required output %q\n%s", workload.Name, marker, tailReplayOutput(combined))
		}
	}
	if manifest.FinalFrameArtifact != "" && manifest.FinalFramePattern != "" {
		statePath := filepath.Join(workdir, filepath.FromSlash(manifest.FinalFrameArtifact))
		state, readErr := os.ReadFile(statePath)
		if readErr != nil {
			return replayBenchmarkObservation{}, fmt.Errorf("workload %s final-frame artifact: %w", workload.Name, readErr)
		}
		pattern, compileErr := regexp.Compile(manifest.FinalFramePattern)
		if compileErr != nil {
			return replayBenchmarkObservation{}, fmt.Errorf("compile final_frame_pattern: %w", compileErr)
		}
		match := pattern.FindSubmatch(state)
		if len(match) < 2 || string(match[1]) != strconv.Itoa(workload.Frames) {
			return replayBenchmarkObservation{}, fmt.Errorf("workload %s did not finish at frame %d", workload.Name, workload.Frames)
		}
	}
	artifacts := make(map[string]string)
	for _, relative := range manifest.Artifacts {
		path := filepath.Join(workdir, filepath.FromSlash(relative))
		content, readErr := os.ReadFile(path)
		if readErr != nil {
			return replayBenchmarkObservation{}, fmt.Errorf("workload %s artifact %s: %w", workload.Name, relative, readErr)
		}
		digest := sha256.Sum256(content)
		artifacts[relative] = hex.EncodeToString(digest[:])
	}
	return replayBenchmarkObservation{Elapsed: elapsed, Artifacts: artifacts, Output: text}, nil
}

func tailReplayOutput(content []byte) string {
	const limit = 12000
	if len(content) > limit {
		content = content[len(content)-limit:]
	}
	return string(content)
}

func roundedReplay(value float64, digits int) float64 {
	scale := math.Pow10(digits)
	return math.Round(value*scale) / scale
}

func replayMedian(values []float64) float64 {
	copyValues := append([]float64(nil), values...)
	sort.Float64s(copyValues)
	middle := len(copyValues) / 2
	if len(copyValues)%2 != 0 {
		return copyValues[middle]
	}
	return (copyValues[middle-1] + copyValues[middle]) / 2
}

func replayPerformance(values []float64, frames int) ReplayBenchmarkPerformance {
	median := replayMedian(values)
	deviations := make([]float64, len(values))
	var sum float64
	minimum, maximum := values[0], values[0]
	for index, value := range values {
		sum += value
		deviations[index] = math.Abs(value - median)
		if value < minimum {
			minimum = value
		}
		if value > maximum {
			maximum = value
		}
	}
	mean := sum / float64(len(values))
	var variance float64
	if len(values) > 1 {
		for _, value := range values {
			variance += (value - mean) * (value - mean)
		}
		variance /= float64(len(values) - 1)
	}
	roundedValues := make([]float64, len(values))
	for index, value := range values {
		roundedValues[index] = roundedReplay(value, 6)
	}
	return ReplayBenchmarkPerformance{
		DurationsSeconds: roundedValues, MedianSeconds: roundedReplay(median, 6), MeanSeconds: roundedReplay(mean, 6),
		StandardDeviationSeconds: roundedReplay(math.Sqrt(variance), 6), MedianAbsoluteDeviationSeconds: roundedReplay(replayMedian(deviations), 6),
		MinimumSeconds: roundedReplay(minimum, 6), MaximumSeconds: roundedReplay(maximum, 6), MedianEmulatedFPS: roundedReplay(float64(frames)/median, 2),
	}
}

func replayGeometricMeanFPS(results map[string]ReplayBenchmarkWorkloadResult) float64 {
	var sum float64
	for _, result := range results {
		sum += math.Log(result.Performance.MedianEmulatedFPS)
	}
	return roundedReplay(math.Exp(sum/float64(len(results))), 2)
}

func workloadReplayIdentity(root, fallbackSave string, workload ReplayBenchmarkWorkload) (map[string]any, error) {
	result := map[string]any{"description": workload.Description, "frames": workload.Frames, "required_output": workload.RequiredOutput, "environment": workload.Environment}
	for key, path := range map[string]string{"replay": workload.Replay, "settings": workload.Settings} {
		if path == "" {
			result[key] = nil
			continue
		}
		identity, err := replayBenchmarkFileIdentity(replayBenchmarkInput(root, path))
		if err != nil {
			return nil, err
		}
		result[key] = replayBenchmarkDisplayFile(root, identity)
	}
	seedPath := replayBenchmarkInput(root, workload.SaveSeed)
	if seedPath == "" {
		seedPath = fallbackSave
	}
	if seedPath != "" {
		decoded, identity, err := replayBenchmarkSeed(seedPath)
		if err != nil {
			return nil, err
		}
		digest := sha256.Sum256(decoded)
		result["save_seed"] = map[string]any{"file": replayBenchmarkDisplayFile(root, identity), "decoded_bytes": len(decoded), "decoded_sha256": hex.EncodeToString(digest[:])}
	} else {
		result["save_seed"] = nil
	}
	return result, nil
}

func BuildReplayBenchmark(options ReplayBenchmarkOptions, progress ioWriter) (ReplayBenchmarkResult, bool, error) {
	if options.Runs < 1 || options.Warmups < 0 {
		return ReplayBenchmarkResult{}, false, fmt.Errorf("replay benchmark needs at least one run and non-negative warmups")
	}
	if options.MaxRegressionPercent < 0 || options.MaxSuiteRegressionPercent < 0 {
		return ReplayBenchmarkResult{}, false, fmt.Errorf("replay benchmark regression limits cannot be negative")
	}
	root, err := filepath.Abs(options.Root)
	if err != nil {
		return ReplayBenchmarkResult{}, false, fmt.Errorf("resolve replay benchmark root: %w", err)
	}
	resolve := func(path string) string { return replayBenchmarkInput(root, path) }
	suitePath, binary, romPath, configPath := resolve(options.SuitePath), resolve(options.BinaryPath), resolve(options.ROMPath), resolve(options.ConfigPath)
	referenceBinary, fallbackSave := resolve(options.ReferenceBinaryPath), resolve(options.FallbackSavePath)
	manifest, suiteIdentity, err := LoadReplayBenchmarkManifest(suitePath)
	if err != nil {
		return ReplayBenchmarkResult{}, false, err
	}
	binaryIdentity, err := replayBenchmarkFileIdentity(binary)
	if err != nil {
		return ReplayBenchmarkResult{}, false, err
	}
	suiteIdentity = replayBenchmarkDisplayFile(root, suiteIdentity)
	binaryIdentity = replayBenchmarkDisplayFile(root, binaryIdentity)
	inputs := make(map[string]ReplayBenchmarkFile)
	for key, path := range map[string]string{"rom": romPath, "config": configPath} {
		if path == "" {
			continue
		}
		identity, identityErr := replayBenchmarkFileIdentity(path)
		if identityErr != nil {
			return ReplayBenchmarkResult{}, false, identityErr
		}
		inputs[key] = replayBenchmarkDisplayFile(root, identity)
	}
	selectedNames := options.SelectedWorkloads
	if len(selectedNames) == 0 {
		selectedNames = append(selectedNames, manifest.DefaultWorkloads...)
		if len(selectedNames) == 0 {
			for _, workload := range manifest.Workloads {
				selectedNames = append(selectedNames, workload.Name)
			}
		}
	}
	byName := make(map[string]ReplayBenchmarkWorkload)
	for _, workload := range manifest.Workloads {
		byName[workload.Name] = workload
	}
	selected := make([]ReplayBenchmarkWorkload, 0, len(selectedNames))
	for _, name := range selectedNames {
		workload, found := byName[name]
		if !found {
			return ReplayBenchmarkResult{}, false, fmt.Errorf("unknown replay benchmark workload %q", name)
		}
		selected = append(selected, workload)
	}
	writeProgress := func(format string, args ...any) {
		if progress != nil {
			fmt.Fprintf(progress, format, args...)
		}
	}
	for warmup := 0; warmup < options.Warmups; warmup++ {
		for _, workload := range selected {
			binaries := []string{binary}
			if referenceBinary != "" {
				binaries = append(binaries, referenceBinary)
				if warmup%2 != 0 {
					binaries[0], binaries[1] = binaries[1], binaries[0]
				}
			}
			for _, usedBinary := range binaries {
				writeProgress("warmup %d/%d: %s\n", warmup+1, options.Warmups, workload.Name)
				if _, err := runReplayBenchmarkOnce(manifest, root, usedBinary, romPath, configPath, fallbackSave, workload); err != nil {
					return ReplayBenchmarkResult{}, false, err
				}
			}
		}
	}
	durations, referenceDurations := make(map[string][]float64), make(map[string][]float64)
	artifacts, referenceArtifacts := make(map[string]map[string]string), make(map[string]map[string]string)
	for round := 0; round < options.Runs; round++ {
		order := append([]ReplayBenchmarkWorkload(nil), selected...)
		if round%2 != 0 {
			for left, right := 0, len(order)-1; left < right; left, right = left+1, right-1 {
				order[left], order[right] = order[right], order[left]
			}
		}
		for workloadIndex, workload := range order {
			if referenceBinary == "" {
				observation, err := runReplayBenchmarkOnce(manifest, root, binary, romPath, configPath, fallbackSave, workload)
				if err != nil {
					return ReplayBenchmarkResult{}, false, err
				}
				if previous := artifacts[workload.Name]; previous != nil && !equalReplayArtifacts(previous, observation.Artifacts) {
					return ReplayBenchmarkResult{}, false, fmt.Errorf("workload %s artifacts changed between runs", workload.Name)
				}
				artifacts[workload.Name] = observation.Artifacts
				durations[workload.Name] = append(durations[workload.Name], observation.Elapsed)
				writeProgress("run %d/%d: %-20s %.4f s\n", round+1, options.Runs, workload.Name, observation.Elapsed)
				continue
			}
			pair := []struct{ role, binary string }{{"candidate", binary}, {"reference", referenceBinary}}
			if (round+workloadIndex)%2 != 0 {
				pair[0], pair[1] = pair[1], pair[0]
			}
			observations := make(map[string]replayBenchmarkObservation)
			for _, item := range pair {
				observation, err := runReplayBenchmarkOnce(manifest, root, item.binary, romPath, configPath, fallbackSave, workload)
				if err != nil {
					return ReplayBenchmarkResult{}, false, err
				}
				observations[item.role] = observation
			}
			candidate, reference := observations["candidate"], observations["reference"]
			if !equalReplayArtifacts(candidate.Artifacts, reference.Artifacts) {
				return ReplayBenchmarkResult{}, false, fmt.Errorf("workload %s candidate/reference artifacts differ: %s", workload.Name, describeReplayArtifactDifferences(candidate.Artifacts, reference.Artifacts))
			}
			if previous := artifacts[workload.Name]; previous != nil && !equalReplayArtifacts(previous, candidate.Artifacts) {
				return ReplayBenchmarkResult{}, false, fmt.Errorf("workload %s candidate artifacts changed between runs", workload.Name)
			}
			if previous := referenceArtifacts[workload.Name]; previous != nil && !equalReplayArtifacts(previous, reference.Artifacts) {
				return ReplayBenchmarkResult{}, false, fmt.Errorf("workload %s reference artifacts changed between runs", workload.Name)
			}
			artifacts[workload.Name], referenceArtifacts[workload.Name] = candidate.Artifacts, reference.Artifacts
			durations[workload.Name] = append(durations[workload.Name], candidate.Elapsed)
			referenceDurations[workload.Name] = append(referenceDurations[workload.Name], reference.Elapsed)
			writeProgress("pair %d/%d: %-20s candidate=%.4f reference=%.4f delta=%+.2f%%\n", round+1, options.Runs, workload.Name, candidate.Elapsed, reference.Elapsed, (candidate.Elapsed/reference.Elapsed-1)*100)
		}
	}
	result := ReplayBenchmarkResult{
		Version: replayBenchmarkVersion, Label: options.Label, CapturedAtUTC: time.Now().UTC().Format(time.RFC3339Nano),
		Host:   map[string]any{"os": runtime.GOOS, "architecture": runtime.GOARCH, "logical_cpus": runtime.NumCPU(), "go": runtime.Version()},
		Method: map[string]any{"warmups": options.Warmups, "measured_runs": options.Runs, "order": "alternating workloads and adjacent A/B roles"},
		Suite:  suiteIdentity, Binary: binaryIdentity, Inputs: inputs, Workloads: make(map[string]ReplayBenchmarkWorkloadResult),
	}
	for _, workload := range selected {
		identity, err := workloadReplayIdentity(root, fallbackSave, workload)
		if err != nil {
			return ReplayBenchmarkResult{}, false, err
		}
		result.Workloads[workload.Name] = ReplayBenchmarkWorkloadResult{Input: identity, Performance: replayPerformance(durations[workload.Name], workload.Frames), FinalArtifactSHA256: artifacts[workload.Name]}
	}
	result.SuiteGeometricMeanFPS = replayGeometricMeanFPS(result.Workloads)
	passed := true
	if referenceBinary != "" {
		referenceIdentity, err := replayBenchmarkFileIdentity(referenceBinary)
		if err != nil {
			return ReplayBenchmarkResult{}, false, err
		}
		referenceIdentity = replayBenchmarkDisplayFile(root, referenceIdentity)
		reference := &ReplayBenchmarkReference{Binary: referenceIdentity, Workloads: make(map[string]ReplayBenchmarkWorkloadResult), MedianAdjacentDeltaPercent: make(map[string]float64)}
		var logarithmicDelta float64
		for _, workload := range selected {
			performance := replayPerformance(referenceDurations[workload.Name], workload.Frames)
			reference.Workloads[workload.Name] = ReplayBenchmarkWorkloadResult{Performance: performance, FinalArtifactSHA256: referenceArtifacts[workload.Name]}
			var ratios []float64
			for index, candidate := range durations[workload.Name] {
				ratios = append(ratios, candidate/referenceDurations[workload.Name][index]-1)
			}
			delta := roundedReplay(replayMedian(ratios)*100, 3)
			reference.MedianAdjacentDeltaPercent[workload.Name] = delta
			logarithmicDelta += math.Log1p(delta / 100)
			if delta > options.MaxRegressionPercent {
				passed = false
			}
		}
		reference.SuiteGeometricMeanFPS = replayGeometricMeanFPS(reference.Workloads)
		reference.SuiteRegressionPercent = roundedReplay((math.Exp(logarithmicDelta/float64(len(selected)))-1)*100, 3)
		if len(selected) == len(manifest.Workloads) && reference.SuiteRegressionPercent > options.MaxSuiteRegressionPercent {
			passed = false
		}
		result.PairedReference = reference
	}
	if options.ComparePath != "" {
		baselineContent, err := os.ReadFile(resolve(options.ComparePath))
		if err != nil {
			return ReplayBenchmarkResult{}, false, fmt.Errorf("read replay benchmark baseline: %w", err)
		}
		var baseline ReplayBenchmarkResult
		if err := json.Unmarshal(baselineContent, &baseline); err != nil {
			return ReplayBenchmarkResult{}, false, fmt.Errorf("parse replay benchmark baseline: %w", err)
		}
		if !equalReplayInputs(result.Inputs, baseline.Inputs) {
			passed = false
		}
		for name, current := range result.Workloads {
			old, found := baseline.Workloads[name]
			if !found || !equalReplayInputValue(current.Input, old.Input) ||
				!equalReplayArtifacts(current.FinalArtifactSHA256, old.FinalArtifactSHA256) || old.Performance.MedianSeconds <= 0 ||
				current.Performance.MedianSeconds/old.Performance.MedianSeconds-1 > options.MaxRegressionPercent/100 {
				passed = false
			}
		}
		if len(result.Workloads) == len(baseline.Workloads) && baseline.SuiteGeometricMeanFPS > 0 &&
			baseline.SuiteGeometricMeanFPS/result.SuiteGeometricMeanFPS-1 > options.MaxSuiteRegressionPercent/100 {
			passed = false
		}
	}
	if options.OutputPath != "" {
		outputPath := resolve(options.OutputPath)
		if err := os.MkdirAll(filepath.Dir(outputPath), 0o755); err != nil {
			return ReplayBenchmarkResult{}, false, err
		}
		content, _ := json.MarshalIndent(result, "", "  ")
		content = append(content, '\n')
		if err := os.WriteFile(outputPath, content, 0o644); err != nil {
			return ReplayBenchmarkResult{}, false, fmt.Errorf("write replay benchmark result: %w", err)
		}
	}
	return result, passed, nil
}

func describeReplayArtifactDifferences(candidate, reference map[string]string) string {
	keys := make(map[string]struct{}, len(candidate)+len(reference))
	for key := range candidate {
		keys[key] = struct{}{}
	}
	for key := range reference {
		keys[key] = struct{}{}
	}
	ordered := make([]string, 0, len(keys))
	for key := range keys {
		ordered = append(ordered, key)
	}
	sort.Strings(ordered)
	var differences []string
	for _, key := range ordered {
		left, leftFound := candidate[key]
		right, rightFound := reference[key]
		if leftFound && rightFound && left == right {
			continue
		}
		if !leftFound {
			differences = append(differences, fmt.Sprintf("%s missing from candidate", key))
			continue
		}
		if !rightFound {
			differences = append(differences, fmt.Sprintf("%s missing from reference", key))
			continue
		}
		differences = append(differences, fmt.Sprintf("%s candidate=%s reference=%s", key, left, right))
	}
	if len(differences) == 0 {
		return "unknown difference"
	}
	return strings.Join(differences, "; ")
}

// ioWriter is the small common surface used for optional live progress.
type ioWriter interface{ Write([]byte) (int, error) }

func equalReplayArtifacts(left, right map[string]string) bool {
	if len(left) != len(right) {
		return false
	}
	for key, value := range left {
		if right[key] != value {
			return false
		}
	}
	return true
}

func equalReplayInputs(left, right map[string]ReplayBenchmarkFile) bool {
	if len(left) != len(right) {
		return false
	}
	for key, value := range left {
		other, found := right[key]
		if !found || value.Bytes != other.Bytes || value.SHA256 != other.SHA256 {
			return false
		}
	}
	return true
}

func equalReplayInputValue(left, right map[string]any) bool {
	leftJSON, leftErr := canonicalReplayInputValue(left)
	rightJSON, rightErr := canonicalReplayInputValue(right)
	return leftErr == nil && rightErr == nil && string(leftJSON) == string(rightJSON)
}

func canonicalReplayInputValue(value map[string]any) ([]byte, error) {
	content, err := json.Marshal(value)
	if err != nil {
		return nil, err
	}
	var decoded any
	if err := json.Unmarshal(content, &decoded); err != nil {
		return nil, err
	}
	return json.Marshal(stripReplayEvidencePaths(decoded))
}

func stripReplayEvidencePaths(value any) any {
	switch typed := value.(type) {
	case map[string]any:
		cleaned := make(map[string]any, len(typed))
		for key, child := range typed {
			if key != "path" {
				cleaned[key] = stripReplayEvidencePaths(child)
			}
		}
		return cleaned
	case []any:
		cleaned := make([]any, len(typed))
		for index, child := range typed {
			cleaned[index] = stripReplayEvidencePaths(child)
		}
		return cleaned
	default:
		return value
	}
}

func WriteReplayBenchmarkSummary(result ReplayBenchmarkResult, passed bool, output ioWriter) {
	names := make([]string, 0, len(result.Workloads))
	for name := range result.Workloads {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		workload := result.Workloads[name]
		fmt.Fprintf(output, "  %-20s %8.4f s  %9.2f emulated fps\n", name, workload.Performance.MedianSeconds, workload.Performance.MedianEmulatedFPS)
	}
	fmt.Fprintf(output, "  suite geometric mean: %.2f emulated fps\n", result.SuiteGeometricMeanFPS)
	if result.PairedReference != nil {
		for _, name := range names {
			delta := result.PairedReference.MedianAdjacentDeltaPercent[name]
			fmt.Fprintf(output, "  %-20s %+.2f%%\n", name, delta)
		}
		fmt.Fprintf(output, "  suite regression: %+.2f%%\n", result.PairedReference.SuiteRegressionPercent)
	}
	if passed {
		fmt.Fprintln(output, "replay benchmark: PASS")
	} else {
		fmt.Fprintln(output, "replay benchmark: FAIL")
	}
}
