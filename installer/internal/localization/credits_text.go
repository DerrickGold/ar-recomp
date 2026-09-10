package localization

import (
	"encoding/binary"
	"fmt"
	"strings"
	"unicode"
	"unicode/utf8"
)

// Credits are tile compositions, not dialogue bytecodes. These alphabet
// mappings contain no extracted lettering bitmaps or staff-list transcriptions.
// The identified release supplies every page. Unknown compositions fail closed.
type creditGlyph struct {
	text        string
	top, bottom []uint16
}

func creditsAlphabet(profile string) []creditGlyph {
	var out []creditGlyph
	add := func(text string, top ...uint16) {
		bottom := make([]uint16, len(top))
		for i, v := range top {
			bottom[i] = v + 0x10
		}
		out = append(out, creditGlyph{text, top, bottom})
	}
	for i := 0; i < 26; i++ {
		tile := uint16(0x20 + i/8*0x20 + i%8*2)
		add(string(rune('A'+i)), tile, tile+1)
	}
	for i := 0; i < 12; i++ {
		add(string(rune('a'+i)), uint16(0x84+i))
	}
	add("m", 0xa0, 0xa1)
	for i, c := range "nopqrstuv" {
		add(string(c), uint16(0xa2+i))
	}
	add("w", 0xab, 0xac)
	for i, c := range "xyz" {
		add(string(c), uint16(0xad+i))
	}
	add("ō", 2)
	add("ū", 3)
	out = append(out, creditGlyph{"ō", []uint16{2}, []uint16{0xb3}})
	add("I", 0x41)
	add("J", 0x43)
	add("mer", 0xc0, 0xc1, 0xc2, 0xc3)
	add("- ", 0xc4, 0xc5)
	out = append(out, creditGlyph{" -", []uint16{0xc6, 0xc7}, []uint16{0xd6, 0x10}})
	add("ENIX", 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf)
	add("!", 0xef)
	// I/J have blank left halves. US/JP use repeated J-left tiles as
	// padding before a single J-right, not a run of the letter J.
	out = append(out, creditGlyph{" ", []uint16{0x10}, []uint16{0x10}},
		creditGlyph{" ", []uint16{0x42}, []uint16{0x52}})
	if profile == "fr" {
		for i := range out {
			if out[i].text == "x" {
				out[i].text, out[i].bottom[0] = "é", 0x98
			}
			if out[i].text == "- " {
				out[i].bottom[0] = 0x10
			}
		}
		out = append(out, creditGlyph{"'", []uint16{0x1b}, []uint16{0x10}},
			creditGlyph{"ê", []uint16{0xd4}, []uint16{0x98}})
	}
	return out
}

// NativeCreditsPage is extraction evidence and editable, fixed-page source.
// Copyright/logo pages are deliberately artwork-only, even if their tiles
// happen to spell readable words. All twenty remain in the graphics archive.
type NativeCreditsPage struct {
	Page        int      `json:"page"`
	ID          string   `json:"semantic_id,omitempty"`
	Lines       []string `json:"lines,omitempty"`
	Rows        []int    `json:"native_rows,omitempty"`
	Accent      bool     `json:"initial_accent"`
	ArtworkOnly bool     `json:"artwork_only"`
}

func creditPageID(profile string, page int) string {
	if page < 15 {
		return fmt.Sprintf("credits.page_%02d", page)
	}
	if profile == "jp" && page == 16 || profile != "jp" && page == 17 {
		return "credits.the_end"
	}
	if profile == "jp" && page == 17 {
		return "credits.special_mode"
	}
	if page == 18 {
		return "credits.best_player"
	}
	if page == 19 {
		return "credits.game_over"
	}
	return ""
}

func decodeCreditPages(profile string, pages []byte) ([]NativeCreditsPage, error) {
	if len(pages) != endingPageCount*endingPageBytes {
		return nil, fmt.Errorf("invalid credits page extent")
	}
	alphabet := creditsAlphabet(profile)
	var result []NativeCreditsPage
	for index := 0; index < endingPageCount; index++ {
		page := NativeCreditsPage{Page: index, ID: creditPageID(profile, index)}
		page.ArtworkOnly = page.ID == ""
		if page.ArtworkOnly {
			result = append(result, page)
			continue
		}
		words := make([]uint16, 1024)
		for i := range words {
			words[i] = binary.LittleEndian.Uint16(pages[index*endingPageBytes+i*2:])
		}
		for row := 0; row < 32; row++ {
			blank := true
			for _, v := range words[row*32 : (row+1)*32] {
				blank = blank && v == 0x10
			}
			if blank {
				continue
			}
			if row >= 27 {
				return nil, fmt.Errorf("credits page %d: text outside visible rows", index)
			}
			var text strings.Builder
			for col := 0; col < 32; {
				found := false
				for _, glyph := range alphabet {
					if len(glyph.top) > 32-col {
						continue
					}
					matches := true
					for k := range glyph.top {
						top, bottom := words[row*32+col+k], words[(row+1)*32+col+k]
						if top & ^uint16(0x400) != glyph.top[k] || bottom & ^uint16(0x400) != glyph.bottom[k] || top&0x400 != bottom&0x400 {
							matches = false
							break
						}
					}
					if !matches {
						continue
					}
					// Capital initials carry built-in side bearings. Restore
					// lexical spaces at lower→upper and packed ENIX boundaries.
					previous, _ := utf8.DecodeLastRuneInString(text.String())
					first, _ := utf8.DecodeRuneInString(glyph.text)
					if unicode.IsUpper(first) && (unicode.IsLower(previous) || strings.HasSuffix(text.String(), "ENIX")) {
						text.WriteByte(' ')
					}
					text.WriteString(glyph.text)
					page.Accent = page.Accent || words[row*32+col]&0x400 != 0
					col += len(glyph.top)
					found = true
					break
				}
				if !found {
					return nil, fmt.Errorf("credits page %d row %d column %d: unknown tile composition %04x/%04x", index, row, col, words[row*32+col], words[(row+1)*32+col])
				}
			}
			page.Lines = append(page.Lines, strings.TrimSpace(text.String()))
			page.Rows = append(page.Rows, row)
			row++
		}
		if len(page.Lines) == 0 || len(page.Lines) > 6 {
			return nil, fmt.Errorf("credits page %d: unsupported line count", index)
		}
		result = append(result, page)
	}
	return result, nil
}

func (d *Decoder) nativeCredits(entries []assetEntry) ([]NativeCreditsPage, error) {
	assets, err := d.endingAssets(entries)
	if err != nil {
		return nil, err
	}
	return decodeCreditPages(d.profile.ID, assets.pages)
}

func nativeCreditsMessages(pages []NativeCreditsPage) []AuthorMessage {
	var messages []AuthorMessage
	for _, page := range pages {
		if page.ArtworkOnly {
			continue
		}
		message := AuthorMessage{ID: page.ID}
		for i, text := range page.Lines {
			if i != 0 {
				message.Operations = append(message.Operations, AuthorOperation{Op: "line"})
			}
			message.Operations = append(message.Operations, AuthorOperation{Op: "text", Value: text})
		}
		message.Operations = append(message.Operations, AuthorOperation{Op: "end"})
		messages = append(messages, message)
	}
	return messages
}
