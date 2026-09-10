package localization

import (
	"bytes"
	"encoding/hex"
	"fmt"
)

// IRObject belongs to the local, diagnostic extraction IR. It is not an
// executable event, author document, or GUI model.
type IRObject map[string]any

func offsetPC(offset int) int      { return (offset/0x8000)<<16 | 0x8000 | offset&0x7fff }
func pcString(pc int) string       { return fmt.Sprintf("$%02X:%04X", pc>>16, pc&0xffff) }
func localString(local int) string { return fmt.Sprintf("$%04X", local) }

func (d *Decoder) span(offset, count int) ([]byte, error) {
	if offset < 0 || count < 0 || count > len(d.rom) || offset > len(d.rom)-count {
		return nil, fmt.Errorf("ROM span outside image: %#x + %#x", offset, count)
	}
	return d.rom[offset : offset+count], nil
}
func (d *Decoder) pcSpan(pc, count int) ([]byte, error) {
	offset, err := pc24Offset(pc)
	if err != nil {
		return nil, err
	}
	return d.span(offset, count)
}
func (d *Decoder) expectPC(pc int, expected []byte, role string) error {
	actual, err := d.pcSpan(pc, len(expected))
	if err != nil || !bytes.Equal(actual, expected) {
		return fmt.Errorf("%s: %s signature changed at %s", d.profile.ID, role, pcString(pc))
	}
	return nil
}

// Only compile-time instruction strings pass through this helper.
func instructionBytes(value string) []byte {
	result, err := hex.DecodeString(value)
	if err != nil {
		panic("invalid instruction signature: " + err.Error())
	}
	return result
}

var interactiveSignature = instructionBytes("08dae220a9ff8d0102ad0002d010")
var composerSignature = instructionBytes("08dac2208514e220a515eba940")

func scanPattern(rom, pattern []byte) []int {
	sites := []int{}
	if len(pattern) == 0 {
		return sites
	}
	for start := 0; start <= len(rom)-len(pattern); {
		found := bytes.Index(rom[start:], pattern)
		if found < 0 {
			break
		}
		start += found
		sites = append(sites, offsetPC(start))
		start++ // Native raw census includes overlapping matches.
	}
	return sites
}
func callPattern(entry int, kind string) ([]byte, error) {
	if _, err := pc24Offset(entry); err != nil {
		return nil, err
	}
	switch kind {
	case "jsr":
		return []byte{0x20, byte(entry), byte(entry >> 8)}, nil
	case "jsl":
		return []byte{0x22, byte(entry), byte(entry >> 8), byte(entry >> 16)}, nil
	default:
		return nil, fmt.Errorf("unsupported call kind %q", kind)
	}
}
func (d *Decoder) callsTo(entry int, kind string, bankLocal bool) ([]int, error) {
	pattern, err := callPattern(entry, kind)
	if err != nil {
		return nil, err
	}
	sites := scanPattern(d.rom, pattern)
	if kind == "jsr" && bankLocal {
		filtered := sites[:0]
		for _, site := range sites {
			if site>>16 == entry>>16 {
				filtered = append(filtered, site)
			}
		}
		sites = filtered
	}
	return sites, nil
}
func (d *Decoder) directY(call int) (*int, error) {
	offset, err := pc24Offset(call)
	if err != nil {
		return nil, err
	}
	if offset >= len(d.rom) {
		return nil, fmt.Errorf("call outside ROM: %s", pcString(call))
	}
	if offset < 3 {
		return nil, nil
	}
	data := d.rom[offset-3 : offset]
	if data[0] != 0xa0 {
		return nil, nil
	}
	source := word(data[1:])
	return &source, nil
}
func branchTarget(rom []byte, offset int) (int, bool) {
	if offset < 0 || offset >= len(rom)-1 {
		return 0, false
	}
	switch rom[offset] {
	case 0x10, 0x30, 0x50, 0x70, 0x80, 0x90, 0xb0, 0xd0, 0xf0:
		return offset + 2 + int(int8(rom[offset+1])), true
	case 0x82:
		if offset >= len(rom)-2 {
			return 0, false
		}
		return offset + 3 + int(int16(word(rom[offset+1:]))), true
	}
	return 0, false
}

type incomingSource struct {
	Branch, Y int
	Shape     string
}

func (d *Decoder) incomingY(call int) ([]incomingSource, error) {
	target, err := pc24Offset(call)
	if err != nil || target >= len(d.rom) {
		return nil, fmt.Errorf("invalid branch-join target %s", pcString(call))
	}
	start := target / 0x8000 * 0x8000
	end := min(start+0x8000, len(d.rom))
	sources := []incomingSource{}
	for offset := start; offset < end-2; offset++ {
		destination, valid := branchTarget(d.rom, offset)
		if !valid || destination != target {
			continue
		}
		if offset >= start+3 && d.rom[offset-3] == 0xa0 {
			sources = append(sources, incomingSource{offsetPC(offset), word(d.rom[offset-2:]), "ldy_immediate_then_branch"})
		} else if offset >= start+4 && d.rom[offset-4] == 0xa0 && d.rom[offset-1] == 0x3a {
			sources = append(sources, incomingSource{offsetPC(offset), word(d.rom[offset-3:]), "ldy_immediate_dec_a_then_branch"})
		}
	}
	return sources, nil
}
func (d *Decoder) conditionalY(call int) ([]int, error) {
	offset, err := pc24Offset(call)
	if err != nil {
		return nil, err
	}
	data, err := d.span(offset-21, 21)
	if err != nil || data[20] != 0x68 || data[0] != 0xa0 || data[10] != 0xa0 || data[17] != 0xa0 {
		return nil, fmt.Errorf("conditional source loads changed at %s", pcString(call))
	}
	for _, branch := range []int{offset - 13, offset - 6} {
		target, ok := branchTarget(d.rom, branch)
		if d.rom[branch] != 0xf0 || !ok || target != offset-1 {
			return nil, fmt.Errorf("conditional source branch changed at %s", pcString(call))
		}
	}
	return []int{word(data[1:]), word(data[11:]), word(data[18:])}, nil
}
func sourcePC(bank, local int) (int, error) {
	if bank < 0 || bank > 0x7f || local < 0x8000 || local > 0xffff {
		return 0, fmt.Errorf("invalid source bank/local: %#x/%#x", bank, local)
	}
	return bank<<16 | local, nil
}
func (d *Decoder) pointerTargets(table, count, bank int) ([]int, error) {
	if count < 0 || count > 0x4000 {
		return nil, fmt.Errorf("invalid pointer count %d", count)
	}
	data, err := d.pcSpan(table, count*2)
	if err != nil {
		return nil, err
	}
	targets := make([]int, 0, count)
	for i := 0; i < count; i++ {
		target, err := sourcePC(bank, word(data[i*2:]))
		if err != nil {
			return nil, err
		}
		if _, err := d.pcSpan(target, 1); err != nil {
			return nil, err
		}
		targets = append(targets, target)
	}
	return targets, nil
}
func distinctInts(values []int) []int {
	result := []int{}
	seen := make(map[int]bool, len(values))
	for _, value := range values {
		if !seen[value] {
			seen[value] = true
			result = append(result, value)
		}
	}
	return result
}
func pcStrings(values []int) []string {
	result := make([]string, 0, len(values))
	for _, value := range values {
		result = append(result, pcString(value))
	}
	return result
}
func localStrings(values []int) []string {
	result := make([]string, 0, len(values))
	for _, value := range values {
		result = append(result, localString(value))
	}
	return result
}
func incomingRows(sources []incomingSource) []IRObject {
	rows := make([]IRObject, 0, len(sources))
	for _, source := range sources {
		rows = append(rows, IRObject{"branch_site": pcString(source.Branch), "source_y": localString(source.Y), "shape": source.Shape})
	}
	return rows
}
func candidateYs(incoming []incomingSource, fallthroughY int) []int {
	values := make([]int, 0, len(incoming)+1)
	for _, source := range incoming {
		values = append(values, source.Y)
	}
	return distinctInts(append(values, fallthroughY))
}

func (d *Decoder) nativeDictionaryProof(p sourceProfile) (IRObject, error) {
	result := IRObject{}
	if d.profile.Encoding != "dictionary-12" {
		return result, nil
	}
	proof := p.DictionaryProof
	if proof == nil {
		return nil, fmt.Errorf("missing dictionary code proof")
	}
	dictionary := offsetPC(d.profile.Dictionary)
	load := []byte{0xb9, byte(dictionary), byte(dictionary >> 8)}
	prefix := instructionBytes("b900003002c860c88b5ac220297f00480a1863010a0aa868e220a9")
	prefix = append(prefix, byte(dictionary>>16))
	prefix = append(prefix, instructionBytes("48aba90c")...)
	for _, consumer := range []Consumer{Interactive, FixedComposer} {
		entry, owner, end := proof.Fixed, p.ComposerEntry, p.ComposerEnd
		suffix := []byte{0xeb}
		if consumer == Interactive {
			entry, owner, end = proof.Interactive, p.InteractiveEntry, p.InteractiveEnd
			suffix = append(suffix, 0xe6, proof.UploadFlag)
		}
		suffix = append(suffix, load...)
		if consumer == Interactive {
			suffix = append(suffix, instructionBytes("9f00b07fe8e8c8c920f0")...)
			displacement := byte(0x09)
			if proof.TrailingSpace {
				displacement = 0x11
			}
			suffix = append(suffix, displacement)
			suffix = append(suffix, instructionBytes("eb48201c90683ad0e6")...)
			if proof.TrailingSpace {
				suffix = append(suffix, instructionBytes("a9209f00b07fe8e8")...)
			}
			suffix = append(suffix, 0x7a, 0xab, 0x80)
			displacement = 0xc2
			if proof.TrailingSpace {
				displacement = 0xba
			}
			suffix = append(suffix, displacement)
		} else {
			suffix = append(suffix, instructionBytes("f00f9f00b07fe8e8c8c920f004eb3ad0eb7aab80c7")...)
		}
		expected := append(append([]byte{}, prefix...), suffix...)
		if err := d.expectPC(entry, expected, string(consumer)+" dictionary reader"); err != nil {
			return nil, err
		}
		startOffset, err := pc24Offset(owner)
		if err != nil {
			return nil, err
		}
		endOffset, err := pc24Offset(end)
		if err != nil {
			return nil, err
		}
		body, err := d.span(startOffset, endOffset-startOffset)
		if err != nil {
			return nil, err
		}
		pattern, err := callPattern(entry, "jsr")
		if err != nil {
			return nil, err
		}
		if !bytes.Contains(body, pattern) {
			return nil, fmt.Errorf("%s dictionary reader is not called by its consumer", consumer)
		}
		result[string(consumer)] = IRObject{"reader_pc24": pcString(entry), "entry_bytes": 12,
			"stop_after_space": "20", "stop_before_zero": consumer == FixedComposer,
			"append_space_on_full_entry": consumer == Interactive && proof.TrailingSpace}
	}
	return result, nil
}
func (d *Decoder) nativeLayout(p sourceProfile) (IRObject, error) {
	entry, err := d.pcSpan(p.InteractiveEntry, 20)
	if err != nil || entry[14] != 0xa2 || entry[17] != 0x20 {
		return nil, fmt.Errorf("unrecognized dialogue cursor/clear entry")
	}
	clear, err := sourcePC(p.InteractiveEntry>>16, word(entry[18:]))
	if err != nil {
		return nil, err
	}
	data, err := d.pcSpan(clear, 20)
	if err != nil || !bytes.Equal(data[:2], []byte{0xda, 0xa2}) ||
		!bytes.Equal(data[4:12], instructionBytes("a90648daa900eba9")) ||
		!bytes.Equal(data[13:20], instructionBytes("eb9f00b07fe8e8")) {
		return nil, fmt.Errorf("unrecognized dialogue clear-loop geometry")
	}
	origin, columns := word(data[2:]), int(data[12])
	if origin&1 != 0 || origin >= 0x800 || columns < 1 || columns > 32 || (origin&63)/2+columns > 32 {
		return nil, fmt.Errorf("invalid native dialogue row extent")
	}
	return IRObject{"column": (origin & 63) / 2, "row": origin / 64, "columns": columns,
		"glyph_advance_cells": 1, "space_delimited_words": p.SpaceDelimitedWords,
		"clear_routine_pc24": pcString(clear)}, nil
}
