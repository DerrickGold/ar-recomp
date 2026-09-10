package localizationkit

import (
	"crypto/sha256"
	"fmt"
	"slices"
	"strings"
	"unicode"
)

// NativeMessage is local extraction evidence, not the author-facing language
// pack format. Source bytes, aliases and unresolved operations remain intact.
type NativeMessage struct {
	ID                   string             `json:"id"`
	CandidateSemanticIDs []string           `json:"candidate_semantic_ids"`
	AlignmentStatus      string             `json:"alignment_status"`
	Category             string             `json:"category,omitempty"`
	CategoryAliases      []string           `json:"category_aliases,omitempty"`
	Discovery            string             `json:"discovery,omitempty"`
	Classification       string             `json:"classification,omitempty"`
	Confidence           string             `json:"confidence,omitempty"`
	Owners               []string           `json:"owners,omitempty"`
	VisibleUnits         *int               `json:"visible_units,omitempty"`
	Source               NativeRecordSource `json:"source"`
	Terminated           bool               `json:"terminated"`
	Operations           []Operation        `json:"operations"`
	DictionaryTokens     []string           `json:"source_dictionary_tokens,omitempty"`
	VerifiedSemanticID   string             `json:"verified_semantic_id,omitempty"`
	SourceOwnership      string             `json:"source_ownership,omitempty"`
	SemanticRouteIDs     *[]string          `json:"semantic_route_ids,omitempty"`
	start, end           int
}
type NativeRecordSource struct {
	FileOffset string `json:"file_offset"`
	SNES       string `json:"snes"`
	End        string `json:"end_file_offset_exclusive"`
	ByteCount  int    `json:"byte_count"`
	SHA256     string `json:"raw_sha256"`
}
type NativePointerSlot struct {
	Slot                int     `json:"slot"`
	CandidateSemanticID string  `json:"candidate_semantic_id"`
	NativePointer       string  `json:"native_pointer"`
	TargetFileOffset    string  `json:"target_file_offset"`
	TargetID            *string `json:"target_id"`
	Classification      string  `json:"classification,omitempty"`
	target              int
}
type NativePointerTable struct {
	FileOffset string `json:"file_offset"`
	SNES       string `json:"snes"`
	Count      int    `json:"pointer_count"`
	offset     int
}
type NativePointerSet struct {
	ID          string              `json:"id"`
	SourceTable NativePointerTable  `json:"source_table"`
	Slots       []NativePointerSlot `json:"slots"`
}

func fileOffset(value int) string { return fmt.Sprintf("0x%06X", value) }
func (d *Decoder) nativeID(offset int) string {
	return fmt.Sprintf("native.%s.bank%02x.%04x", d.profile.ID, offset/0x8000, 0x8000+offset%0x8000)
}
func appendUnique(values []string, value string) []string {
	if !slices.Contains(values, value) {
		return append(values, value)
	}
	return values
}
func (d *Decoder) makeMessage(start, limit int, consumer Consumer) (*NativeMessage, error) {
	if start < 0 || start >= len(d.rom) {
		return nil, fmt.Errorf("catalogue source outside ROM: %d", start)
	}
	if limit < 0 {
		limit = min(len(d.rom), start+maxRecordBytes)
	}
	if limit <= start {
		return nil, fmt.Errorf("catalogue record has no readable source bytes")
	}
	record, err := d.DecodeRecord(consumer, start, limit, true)
	if err != nil {
		return nil, err
	}
	raw, err := d.span(start, record.End-start)
	if err != nil {
		return nil, err
	}
	return &NativeMessage{
		ID: d.nativeID(start), CandidateSemanticIDs: []string{},
		AlignmentStatus: "positional_unverified", Terminated: record.Terminated,
		Operations: record.Operations, DictionaryTokens: record.DictionaryTokens,
		Source: NativeRecordSource{fileOffset(start), cursorAddress(start), fileOffset(record.End), len(raw), fmt.Sprintf("%x", sha256.Sum256(raw))},
		start:  start, end: record.End,
	}, nil
}
func verifySemantic(message *NativeMessage, semanticID string) error {
	if message.VerifiedSemanticID != "" && message.VerifiedSemanticID != semanticID {
		return fmt.Errorf("%s: conflicting verified semantics %q and %q", message.ID, message.VerifiedSemanticID, semanticID)
	}
	message.VerifiedSemanticID, message.AlignmentStatus = semanticID, "callsite_verified"
	if !slices.Contains(message.CandidateSemanticIDs, semanticID) {
		message.CandidateSemanticIDs = append([]string{semanticID}, message.CandidateSemanticIDs...)
	}
	return nil
}

type catalogBuilder struct {
	d        *Decoder
	p        catalogProfile
	messages []*NativeMessage
	byOffset map[int]*NativeMessage
	pointers []*NativePointerSet
}

func newCatalogBuilder(d *Decoder, p catalogProfile) *catalogBuilder {
	return &catalogBuilder{d: d, p: p, messages: []*NativeMessage{}, byOffset: map[int]*NativeMessage{}, pointers: []*NativePointerSet{}}
}
func (b *catalogBuilder) add(start, limit int, category, semanticID, discovery string) (*NativeMessage, error) {
	if existing := b.byOffset[start]; existing != nil {
		existing.CandidateSemanticIDs = appendUnique(existing.CandidateSemanticIDs, semanticID)
		if existing.Category != category {
			existing.CategoryAliases = appendUnique(existing.CategoryAliases, category)
		}
		return existing, nil
	}
	message, err := b.d.makeMessage(start, limit, Interactive)
	if err != nil {
		return nil, err
	}
	message.Category, message.Discovery = category, discovery
	message.CandidateSemanticIDs = []string{semanticID}
	b.messages = append(b.messages, message)
	b.byOffset[start] = message
	return message, nil
}
func (b *catalogBuilder) sequential(start, end int, category, prefix string) ([]int, error) {
	if _, err := b.d.span(start, end-start); err != nil {
		return nil, err
	}
	starts := []int{}
	position := start
	for position < end {
		message, err := b.add(position, end, category, fmt.Sprintf("%s.%03d", prefix, len(starts)), "decoded_structure")
		if err != nil {
			return nil, err
		}
		if !message.Terminated || message.end <= position || message.end > end {
			return nil, fmt.Errorf("%s record at %s has no valid boundary before %s", category, cursorAddress(position), cursorAddress(end))
		}
		starts = append(starts, position)
		position = message.end
	}
	return starts, nil
}
func (b *catalogBuilder) pointerSet(table, count, allowedEnd int, category, prefix string) (*NativePointerSet, []int, error) {
	if count < 0 || count > 0x4000 {
		return nil, nil, fmt.Errorf("invalid catalogue pointer count")
	}
	data, err := b.d.span(table, count*2)
	if err != nil {
		return nil, nil, err
	}
	result := &NativePointerSet{ID: "native." + b.d.profile.ID + "." + prefix,
		SourceTable: NativePointerTable{fileOffset(table), cursorAddress(table), count, table}, Slots: []NativePointerSlot{}}
	ends := []int{}
	for index := 0; index < count; index++ {
		pointer := word(data[index*2:])
		if pointer < 0x8000 {
			return nil, nil, fmt.Errorf("non-ROM pointer %s at %s", localString(pointer), cursorAddress(table+index*2))
		}
		target := table/0x8000*0x8000 + pointer - 0x8000
		slot := NativePointerSlot{Slot: index, CandidateSemanticID: fmt.Sprintf("%s.%02d", prefix, index), NativePointer: localString(pointer), TargetFileOffset: fileOffset(target), target: target}
		if allowedEnd >= 0 && target >= allowedEnd {
			slot.Classification = "sentinel_or_next_table"
		} else {
			message, err := b.add(target, allowedEnd, category, slot.CandidateSemanticID, "decoded_structure")
			if err != nil {
				return nil, nil, err
			}
			targetID := message.ID
			slot.TargetID = &targetID
			ends = append(ends, message.end)
		}
		result.Slots = append(result.Slots, slot)
	}
	b.pointers = append(b.pointers, result)
	return result, ends, nil
}
func visibleUnits(operations []Operation) int {
	total := 0
	for _, op := range operations {
		switch op["op"] {
		case "text":
			for _, r := range op["value"].(string) {
				// Python str.isspace also treats these four C0 separators as
				// whitespace. Preserve that reference distinction exactly.
				if !unicode.IsSpace(r) && (r < 0x1c || r > 0x1f) {
					total++
				}
			}
		case "native_glyphs":
			total += len(strings.Fields(op["codes"].(string)))
		case "insert_icon":
			total++
		}
	}
	return total
}
func recordHasLanguage(record *NativeMessage) bool {
	if visibleUnits(record.Operations) > 0 {
		return true
	}
	for _, op := range record.Operations {
		if op["op"] == "insert_master_name" || op["op"] == "insert_indexed_text" || op["op"] == "format_number" {
			return true
		}
	}
	return false
}
