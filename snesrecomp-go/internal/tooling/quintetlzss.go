package tooling

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
)

const quintetLZSSReportVersion = 1

type QuintetLZSSOptions struct {
	InputPath     string
	Offset        int
	Size          int
	Headered      bool
	ComparePath   string
	CompareOffset int
}

type QuintetLZSSReport struct {
	Version             int    `json:"version"`
	Algorithm           string `json:"algorithm"`
	InputSHA256         string `json:"input_sha256"`
	InputOffset         int    `json:"input_offset"`
	HeaderBytes         int    `json:"header_bytes"`
	CompressedBytesRead int    `json:"compressed_bytes_read"`
	OutputBytes         int    `json:"output_bytes"`
	OutputSHA256        string `json:"output_sha256"`
	PreviewHex          string `json:"preview_hex"`
	Compared            bool   `json:"compared"`
	CompareMatch        bool   `json:"compare_match,omitempty"`
	FirstDifference     *int   `json:"first_difference,omitempty"`
	NoWrite             bool   `json:"no_write"`
}

type quintetBitReader struct {
	data   []byte
	bitPos int
}

func (reader *quintetBitReader) read(count int) (uint32, error) {
	var value uint32
	for index := 0; index < count; index++ {
		if reader.bitPos < 0 || reader.bitPos/8 >= len(reader.data) {
			return 0, io.ErrUnexpectedEOF
		}
		current := reader.data[reader.bitPos/8]
		value = value<<1 | uint32((current>>uint(7-reader.bitPos%8))&1)
		reader.bitPos++
	}
	return value, nil
}

// DecompressQuintetLZSS decodes the bit-packed 256-byte-ring format used by
// several Quintet-era games. If headered is true, offset points at a little-
// endian decompressed-size word; otherwise size supplies the exact output
// length. The compressed byte count excludes the optional two-byte header.
func DecompressQuintetLZSS(input []byte, offset, size int, headered bool) ([]byte, int, error) {
	if offset < 0 || offset > len(input) {
		return nil, 0, fmt.Errorf("input offset %#x is outside %d-byte input", offset, len(input))
	}
	streamOffset := offset
	if headered {
		if offset+2 > len(input) {
			return nil, 0, fmt.Errorf("read decompressed-size header at %#x: %w", offset, io.ErrUnexpectedEOF)
		}
		size = int(input[offset]) | int(input[offset+1])<<8
		streamOffset += 2
	}
	if size < 0 {
		return nil, 0, fmt.Errorf("negative decompressed size %d", size)
	}
	if size == 0 {
		return []byte{}, 0, nil
	}
	reader := quintetBitReader{data: input, bitPos: streamOffset * 8}
	ring := [256]byte{}
	for index := range ring {
		ring[index] = 0x20
	}
	writePosition := 0xef
	output := make([]byte, 0, size)
	for len(output) < size {
		literal, err := reader.read(1)
		if err != nil {
			return nil, 0, fmt.Errorf("read control bit after %d output bytes: %w", len(output), err)
		}
		if literal != 0 {
			value, err := reader.read(8)
			if err != nil {
				return nil, 0, fmt.Errorf("read literal after %d output bytes: %w", len(output), err)
			}
			item := byte(value)
			output = append(output, item)
			ring[writePosition] = item
			writePosition = (writePosition + 1) & 0xff
			continue
		}
		sourceValue, err := reader.read(8)
		if err != nil {
			return nil, 0, fmt.Errorf("read match source after %d output bytes: %w", len(output), err)
		}
		lengthValue, err := reader.read(4)
		if err != nil {
			return nil, 0, fmt.Errorf("read match length after %d output bytes: %w", len(output), err)
		}
		source := int(sourceValue)
		for count := int(lengthValue) + 2; count > 0 && len(output) < size; count-- {
			item := ring[source]
			source = (source + 1) & 0xff
			output = append(output, item)
			ring[writePosition] = item
			writePosition = (writePosition + 1) & 0xff
		}
	}
	consumed := (reader.bitPos+7)/8 - streamOffset
	return output, consumed, nil
}

func BuildQuintetLZSS(options QuintetLZSSOptions) (QuintetLZSSReport, []byte, error) {
	input, err := os.ReadFile(options.InputPath)
	if err != nil {
		return QuintetLZSSReport{}, nil, fmt.Errorf("read LZSS input %s: %w", options.InputPath, err)
	}
	output, consumed, err := DecompressQuintetLZSS(input, options.Offset, options.Size, options.Headered)
	if err != nil {
		return QuintetLZSSReport{}, nil, err
	}
	inputHash, outputHash := sha256.Sum256(input), sha256.Sum256(output)
	preview := output
	if len(preview) > 32 {
		preview = preview[:32]
	}
	report := QuintetLZSSReport{
		Version: quintetLZSSReportVersion, Algorithm: "quintet-bitpacked-lzss",
		InputSHA256: hex.EncodeToString(inputHash[:]), InputOffset: options.Offset,
		CompressedBytesRead: consumed, OutputBytes: len(output), OutputSHA256: hex.EncodeToString(outputHash[:]),
		PreviewHex: hex.EncodeToString(preview), NoWrite: true,
	}
	if options.Headered {
		report.HeaderBytes = 2
	}
	if options.ComparePath != "" {
		compare, readErr := os.ReadFile(options.ComparePath)
		if readErr != nil {
			return QuintetLZSSReport{}, nil, fmt.Errorf("read LZSS comparison %s: %w", options.ComparePath, readErr)
		}
		if options.CompareOffset < 0 || options.CompareOffset > len(compare) {
			return QuintetLZSSReport{}, nil, fmt.Errorf("comparison offset %#x is outside %d-byte input", options.CompareOffset, len(compare))
		}
		compare = compare[options.CompareOffset:]
		report.Compared = true
		report.CompareMatch = len(compare) == len(output) && bytes.Equal(compare, output)
		if !report.CompareMatch {
			first := 0
			for first < len(compare) && first < len(output) && compare[first] == output[first] {
				first++
			}
			report.FirstDifference = &first
		}
	}
	return report, output, nil
}

func WriteQuintetLZSSReport(writer io.Writer, report QuintetLZSSReport, format string) error {
	switch format {
	case "json":
		encoder := json.NewEncoder(writer)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		fmt.Fprintf(writer, "Quintet LZSS: offset=$%06X output=%d compressed=%d sha256=%s\n",
			report.InputOffset, report.OutputBytes, report.CompressedBytesRead, report.OutputSHA256)
		if report.PreviewHex != "" {
			fmt.Fprintf(writer, "  first bytes: %s\n", report.PreviewHex)
		}
		if report.Compared {
			if report.CompareMatch {
				fmt.Fprintln(writer, "  comparison: MATCH")
			} else {
				fmt.Fprintf(writer, "  comparison: MISMATCH at +$%X\n", *report.FirstDifference)
			}
		}
		return nil
	default:
		return fmt.Errorf("unknown LZSS report format %q (want text or json)", format)
	}
}
