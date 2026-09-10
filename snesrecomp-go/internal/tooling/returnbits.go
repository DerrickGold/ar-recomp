package tooling

// Report-only known-bit domain. A mask bit says the corresponding value bit
// is known. Unknown bits are independent; the interval used by alias checks
// includes every represented value, not just aligned or observed samples.
// This domain deliberately does not preserve incoming-PC/status identities
// through arithmetic, and is enabled only by the stack-alias query.
func returnWordBits(w returnWord, width int) (mask, value uint16) {
	for b := range width {
		var m uint16
		switch w[b].kind {
		case returnValueConstant:
			m = 0xff
		case returnValueBits:
			m = uint16(w[b].lane)
		}
		mask |= m << (8 * b)
		value |= (w[b].value & m) << (8 * b)
	}
	return
}

func returnBitsWord(mask, value uint16) returnWord {
	var w returnWord
	for b := range 2 {
		m, v := byte(mask>>(8*b)), byte(value>>(8*b))
		switch m {
		case 0: // Canonical unknown: no stale data in a worklist state.
		case 0xff:
			w[b] = returnByte{kind: returnValueConstant, value: uint16(v)}
		default:
			w[b] = returnByte{kind: returnValueBits, lane: m, value: uint16(v & m)}
		}
	}
	return w
}

func returnWidthMask(width int) uint16 { return uint16(0xffff >> (16 - 8*width)) }

func returnLogicBits(op string, w returnWord, width int, immediate uint16) returnWord {
	mask, value := returnWordBits(w, width)
	switch op {
	case "AND":
		mask |= ^immediate & returnWidthMask(width)
		value &= immediate
	case "ORA":
		mask |= immediate & returnWidthMask(width)
		value |= immediate
	case "EOR":
		value ^= immediate
	default:
		return returnWord{}
	}
	return returnBitsWord(mask, value&mask)
}

func returnShiftBits(op string, w returnWord, width int, carry int8) (returnWord, int8) {
	mask, value := returnWordBits(w, width)
	top := uint16(1 << (8*width - 1))
	ejected, incoming := uint16(1), top
	var outMask, outValue uint16
	switch op {
	case "ASL", "ROL":
		ejected, incoming = top, 1
		outMask, outValue = mask<<1, value<<1
	case "LSR", "ROR":
		outMask, outValue = mask>>1, value>>1
	default:
		return returnWord{}, -1
	}
	if op == "ASL" || op == "LSR" {
		carry = 0 // logical shift inserts zero; rotate inserts the old carry
	}
	if carry >= 0 {
		outMask |= incoming
		if carry == 1 {
			outValue |= incoming
		}
	}
	outCarry := int8(-1)
	if mask&ejected != 0 {
		outCarry = 0
		if value&ejected != 0 {
			outCarry = 1
		}
	}
	return returnBitsWord(outMask&returnWidthMask(width), outValue), outCarry
}
