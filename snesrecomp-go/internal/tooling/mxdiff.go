package tooling

import (
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

const mxDiffVersion = 1

type MXDiffOptions struct {
	RecompPath, OraclePath                          string
	RecompFrameColumn, RecompMColumn, RecompXColumn int
	OracleFrameColumn, OracleMColumn, OracleXColumn int
	Offset                                          int64
	From, To                                        *uint64
	Context, Top                                    int
}

type MXDiffRow struct {
	GameFrame uint64 `json:"game_frame"`
	RecompM   int    `json:"recomp_m"`
	RecompX   int    `json:"recomp_x"`
	OracleM   int    `json:"oracle_m"`
	OracleX   int    `json:"oracle_x"`
	Mismatch  bool   `json:"mismatch"`
}

type MXDiffReport struct {
	Version          int         `json:"version"`
	Mode             string      `json:"mode"`
	NoWrite          bool        `json:"no_write"`
	RecompSHA256     string      `json:"recomp_sha256"`
	OracleSHA256     string      `json:"oracle_sha256"`
	Offset           int64       `json:"oracle_frame_offset"`
	RecompSamples    int         `json:"recomp_samples"`
	OracleSamples    int         `json:"oracle_samples"`
	CommonFrames     int         `json:"common_frames"`
	ComparedFrames   int         `json:"compared_frames"`
	UnresolvedOracle int         `json:"unresolved_oracle_frames"`
	DivergentFrames  int         `json:"divergent_frames"`
	FirstDivergence  *uint64     `json:"first_divergence,omitempty"`
	Rows             []MXDiffRow `json:"rows,omitempty"`
	RowsTruncated    int         `json:"rows_truncated,omitempty"`
}

type mxSample struct{ M, X int }

func loadMXSamples(path string, frameColumn, mColumn, xColumn int) ([]byte, map[uint64]mxSample, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, fmt.Errorf("read M/X trace %s: %w", path, err)
	}
	if frameColumn < 0 || mColumn < 0 || xColumn < 0 {
		return nil, nil, fmt.Errorf("M/X trace column indexes cannot be negative")
	}
	maximumColumn := frameColumn
	if mColumn > maximumColumn {
		maximumColumn = mColumn
	}
	if xColumn > maximumColumn {
		maximumColumn = xColumn
	}
	samples := make(map[uint64]mxSample)
	scanner := bufio.NewScanner(bytes.NewReader(content))
	line := 0
	for scanner.Scan() {
		line++
		fields := strings.Fields(scanner.Text())
		if len(fields) <= maximumColumn || strings.HasPrefix(strings.TrimSpace(scanner.Text()), "#") {
			continue
		}
		frame, frameErr := strconv.ParseUint(fields[frameColumn], 0, 64)
		m, mErr := strconv.Atoi(fields[mColumn])
		x, xErr := strconv.Atoi(fields[xColumn])
		if frameErr != nil || mErr != nil || xErr != nil {
			continue
		}
		if (m < -1 || m > 1) || (x < -1 || x > 1) {
			return nil, nil, fmt.Errorf("M/X trace %s line %d has invalid m=%d x=%d", path, line, m, x)
		}
		samples[frame] = mxSample{m, x}
	}
	if err := scanner.Err(); err != nil {
		return nil, nil, fmt.Errorf("scan M/X trace %s: %w", path, err)
	}
	return content, samples, nil
}

func BuildMXDiff(options MXDiffOptions) (MXDiffReport, error) {
	if options.RecompFrameColumn == 0 && options.RecompMColumn == 0 && options.RecompXColumn == 0 {
		options.RecompMColumn, options.RecompXColumn = 1, 2
	}
	if options.OracleFrameColumn == 0 && options.OracleMColumn == 0 && options.OracleXColumn == 0 {
		options.OracleMColumn, options.OracleXColumn = 1, 2
	}
	if options.Top <= 0 {
		options.Top = 40
	}
	if options.Context < 0 {
		return MXDiffReport{}, fmt.Errorf("M/X diff context cannot be negative")
	}
	recompContent, recomp, err := loadMXSamples(options.RecompPath, options.RecompFrameColumn, options.RecompMColumn, options.RecompXColumn)
	if err != nil {
		return MXDiffReport{}, err
	}
	oracleContent, oracleRaw, err := loadMXSamples(options.OraclePath, options.OracleFrameColumn, options.OracleMColumn, options.OracleXColumn)
	if err != nil {
		return MXDiffReport{}, err
	}
	oracle := make(map[uint64]mxSample)
	for frame, sample := range oracleRaw {
		shifted := int64(frame) + options.Offset
		if shifted >= 0 {
			oracle[uint64(shifted)] = sample
		}
	}
	recompHash, oracleHash := sha256.Sum256(recompContent), sha256.Sum256(oracleContent)
	report := MXDiffReport{
		Version: mxDiffVersion, Mode: "game-frame-mx-diff", NoWrite: true, Offset: options.Offset,
		RecompSHA256: hex.EncodeToString(recompHash[:]), OracleSHA256: hex.EncodeToString(oracleHash[:]),
		RecompSamples: len(recomp), OracleSamples: len(oracleRaw),
	}
	var common []uint64
	for frame := range recomp {
		if _, found := oracle[frame]; found {
			common = append(common, frame)
		}
	}
	sort.Slice(common, func(i, j int) bool { return common[i] < common[j] })
	report.CommonFrames = len(common)
	var divergent []uint64
	for _, frame := range common {
		if options.From != nil && frame < *options.From || options.To != nil && frame > *options.To {
			continue
		}
		oracleSample := oracle[frame]
		if oracleSample.M < 0 || oracleSample.X < 0 {
			report.UnresolvedOracle++
			continue
		}
		report.ComparedFrames++
		if recomp[frame] != oracleSample {
			divergent = append(divergent, frame)
		}
	}
	report.DivergentFrames = len(divergent)
	if len(divergent) == 0 {
		return report, nil
	}
	first := divergent[0]
	report.FirstDivergence = &first
	contextLow := first
	if uint64(options.Context) < first {
		contextLow = first - uint64(options.Context)
	} else {
		contextLow = 0
	}
	contextHigh := first + uint64(options.Context)
	for _, frame := range common {
		if frame < contextLow || frame > contextHigh {
			continue
		}
		r, o := recomp[frame], oracle[frame]
		report.Rows = append(report.Rows, MXDiffRow{frame, r.M, r.X, o.M, o.X, o.M >= 0 && o.X >= 0 && r != o})
	}
	if len(report.Rows) > options.Top {
		report.RowsTruncated = len(report.Rows) - options.Top
		report.Rows = report.Rows[:options.Top]
	}
	return report, nil
}

func WriteMXDiff(writer io.Writer, report MXDiffReport, format string) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(writer)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		fmt.Fprintf(writer, "M/X diff: common=%d compared=%d divergent=%d unresolved_oracle=%d offset=%+d\n",
			report.CommonFrames, report.ComparedFrames, report.DivergentFrames, report.UnresolvedOracle, report.Offset)
		if report.FirstDivergence == nil {
			fmt.Fprintln(writer, "  no M/X divergence in the selected frame range")
			return nil
		}
		fmt.Fprintf(writer, "  first divergence at game-frame %d\n", *report.FirstDivergence)
		for _, row := range report.Rows {
			marker := " "
			if row.Mismatch {
				marker = "*"
			}
			fmt.Fprintf(writer, "  %s %6d: recomp m%d x%d  oracle m%d x%d\n", marker, row.GameFrame, row.RecompM, row.RecompX, row.OracleM, row.OracleX)
		}
		if report.RowsTruncated != 0 {
			fmt.Fprintf(writer, "... %d more context row(s)\n", report.RowsTruncated)
		}
		return nil
	default:
		return fmt.Errorf("unknown M/X diff format %q (want text or json)", format)
	}
}

func parseMXColumns(value string) ([3]int, error) {
	parts := strings.Split(value, ",")
	if len(parts) != 3 {
		return [3]int{}, fmt.Errorf("column layout %q needs frame,m,x", value)
	}
	var columns [3]int
	for index, part := range parts {
		parsed, err := strconv.Atoi(strings.TrimSpace(part))
		if err != nil || parsed < 0 {
			return [3]int{}, fmt.Errorf("parse column layout %q", value)
		}
		columns[index] = parsed
	}
	return columns, nil
}

func RunMXDiffCommand(args []string, defaultRoot string, output io.Writer) error {
	if len(args) < 2 || strings.HasPrefix(args[0], "-") || strings.HasPrefix(args[1], "-") {
		return fmt.Errorf("mx-diff needs RECOMP_TRACE ORACLE_TRACE before its options")
	}
	recompPath, oraclePath := args[0], args[1]
	flags := flag.NewFlagSet("mx-diff", flag.ContinueOnError)
	rootValue := flags.String("root", defaultRoot, "game project root used to resolve relative paths")
	offset := flags.Int64("offset", 0, "add this signed frame offset to oracle samples")
	from := flags.Int64("from", -1, "first recompiled game frame to compare")
	to := flags.Int64("to", -1, "last recompiled game frame to compare")
	context := flags.Int("context", 8, "frames of context around the first divergence")
	top := flags.Int("top", 40, "maximum context rows")
	recompColumnsValue := flags.String("recomp-columns", "0,1,2", "zero-based frame,m,x columns")
	oracleColumnsValue := flags.String("oracle-columns", "0,1,2", "zero-based frame,m,x columns")
	format := flags.String("format", "text", "report format: text or json")
	if err := flags.Parse(args[2:]); err != nil {
		return err
	}
	if flags.NArg() != 0 || *from < -1 || *to < -1 || (*from >= 0 && *to >= 0 && *to < *from) {
		return fmt.Errorf("invalid mx-diff frame range or trailing argument")
	}
	recompColumns, err := parseMXColumns(*recompColumnsValue)
	if err != nil {
		return err
	}
	oracleColumns, err := parseMXColumns(*oracleColumnsValue)
	if err != nil {
		return err
	}
	root, err := filepath.Abs(*rootValue)
	if err != nil {
		return fmt.Errorf("resolve M/X diff project root: %w", err)
	}
	options := MXDiffOptions{
		RecompPath: resolveWRAMPath(root, recompPath), OraclePath: resolveWRAMPath(root, oraclePath), Offset: *offset,
		RecompFrameColumn: recompColumns[0], RecompMColumn: recompColumns[1], RecompXColumn: recompColumns[2],
		OracleFrameColumn: oracleColumns[0], OracleMColumn: oracleColumns[1], OracleXColumn: oracleColumns[2],
		Context: *context, Top: *top,
	}
	if *from >= 0 {
		value := uint64(*from)
		options.From = &value
	}
	if *to >= 0 {
		value := uint64(*to)
		options.To = &value
	}
	report, err := BuildMXDiff(options)
	if err != nil {
		return err
	}
	return WriteMXDiff(output, report, *format)
}
