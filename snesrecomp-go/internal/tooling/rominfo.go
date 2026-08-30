package tooling

import (
	"crypto/sha1"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"strings"
	"unicode"

	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const romInfoVersion = 1

type ROMInfoOptions struct {
	ROMPath string
}

type ROMHeaderCandidate struct {
	Offset             int    `json:"offset"`
	Mapper             string `json:"mapper"`
	Title              string `json:"title"`
	MapMode            byte   `json:"map_mode"`
	FastROM            bool   `json:"fastrom"`
	CartridgeType      byte   `json:"cartridge_type"`
	DeclaredROMBytes   uint64 `json:"declared_rom_bytes"`
	DeclaredSRAMBytes  uint64 `json:"declared_sram_bytes"`
	RegionCode         byte   `json:"region_code"`
	Region             string `json:"region"`
	Version            byte   `json:"version"`
	ChecksumComplement uint16 `json:"checksum_complement"`
	Checksum           uint16 `json:"checksum"`
	ComplementValid    bool   `json:"complement_valid"`
	Score              int    `json:"score"`
}

type ROMVector struct {
	Name    string `json:"name"`
	Address uint16 `json:"address"`
}

type ROMInfoReport struct {
	Version          int                  `json:"version"`
	Mode             string               `json:"mode"`
	NoWrite          bool                 `json:"no_write"`
	RawSize          int                  `json:"raw_size"`
	ROMSize          int                  `json:"rom_size"`
	CopierHeader     bool                 `json:"copier_header"`
	RawSHA256        string               `json:"raw_sha256"`
	SHA256           string               `json:"sha256"`
	SHA1             string               `json:"sha1"`
	CRC32            uint32               `json:"crc32"`
	ByteSum16        uint16               `json:"byte_sum_16"`
	Header           *ROMHeaderCandidate  `json:"header,omitempty"`
	HeaderCandidates []ROMHeaderCandidate `json:"header_candidates,omitempty"`
	Vectors          []ROMVector          `json:"vectors,omitempty"`
	Warnings         []string             `json:"warnings,omitempty"`
}

func BuildROMInfo(options ROMInfoOptions) (ROMInfoReport, error) {
	raw, err := os.ReadFile(options.ROMPath)
	if err != nil {
		return ROMInfoReport{}, fmt.Errorf("read ROM: %w", err)
	}
	image, err := romimage.Load(options.ROMPath)
	if err != nil {
		return ROMInfoReport{}, err
	}
	rawHash, imageHash := sha256.Sum256(raw), sha256.Sum256(image)
	sha1Hash := sha1.Sum(image)
	report := ROMInfoReport{
		Version: romInfoVersion, Mode: "cartridge_identity_and_header", NoWrite: true,
		RawSize: len(raw), ROMSize: len(image), CopierHeader: len(raw) != len(image),
		RawSHA256: hex.EncodeToString(rawHash[:]), SHA256: hex.EncodeToString(imageHash[:]),
		SHA1: hex.EncodeToString(sha1Hash[:]), CRC32: crc32.ChecksumIEEE(image),
	}
	for _, value := range image {
		report.ByteSum16 += uint16(value)
	}
	candidateOffsets := []struct {
		offset int
		mapper string
	}{{0x7fc0, "lorom"}, {0xffc0, "hirom"}, {0x40ffc0, "exhirom"}}
	for _, item := range candidateOffsets {
		if item.offset+0x40 > len(image) {
			continue
		}
		report.HeaderCandidates = append(report.HeaderCandidates, parseROMHeaderCandidate(image, item.offset, item.mapper))
	}
	if len(report.HeaderCandidates) == 0 {
		report.Warnings = append(report.Warnings, "ROM is too small to contain a standard SNES header")
		return report, nil
	}
	best := 0
	for index := 1; index < len(report.HeaderCandidates); index++ {
		if report.HeaderCandidates[index].Score > report.HeaderCandidates[best].Score {
			best = index
		}
	}
	report.Header = &report.HeaderCandidates[best]
	report.Vectors = parseROMVectors(image, report.Header.Offset)
	if !report.Header.ComplementValid {
		report.Warnings = append(report.Warnings, "selected header checksum/complement pair is invalid")
	}
	if report.Header.DeclaredROMBytes != 0 && report.Header.DeclaredROMBytes != uint64(len(image)) {
		report.Warnings = append(report.Warnings, fmt.Sprintf("header declares %d ROM bytes, file contains %d", report.Header.DeclaredROMBytes, len(image)))
	}
	return report, nil
}

func parseROMHeaderCandidate(image []byte, offset int, assumedMapper string) ROMHeaderCandidate {
	read16 := func(relative int) uint16 { return uint16(image[offset+relative]) | uint16(image[offset+relative+1])<<8 }
	mapMode := image[offset+0x15]
	candidate := ROMHeaderCandidate{
		Offset: offset, Mapper: romMapperName(mapMode, assumedMapper), Title: cleanROMTitle(image[offset : offset+21]),
		MapMode: mapMode, FastROM: mapMode&0x10 != 0, CartridgeType: image[offset+0x16],
		DeclaredROMBytes: romDeclaredSize(image[offset+0x17]), DeclaredSRAMBytes: ramDeclaredSize(image[offset+0x18]),
		RegionCode: image[offset+0x19], Region: romRegionName(image[offset+0x19]), Version: image[offset+0x1b],
		ChecksumComplement: read16(0x1c), Checksum: read16(0x1e),
	}
	candidate.ComplementValid = candidate.Checksum^candidate.ChecksumComplement == 0xffff
	if candidate.ComplementValid {
		candidate.Score += 8
	}
	if candidate.Title != "" {
		candidate.Score += 2
	}
	if candidate.Mapper == assumedMapper || assumedMapper == "exhirom" && candidate.Mapper == "exhirom" {
		candidate.Score += 4
	}
	reset := read16(0x3c)
	if reset >= 0x8000 {
		candidate.Score += 4
	}
	return candidate
}

func cleanROMTitle(data []byte) string {
	var builder strings.Builder
	for _, value := range data {
		r := rune(value)
		if value == 0 {
			r = ' '
		} else if !unicode.IsPrint(r) {
			r = '�'
		}
		builder.WriteRune(r)
	}
	return strings.TrimSpace(builder.String())
}

func romMapperName(mode byte, fallback string) string {
	switch mode & 0x2f {
	case 0x20:
		return "lorom"
	case 0x21:
		return "hirom"
	case 0x25:
		return "exhirom"
	default:
		return fallback
	}
}

func romDeclaredSize(exponent byte) uint64 {
	if exponent >= 0x20 {
		return 0
	}
	return uint64(1024) << exponent
}

func ramDeclaredSize(exponent byte) uint64 {
	if exponent == 0 || exponent >= 0x20 {
		return 0
	}
	return uint64(1024) << exponent
}

func romRegionName(code byte) string {
	switch code {
	case 0:
		return "Japan"
	case 1:
		return "USA/Canada"
	case 2:
		return "Europe/Oceania/Asia"
	case 3:
		return "Sweden"
	case 4:
		return "Finland"
	case 5:
		return "Denmark"
	case 6:
		return "France"
	case 7:
		return "Netherlands"
	case 8:
		return "Spain"
	case 9:
		return "Germany/Austria/Switzerland"
	case 10:
		return "Italy"
	case 11:
		return "Hong Kong/China"
	case 13:
		return "South Korea"
	default:
		return fmt.Sprintf("unknown ($%02X)", code)
	}
}

func parseROMVectors(image []byte, headerOffset int) []ROMVector {
	entries := []struct {
		name     string
		relative int
	}{
		{"native_cop", 0x24}, {"native_brk", 0x26}, {"native_abort", 0x28},
		{"native_nmi", 0x2a}, {"native_reset", 0x2c}, {"native_irq", 0x2e},
		{"emulation_cop", 0x34}, {"emulation_abort", 0x38}, {"emulation_nmi", 0x3a},
		{"emulation_reset", 0x3c}, {"emulation_irq", 0x3e},
	}
	var vectors []ROMVector
	for _, entry := range entries {
		offset := headerOffset + entry.relative
		if offset+2 > len(image) {
			continue
		}
		address := uint16(image[offset]) | uint16(image[offset+1])<<8
		if address != 0 && address != 0xffff {
			vectors = append(vectors, ROMVector{Name: entry.name, Address: address})
		}
	}
	return vectors
}

func WriteROMInfo(output io.Writer, report ROMInfoReport, format string) error {
	switch strings.ToLower(strings.TrimSpace(format)) {
	case "json":
		encoder := json.NewEncoder(output)
		encoder.SetIndent("", "  ")
		return encoder.Encode(report)
	case "", "text":
		fmt.Fprintf(output, "ROM identity: %d bytes (raw %d, copier_header=%t)\n", report.ROMSize, report.RawSize, report.CopierHeader)
		fmt.Fprintf(output, "  SHA-256 %s\n  SHA-1   %s\n  CRC32   %08X\n", report.SHA256, report.SHA1, report.CRC32)
		if report.Header != nil {
			header := report.Header
			fmt.Fprintf(output, "Header @$%06X: %q, mapper=%s%s map_mode=$%02X type=$%02X\n",
				header.Offset, header.Title, header.Mapper, map[bool]string{true: "+fastrom"}[header.FastROM], header.MapMode, header.CartridgeType)
			fmt.Fprintf(output, "  ROM=%d SRAM=%d region=%s version=%d checksum=$%04X complement=$%04X valid=%t byte_sum=$%04X\n",
				header.DeclaredROMBytes, header.DeclaredSRAMBytes, header.Region, header.Version,
				header.Checksum, header.ChecksumComplement, header.ComplementValid, report.ByteSum16)
		}
		for _, vector := range report.Vectors {
			fmt.Fprintf(output, "  %-17s $00:%04X\n", vector.Name, vector.Address)
		}
		for _, warning := range report.Warnings {
			fmt.Fprintf(output, "warning: %s\n", warning)
		}
		return nil
	default:
		return fmt.Errorf("unknown ROM-info format %q (want text or json)", format)
	}
}
