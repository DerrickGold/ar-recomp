package localization

import "unicode/utf8"

// This is the builder counterpart of ArUnicodeGrapheme_Next. Both use the
// game's generated Unicode property table and the same UAX #29 rules. Keyboard
// acceptance is also compared directly with the production C loader in tests.
type graphemeRange struct {
	first, last rune
	properties  uint8
}

const (
	gOther        = 1
	gCR           = 2
	gLF           = 3
	gControl      = 4
	gExtend       = 5
	gL            = 6
	gV            = 7
	gT            = 8
	gLV           = 9
	gLVT          = 10
	gRI           = 11
	gSpacing      = 12
	gPrepend      = 13
	gZWJ          = 14
	gPictographic = 19
)

func graphemeProperties(r rune) uint8 {
	low, high := 0, len(graphemeRanges)
	for low < high {
		mid := low + (high-low)/2
		v := graphemeRanges[mid]
		if r < v.first {
			high = mid
		} else if r > v.last {
			low = mid + 1
		} else {
			return v.properties
		}
	}
	return gOther
}

func graphemeControl(c uint8) bool { return c == gCR || c == gLF || c == gControl }

func graphemeBreak(previous, current uint8, regional int, emojiZWJ, indicLinker bool) bool {
	if previous == gCR && current == gLF {
		return false
	} // GB3
	if graphemeControl(previous) || graphemeControl(current) {
		return true
	} // GB4/5
	if previous == gL && (current == gL || current == gV || current == gLV || current == gLVT) {
		return false
	} // GB6
	if (previous == gLV || previous == gV) && (current == gV || current == gT) {
		return false
	} // GB7
	if (previous == gLVT || previous == gT) && current == gT {
		return false
	} // GB8
	if current == gExtend || current == gZWJ || current == gSpacing || previous == gPrepend {
		return false
	} // GB9–9b
	if indicLinker {
		return false
	} // GB9c
	if emojiZWJ && current == gPictographic {
		return false
	} // GB11
	if previous == gRI && current == gRI && regional%2 != 0 {
		return false
	} // GB12/13
	return true
}

func nextGrapheme(text string, offset int) (int, bool) {
	if offset < 0 || offset >= len(text) {
		return 0, false
	}
	r, size := utf8.DecodeRuneInString(text[offset:])
	if r == utf8.RuneError && size == 1 {
		return 0, false
	}
	properties := graphemeProperties(r)
	previous, regional, emoji, indic := properties&31, 0, 0, 0
	if previous == gRI {
		regional = 1
	}
	if previous == gPictographic {
		emoji = 1
	}
	if properties>>5 == 2 {
		indic = 1
	}
	cursor := offset + size
	for cursor < len(text) {
		r, size = utf8.DecodeRuneInString(text[cursor:])
		if r == utf8.RuneError && size == 1 {
			return 0, false
		}
		properties = graphemeProperties(r)
		current, currentIndic := properties&31, properties>>5
		if graphemeBreak(previous, current, regional, emoji == 2, indic == 2 && currentIndic == 2) {
			break
		}
		if current == gRI {
			regional++
		} else {
			regional = 0
		}
		switch {
		case current == gPictographic:
			emoji = 1
		case current == gExtend && emoji == 1:
		case current == gZWJ && emoji == 1:
			emoji = 2
		default:
			emoji = 0
		}
		switch {
		case currentIndic == 2:
			indic = 1
		case currentIndic == 1 && indic != 0:
			indic = 2
		case currentIndic != 3:
			indic = 0
		}
		previous = current
		cursor += size
	}
	return cursor, true
}
