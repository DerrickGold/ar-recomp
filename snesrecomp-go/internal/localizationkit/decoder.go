// Package localizationkit implements the builder's ROM-to-language toolchain.
// Consumer decoding is deliberately separate from source discovery and author
// export. Record decoding alone is not a complete extraction or playable pack.
package localizationkit

import (
	"crypto/sha256"
	_ "embed"
	"encoding/json"
	"fmt"
	"strings"
	"unicode/utf8"
)

const (
	romSize         = 0x100000
	dictionaryBytes = 128 * 12
	// Bound malformed synthetic inputs as well as recognized retail records.
	maxRecordBytes = 0x10000
)

// Consumer selects the native reader grammar. Shared dictionary storage does
// not imply shared termination or control-byte semantics.
type Consumer string

const (
	Interactive   Consumer = "interactive"
	FixedComposer Consumer = "fixed"
)

// Operation is a tagged, lossless extraction-IR object, not author syntax.
// Its fields follow language_pack_extract.py (including explicit null values
// for unresolved semantics). It is never executed by the game or GUI.
type Operation map[string]any

type Record struct {
	Operations       []Operation `json:"operations"`
	End              int         `json:"end"`
	Terminated       bool        `json:"terminated"`
	DictionaryTokens []string    `json:"dictionary_tokens"`
}

type iconPart struct {
	Value     string `json:"value"`
	PartIndex int    `json:"part_index"`
	PartCount int    `json:"part_count"`
}

type decoderProfile struct {
	ID                   string            `json:"id"`
	Locale               string            `json:"locale"`
	Label                string            `json:"label"`
	SHA256               string            `json:"sha256"`
	Encoding             string            `json:"encoding"`
	Dictionary           int               `json:"dictionary"`
	FullEntrySeparator   string            `json:"interactive_full_entry_separator"`
	Glyphs               map[byte]string   `json:"glyphs"`
	Icons                map[byte]iconPart `json:"icons"`
	NumberSemantics      map[string]string `json:"number_semantics"`
	IndexedTextSemantics map[string]string `json:"indexed_text_semantics"`
	NamePrefix           *int              `json:"name_prefix"`
	NameSeparator        string            `json:"name_separator"`
	PopulationSource     *int              `json:"population_source"`
	PopulationCodes      [2]byte           `json:"population_codes"`
	SpeedCodes           [2]byte           `json:"speed_codes"`
	SpeedSourceCall      *int              `json:"speed_source_call"`
}

//go:embed data/decoder-profiles.json
var decoderProfileJSON []byte

type decoderFactsData struct {
	Profiles         []decoderProfile  `json:"profiles"`
	KanaCompositions map[string]string `json:"kana_compositions"`
}

var decoderFacts = func() decoderFactsData {
	var facts decoderFactsData
	if err := json.Unmarshal(decoderProfileJSON, &facts); err != nil {
		panic("invalid embedded localization profiles: " + err.Error())
	}
	return facts
}()

// Decoder owns a private ROM copy. The caller cannot change the bytes after
// identification; supported profiles are immutable, generated encoding facts.
type Decoder struct {
	rom         []byte
	profile     decoderProfile
	dictionary  []byte
	speedSource *int
}

// NewDecoder accepts only an exact, headerless supported retail ROM. No title
// heuristic, region guess, patched image, or copier-header normalization can
// silently select a decoder with different native behavior.
func NewDecoder(rom []byte) (*Decoder, error) {
	if len(rom) != romSize {
		return nil, fmt.Errorf("expected 1 MiB headerless ROM, got %d bytes", len(rom))
	}
	digest := fmt.Sprintf("%x", sha256.Sum256(rom))
	for _, profile := range decoderFacts.Profiles {
		if profile.SHA256 == digest {
			return newDecoder(rom, profile)
		}
	}
	return nil, fmt.Errorf("unsupported ROM SHA-256 %s; an exact supported clean ROM is required", digest)
}

func newDecoder(rom []byte, profile decoderProfile) (*Decoder, error) {
	if profile.Encoding != "dictionary-12" && profile.Encoding != "direct-glyph" {
		return nil, fmt.Errorf("unknown text encoding %q", profile.Encoding)
	}
	if profile.FullEntrySeparator != "" && profile.FullEntrySeparator != " " {
		return nil, fmt.Errorf("invalid dictionary full-entry separator")
	}
	for _, source := range []*int{profile.NamePrefix, profile.PopulationSource} {
		if source != nil && (*source < 0 || *source >= len(rom)) {
			return nil, fmt.Errorf("contextual text source is outside ROM")
		}
	}
	d := &Decoder{rom: append([]byte(nil), rom...), profile: profile}
	if profile.Encoding == "dictionary-12" {
		if profile.Dictionary < 0 || profile.Dictionary > len(rom)-dictionaryBytes {
			return nil, fmt.Errorf("truncated dictionary")
		}
		d.dictionary = d.rom[profile.Dictionary : profile.Dictionary+dictionaryBytes]
	}
	if profile.SpeedSourceCall != nil {
		call := *profile.SpeedSourceCall
		offset, err := pc24Offset(call)
		if err != nil || offset < 3 || offset >= len(d.rom) || d.rom[offset-3] != 0xa0 {
			return nil, fmt.Errorf("message-speed source lacks verified adjacent LDY at $%06X", call)
		}
		pointer := word(d.rom[offset-2:offset]) + 6
		source, err := pc24Offset((call & 0xff0000) | pointer)
		if pointer > 0xffff || err != nil || source >= len(d.rom) {
			return nil, fmt.Errorf("message-speed source is outside ROM")
		}
		d.speedSource = &source
	}
	return d, nil
}

func (d *Decoder) ReleaseID() string { return d.profile.ID }

func pc24Offset(address int) (int, error) {
	if address < 0 || address > 0x7fffff || address&0xffff < 0x8000 {
		return 0, fmt.Errorf("invalid LoROM address $%06X", address)
	}
	return (address>>16)*0x8000 + (address & 0x7fff), nil
}

func cursorAddress(offset int) string {
	return fmt.Sprintf("$%02X:%04X", offset/0x8000, 0x8000+offset%0x8000)
}

func word(data []byte) int        { return int(data[0]) | int(data[1])<<8 }
func hexBytes(data []byte) string { return fmt.Sprintf("% X", data) }

func (d *Decoder) dictionaryEntry(token byte, consumer Consumer) ([]byte, error) {
	if consumer != Interactive && consumer != FixedComposer {
		return nil, fmt.Errorf("dictionary expansion requires a known consumer")
	}
	if d.dictionary == nil || token < 0x80 {
		return nil, fmt.Errorf("invalid dictionary token")
	}
	start := int(token&0x7f) * 12
	entry := d.dictionary[start : start+12]
	for i, code := range entry {
		if consumer == FixedComposer && code == 0 {
			return entry[:i], nil
		}
		if code == 0x20 {
			return entry[:i+1], nil
		}
	}
	if consumer == Interactive && d.profile.FullEntrySeparator != "" {
		result := make([]byte, 13)
		copy(result, entry)
		result[12] = 0x20
		return result, nil
	}
	return entry, nil
}

type recordWriter struct {
	decoder  *Decoder
	consumer Consumer
	start    int
	text     []string
	glyphs   []byte
	record   Record
}

func (w *recordWriter) flushText() {
	if len(w.text) != 0 {
		w.record.Operations = append(w.record.Operations, Operation{"op": "text", "value": strings.Join(w.text, "")})
		w.text = w.text[:0]
	}
}
func (w *recordWriter) flushGlyphs() {
	if len(w.glyphs) != 0 {
		w.record.Operations = append(w.record.Operations, Operation{"op": "native_glyphs", "codes": hexBytes(w.glyphs), "unicode": nil})
		w.glyphs = w.glyphs[:0]
	}
}
func (w *recordWriter) flush() { w.flushText(); w.flushGlyphs() }
func (w *recordWriter) operation(op Operation) {
	w.flush()
	w.record.Operations = append(w.record.Operations, op)
}

func (w *recordWriter) glyph(code byte) {
	profile := w.decoder.profile
	icon, isIcon := profile.Icons[code]
	if w.consumer == FixedComposer {
		for i := 0; i < 2; i++ {
			if profile.PopulationSource != nil && w.start == *profile.PopulationSource && code == profile.PopulationCodes[i] {
				icon, isIcon = iconPart{"status.population", i, 2}, true
			} else if w.decoder.speedSource != nil && w.start == *w.decoder.speedSource && code == profile.SpeedCodes[i] {
				icon, isIcon = iconPart{"ui.speed_direction", i, 2}, true
			}
		}
	}
	if isIcon {
		w.operation(Operation{"op": "insert_icon", "value": icon.Value, "native_code": fmt.Sprintf("%02X", code),
			"part_index": icon.PartIndex, "part_count": icon.PartCount, "confidence": "mapped_semantic"})
	} else if character, ok := profile.Glyphs[code]; ok {
		w.flushGlyphs()
		w.text = append(w.text, character)
	} else {
		w.flushText()
		w.glyphs = append(w.glyphs, code)
	}
}

func (w *recordWriter) diacritic(code byte) {
	combining, kind := "\u3099", "dakuten"
	if code == 0xdf {
		combining, kind = "\u309a", "handakuten"
	}
	if len(w.text) != 0 {
		last := len(w.text) - 1
		base := w.text[last]
		r, size := utf8.DecodeRuneInString(base)
		if size == len(base) && r >= 0x3040 && r <= 0x30ff {
			pair := base + combining
			if normalized, ok := decoderFacts.KanaCompositions[pair]; ok {
				pair = normalized
			}
			w.text[last] = pair
			return
		}
	}
	w.operation(Operation{"op": "native_diacritic", "code": fmt.Sprintf("%02X", code), "kind": kind, "unicode": nil})
}

func nullable(value string) any {
	if value == "" {
		return nil
	}
	return value
}

// DecodeRecord retains every decoded operation, native cursor, unresolved
// glyph, and dictionary token. Truncation is diagnostic IR, not success:
// callers publishing source packs must require Terminated and validate all
// operations. stopOnYield applies only to the interactive reader.
func (d *Decoder) DecodeRecord(consumer Consumer, start, limit int, stopOnYield bool) (Record, error) {
	if d == nil || (consumer != Interactive && consumer != FixedComposer) {
		return Record{}, fmt.Errorf("invalid decoder or consumer")
	}
	if start < 0 || limit < start || limit > len(d.rom) || limit-start > maxRecordBytes {
		return Record{}, fmt.Errorf("invalid or oversized record bounds %d..%d", start, limit)
	}
	w := recordWriter{decoder: d, consumer: consumer, start: start,
		record: Record{Operations: []Operation{}, DictionaryTokens: []string{}}}
	position := start
	for position < limit {
		code := d.rom[position]
		position++
		if code == 0 || code == 1 {
			op := Operation{"op": "end", "native_cursor_after": cursorAddress(position)}
			if code == 1 {
				if consumer == Interactive {
					op["op"] = "yield"
				} else {
					op["variant"] = "control_01"
				}
			}
			w.operation(op)
			if code == 0 || consumer == FixedComposer || stopOnYield {
				w.record.Terminated = true
				break
			}
			continue
		}
		if code == 6 {
			if consumer == Interactive && d.profile.NamePrefix != nil {
				prefix := *d.profile.NamePrefix
				end := min(prefix+32, len(d.rom))
				record, err := d.DecodeRecord(FixedComposer, prefix, end, true)
				if err != nil {
					return Record{}, fmt.Errorf("name prefix: %w", err)
				}
				for _, op := range record.Operations {
					if op["op"] == "text" {
						w.operation(op)
					} else if op["op"] != "end" {
						return Record{}, fmt.Errorf("non-text dialogue name prefix")
					}
				}
			}
			w.operation(Operation{"op": "insert_master_name"})
			if consumer == Interactive && d.profile.NameSeparator != "" {
				w.operation(Operation{"op": "text", "value": d.profile.NameSeparator})
			}
			continue
		}
		if code == 0x0d {
			w.operation(Operation{"op": "line_break"})
			continue
		}
		argumentCount := 0
		switch code {
		case 8:
			argumentCount = 4
		case 9:
			argumentCount = 3
		case 0x0b:
			argumentCount = 1
		case 7:
			if consumer == Interactive {
				argumentCount = 1
			}
		case 0x0a:
			if consumer == Interactive {
				argumentCount = 6
			}
		}
		if argumentCount != 0 {
			if argumentCount > limit-position {
				kind := "truncated_native_control"
				if consumer == FixedComposer {
					kind = "truncated_composer_control"
				}
				w.operation(Operation{"op": kind, "code": fmt.Sprintf("%02X", code), "available_args_hex": hexBytes(d.rom[position:limit])})
				position = limit
				break
			}
			args := d.rom[position : position+argumentCount]
			position += argumentCount
			switch code {
			case 8:
				encoded := hexBytes(args)
				semantic := d.profile.IndexedTextSemantics[encoded]
				confidence := "unresolved_semantic"
				if semantic != "" {
					confidence = "mapped_semantic"
				}
				w.operation(Operation{"op": "insert_indexed_text", "code": "08", "args_hex": encoded,
					"index_address": fmt.Sprintf("$%04X", word(args)), "pointer_table": fmt.Sprintf("$%04X", word(args[2:])), "value": nullable(semantic), "confidence": confidence})
			case 9:
				address := fmt.Sprintf("$%04X", word(args[1:]))
				semantic := d.profile.NumberSemantics[address]
				confidence := "mapped_address_only"
				if semantic != "" {
					confidence = "mapped_semantic"
				}
				w.operation(Operation{"op": "format_number", "width": int(args[0]), "native_address": address, "value": nullable(semantic), "confidence": confidence})
			case 0x0b:
				w.flushGlyphs()
				for i := 0; i < int(args[0]); i++ {
					w.text = append(w.text, " ")
				}
			default:
				confidence := "mapped_reserved"
				if code == 0x0a {
					confidence = "unresolved"
				}
				w.operation(Operation{"op": "native_control", "code": fmt.Sprintf("%02X", code), "args_hex": hexBytes(args), "confidence": confidence})
			}
			continue
		}
		if consumer == FixedComposer && code < 0x0c {
			w.operation(Operation{"op": "composer_noop_control", "code": fmt.Sprintf("%02X", code), "confidence": "mapped_reserved"})
			continue
		}
		if consumer == Interactive {
			switch code {
			case 2:
				w.operation(Operation{"op": "page_break", "confidence": "mapped"})
				continue
			case 3:
				w.operation(Operation{"op": "delay", "frames": 30})
				continue
			case 4:
				w.operation(Operation{"op": "toggle_text_state", "native_control": "04", "confidence": "mapped_presentation_only"})
				continue
			case 5:
				w.operation(Operation{"op": "reset_text_cursor", "confidence": "mapped"})
				continue
			}
		}
		if d.profile.Encoding == "dictionary-12" && code >= 0x80 {
			entry, err := d.dictionaryEntry(code, consumer)
			if err != nil {
				return Record{}, err
			}
			w.record.DictionaryTokens = append(w.record.DictionaryTokens, fmt.Sprintf("%02X", code))
			for _, expanded := range entry {
				w.glyph(expanded)
			}
		} else if d.profile.Encoding == "direct-glyph" && (code == 0xde || code == 0xdf) {
			w.diacritic(code)
		} else {
			w.glyph(code)
		}
	}
	w.flush()
	if consumer == FixedComposer && !w.record.Terminated {
		w.operation(Operation{"op": "truncated_composer_record", "native_cursor_after": cursorAddress(position)})
	}
	w.record.End = position
	return w.record, nil
}
