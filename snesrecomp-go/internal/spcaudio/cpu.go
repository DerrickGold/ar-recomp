package spcaudio

const (
	flagC byte = 1 << 0
	flagZ byte = 1 << 1
	flagI byte = 1 << 2
	flagH byte = 1 << 3
	flagB byte = 1 << 4
	flagP byte = 1 << 5
	flagV byte = 1 << 6
	flagN byte = 1 << 7
)

type cpu struct {
	apu     *apu
	a, x, y byte
	sp, psw byte
	pc      uint16
	cycles  uint64
	stopped bool
}

var opcodeCycles = [256]byte{
	2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 8,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4, 6,
	2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5, 4,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3, 8,
	2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6, 6,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 4, 3,
	2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 5,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 6,
	2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4, 5,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12, 5,
	3, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4, 4,
	2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 4,
	3, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4, 9,
	2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6, 3,
	2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4, 3,
	2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4, 3,
}

func (c *cpu) fetch() byte {
	value := c.apu.read(c.pc)
	c.pc++
	return value
}

func (c *cpu) fetchWord() uint16 {
	low := uint16(c.fetch())
	return low | uint16(c.fetch())<<8
}

func (c *cpu) direct(value byte) uint16 {
	address := uint16(value)
	if c.psw&flagP != 0 {
		address |= 0x100
	}
	return address
}

func (c *cpu) directNext(value byte) uint16 { return c.direct(value + 1) }

func (c *cpu) directPointer(value byte) uint16 {
	low := uint16(c.apu.read(c.direct(value)))
	high := uint16(c.apu.read(c.directNext(value)))
	return low | high<<8
}

func (c *cpu) push(value byte) {
	c.apu.write(0x100|uint16(c.sp), value)
	c.sp--
}

func (c *cpu) pop() byte {
	c.sp++
	return c.apu.read(0x100 | uint16(c.sp))
}

func (c *cpu) pushPC() {
	c.push(byte(c.pc >> 8))
	c.push(byte(c.pc))
}

func (c *cpu) popPC() { c.pc = uint16(c.pop()) | uint16(c.pop())<<8 }

func (c *cpu) setFlag(flag byte, enabled bool) {
	if enabled {
		c.psw |= flag
	} else {
		c.psw &^= flag
	}
}

func (c *cpu) nz(value byte) byte {
	c.setFlag(flagZ, value == 0)
	c.setFlag(flagN, value&0x80 != 0)
	return value
}

func (c *cpu) nzw(value uint16) uint16 {
	c.setFlag(flagZ, value == 0)
	c.setFlag(flagN, value&0x8000 != 0)
	return value
}

func (c *cpu) carry() int {
	if c.psw&flagC != 0 {
		return 1
	}
	return 0
}

func (c *cpu) adc(left, right byte) byte {
	carry := c.carry()
	result := int(left) + int(right) + carry
	c.setFlag(flagC, result > 0xff)
	c.setFlag(flagH, int(left&0x0f)+int(right&0x0f)+carry > 0x0f)
	c.setFlag(flagV, (^(left^right)&(left^byte(result))&0x80) != 0)
	return c.nz(byte(result))
}

func (c *cpu) sbc(left, right byte) byte {
	borrow := 1 - c.carry()
	result := int(left) - int(right) - borrow
	c.setFlag(flagC, result >= 0)
	c.setFlag(flagH, int(left&0x0f)-int(right&0x0f)-borrow >= 0)
	c.setFlag(flagV, ((left^right)&(left^byte(result))&0x80) != 0)
	return c.nz(byte(result))
}

func (c *cpu) compare(left, right byte) {
	result := int(left) - int(right)
	c.setFlag(flagC, result >= 0)
	c.nz(byte(result))
}

// alu applies one of the six regular arithmetic/logic rows. Operation numbers
// follow the opcode matrix: OR, AND, EOR, CMP, ADC, SBC.
func (c *cpu) alu(operation int, left, right byte) byte {
	switch operation {
	case 0:
		return c.nz(left | right)
	case 1:
		return c.nz(left & right)
	case 2:
		return c.nz(left ^ right)
	case 3:
		c.compare(left, right)
		return left
	case 4:
		return c.adc(left, right)
	default:
		return c.sbc(left, right)
	}
}

func (c *cpu) branch(condition bool) int {
	offset := int8(c.fetch())
	if !condition {
		return 0
	}
	c.pc = uint16(int32(c.pc) + int32(offset))
	return 2
}

func (c *cpu) memoryBit() (uint16, byte) {
	operand := c.fetchWord()
	return operand & 0x1fff, byte(1 << (operand >> 13))
}

func (c *cpu) shift(address uint16, kind byte) {
	value := c.apu.read(address)
	var result byte
	switch kind {
	case 0: // ASL
		c.setFlag(flagC, value&0x80 != 0)
		result = value << 1
	case 1: // ROL
		carry := byte(c.carry())
		c.setFlag(flagC, value&0x80 != 0)
		result = value<<1 | carry
	case 2: // LSR
		c.setFlag(flagC, value&1 != 0)
		result = value >> 1
	default: // ROR
		carry := byte(c.carry()) << 7
		c.setFlag(flagC, value&1 != 0)
		result = value>>1 | carry
	}
	c.apu.write(address, c.nz(result))
}

func (c *cpu) stepRegularALU(opcode byte) bool {
	operation := int(opcode >> 5)
	if operation > 5 {
		return false
	}
	mode := opcode & 0x1f
	var address uint16
	var right byte
	switch mode {
	case 0x04:
		address = c.direct(c.fetch())
		right = c.apu.read(address)
	case 0x05:
		address = c.fetchWord()
		right = c.apu.read(address)
	case 0x06:
		right = c.apu.read(c.direct(c.x))
	case 0x07:
		address = c.directPointer(c.fetch() + c.x)
		right = c.apu.read(address)
	case 0x08:
		right = c.fetch()
	case 0x14:
		address = c.direct(c.fetch() + c.x)
		right = c.apu.read(address)
	case 0x15:
		address = c.fetchWord() + uint16(c.x)
		right = c.apu.read(address)
	case 0x16:
		address = c.fetchWord() + uint16(c.y)
		right = c.apu.read(address)
	case 0x17:
		address = c.directPointer(c.fetch()) + uint16(c.y)
		right = c.apu.read(address)
	case 0x09:
		source := c.apu.read(c.direct(c.fetch()))
		destination := c.direct(c.fetch())
		value := c.alu(operation, c.apu.read(destination), source)
		if operation != 3 {
			c.apu.write(destination, value)
		}
		return true
	case 0x18:
		immediate := c.fetch()
		destination := c.direct(c.fetch())
		value := c.alu(operation, c.apu.read(destination), immediate)
		if operation != 3 {
			c.apu.write(destination, value)
		}
		return true
	case 0x19:
		destination := c.direct(c.x)
		value := c.alu(operation, c.apu.read(destination), c.apu.read(c.direct(c.y)))
		if operation != 3 {
			c.apu.write(destination, value)
		}
		return true
	default:
		return false
	}
	c.a = c.alu(operation, c.a, right)
	return true
}

func (c *cpu) step() int {
	opcode := c.fetch()
	extra := 0
	if c.stepRegularALU(opcode) {
		cycles := int(opcodeCycles[opcode])
		c.cycles += uint64(cycles)
		return cycles
	}

	switch opcode {
	case 0x00: // NOP
	case 0x01, 0x11, 0x21, 0x31, 0x41, 0x51, 0x61, 0x71,
		0x81, 0x91, 0xa1, 0xb1, 0xc1, 0xd1, 0xe1, 0xf1: // TCALL n
		index := uint16(opcode >> 4)
		c.pushPC()
		vector := uint16(0xffde) - index*2
		c.pc = uint16(c.apu.read(vector)) | uint16(c.apu.read(vector+1))<<8
	case 0x02, 0x22, 0x42, 0x62, 0x82, 0xa2, 0xc2, 0xe2: // SET1
		address := c.direct(c.fetch())
		c.apu.write(address, c.apu.read(address)|byte(1<<(opcode>>5)))
	case 0x12, 0x32, 0x52, 0x72, 0x92, 0xb2, 0xd2, 0xf2: // CLR1
		address := c.direct(c.fetch())
		c.apu.write(address, c.apu.read(address)&^byte(1<<(opcode>>5)))
	case 0x03, 0x23, 0x43, 0x63, 0x83, 0xa3, 0xc3, 0xe3: // BBS
		value := c.apu.read(c.direct(c.fetch()))
		extra = c.branch(value&byte(1<<(opcode>>5)) != 0)
	case 0x13, 0x33, 0x53, 0x73, 0x93, 0xb3, 0xd3, 0xf3: // BBC
		value := c.apu.read(c.direct(c.fetch()))
		extra = c.branch(value&byte(1<<(opcode>>5)) == 0)
	case 0x0a: // OR1 C,bit
		address, bit := c.memoryBit()
		c.setFlag(flagC, c.psw&flagC != 0 || c.apu.read(address)&bit != 0)
	case 0x2a: // OR1 C,/bit
		address, bit := c.memoryBit()
		c.setFlag(flagC, c.psw&flagC != 0 || c.apu.read(address)&bit == 0)
	case 0x4a: // AND1 C,bit
		address, bit := c.memoryBit()
		c.setFlag(flagC, c.psw&flagC != 0 && c.apu.read(address)&bit != 0)
	case 0x6a: // AND1 C,/bit
		address, bit := c.memoryBit()
		c.setFlag(flagC, c.psw&flagC != 0 && c.apu.read(address)&bit == 0)
	case 0x8a: // EOR1 C,bit
		address, bit := c.memoryBit()
		c.setFlag(flagC, (c.psw&flagC != 0) != (c.apu.read(address)&bit != 0))
	case 0xaa: // MOV1 C,bit
		address, bit := c.memoryBit()
		c.setFlag(flagC, c.apu.read(address)&bit != 0)
	case 0xca: // MOV1 bit,C
		address, bit := c.memoryBit()
		value := c.apu.read(address)
		if c.psw&flagC != 0 {
			value |= bit
		} else {
			value &^= bit
		}
		c.apu.write(address, value)
	case 0xea: // NOT1 bit
		address, bit := c.memoryBit()
		c.apu.write(address, c.apu.read(address)^bit)
	case 0x0b, 0x2b, 0x4b, 0x6b: // shift dp
		c.shift(c.direct(c.fetch()), opcode>>5)
	case 0x0c, 0x2c, 0x4c, 0x6c: // shift abs
		c.shift(c.fetchWord(), opcode>>5)
	case 0x1b, 0x3b, 0x5b, 0x7b: // shift dp+X
		c.shift(c.direct(c.fetch()+c.x), opcode>>5)
	case 0x1c, 0x3c, 0x5c, 0x7c: // shift A
		value := c.a
		var result byte
		switch opcode >> 5 {
		case 0:
			c.setFlag(flagC, value&0x80 != 0)
			result = value << 1
		case 1:
			carry := byte(c.carry())
			c.setFlag(flagC, value&0x80 != 0)
			result = value<<1 | carry
		case 2:
			c.setFlag(flagC, value&1 != 0)
			result = value >> 1
		default:
			carry := byte(c.carry()) << 7
			c.setFlag(flagC, value&1 != 0)
			result = value>>1 | carry
		}
		c.a = c.nz(result)
	case 0x0d:
		c.push(c.psw)
	case 0x2d:
		c.push(c.a)
	case 0x4d:
		c.push(c.x)
	case 0x6d:
		c.push(c.y)
	case 0x8e:
		c.psw = c.pop()
	case 0xae:
		c.a = c.pop()
	case 0xce:
		c.x = c.pop()
	case 0xee:
		c.y = c.pop()
	case 0x0e, 0x4e: // TSET1/TCLR1
		address := c.fetchWord()
		value := c.apu.read(address)
		c.nz(c.a - value)
		if opcode == 0x0e {
			value |= c.a
		} else {
			value &^= c.a
		}
		c.apu.write(address, value)
	case 0x0f: // BRK
		c.pushPC()
		c.push(c.psw | flagB)
		c.psw |= flagB | flagI
		c.pc = uint16(c.apu.read(0xffde)) | uint16(c.apu.read(0xffdf))<<8
	case 0x10:
		extra = c.branch(c.psw&flagN == 0)
	case 0x30:
		extra = c.branch(c.psw&flagN != 0)
	case 0x50:
		extra = c.branch(c.psw&flagV == 0)
	case 0x70:
		extra = c.branch(c.psw&flagV != 0)
	case 0x90:
		extra = c.branch(c.psw&flagC == 0)
	case 0xb0:
		extra = c.branch(c.psw&flagC != 0)
	case 0xd0:
		extra = c.branch(c.psw&flagZ == 0)
	case 0xf0:
		extra = c.branch(c.psw&flagZ != 0)
	case 0x1a, 0x3a: // DECW/INCW
		dp := c.fetch()
		value := uint16(c.apu.read(c.direct(dp))) | uint16(c.apu.read(c.directNext(dp)))<<8
		if opcode == 0x1a {
			value--
		} else {
			value++
		}
		c.apu.write(c.direct(dp), byte(value))
		c.apu.write(c.directNext(dp), byte(value>>8))
		c.nzw(value)
	case 0x1d:
		c.x = c.nz(c.x - 1)
	case 0x3d:
		c.x = c.nz(c.x + 1)
	case 0xdc:
		c.y = c.nz(c.y - 1)
	case 0xfc:
		c.y = c.nz(c.y + 1)
	case 0x9c:
		c.a = c.nz(c.a - 1)
	case 0xbc:
		c.a = c.nz(c.a + 1)
	case 0x8b, 0xab: // DEC/INC dp
		address := c.direct(c.fetch())
		value := c.apu.read(address)
		if opcode == 0x8b {
			value--
		} else {
			value++
		}
		c.apu.write(address, c.nz(value))
	case 0x8c, 0xac: // DEC/INC abs
		address := c.fetchWord()
		value := c.apu.read(address)
		if opcode == 0x8c {
			value--
		} else {
			value++
		}
		c.apu.write(address, c.nz(value))
	case 0x9b, 0xbb: // DEC/INC dp+X
		address := c.direct(c.fetch() + c.x)
		value := c.apu.read(address)
		if opcode == 0x9b {
			value--
		} else {
			value++
		}
		c.apu.write(address, c.nz(value))
	case 0x1e:
		c.compare(c.x, c.apu.read(c.fetchWord()))
	case 0x3e:
		c.compare(c.x, c.apu.read(c.direct(c.fetch())))
	case 0x5e:
		c.compare(c.y, c.apu.read(c.fetchWord()))
	case 0x7e:
		c.compare(c.y, c.apu.read(c.direct(c.fetch())))
	case 0xc8:
		c.compare(c.x, c.fetch())
	case 0xad:
		c.compare(c.y, c.fetch())
	case 0x1f: // JMP [abs+X]
		address := c.fetchWord() + uint16(c.x)
		c.pc = uint16(c.apu.read(address)) | uint16(c.apu.read(address+1))<<8
	case 0x2f:
		extra = c.branch(true)
	case 0x3f:
		address := c.fetchWord()
		c.pushPC()
		c.pc = address
	case 0x4f:
		address := uint16(0xff00) | uint16(c.fetch())
		c.pushPC()
		c.pc = address
	case 0x5f:
		c.pc = c.fetchWord()
	case 0x6f:
		c.popPC()
	case 0x7f:
		c.psw = c.pop()
		c.popPC()
	case 0x20:
		c.psw &^= flagP
	case 0x40:
		c.psw |= flagP
	case 0x60:
		c.psw &^= flagC
	case 0x80:
		c.psw |= flagC
	case 0xa0:
		c.psw |= flagI
	case 0xc0:
		c.psw &^= flagI
	case 0xe0:
		c.psw &^= flagV | flagH
	case 0xed:
		c.psw ^= flagC
	case 0x2e, 0xde: // CBNE dp[,X],rel
		dp := c.fetch()
		address := c.direct(dp)
		if opcode == 0xde {
			address = c.direct(dp + c.x)
		}
		extra = c.branch(c.a != c.apu.read(address))
	case 0x6e: // DBNZ dp,rel
		address := c.direct(c.fetch())
		value := c.apu.read(address) - 1
		c.apu.write(address, value)
		extra = c.branch(value != 0)
	case 0xfe: // DBNZ Y,rel
		c.y--
		extra = c.branch(c.y != 0)
	case 0x5a, 0x7a, 0x9a, 0xba: // CMPW/ADDW/SUBW/MOVW
		dp := c.fetch()
		right := uint16(c.apu.read(c.direct(dp))) | uint16(c.apu.read(c.directNext(dp)))<<8
		left := uint16(c.a) | uint16(c.y)<<8
		switch opcode {
		case 0x5a:
			result := int(left) - int(right)
			c.setFlag(flagC, result >= 0)
			c.nzw(uint16(result))
		case 0x7a:
			result := uint32(left) + uint32(right)
			c.setFlag(flagC, result > 0xffff)
			c.setFlag(flagH, int(left&0x0fff)+int(right&0x0fff) > 0x0fff)
			c.setFlag(flagV, (^(left^right)&(left^uint16(result))&0x8000) != 0)
			value := c.nzw(uint16(result))
			c.a = byte(value)
			c.y = byte(value >> 8)
		case 0x9a:
			result := int32(left) - int32(right)
			c.setFlag(flagC, result >= 0)
			c.setFlag(flagH, int(left&0x0fff)-int(right&0x0fff) >= 0)
			c.setFlag(flagV, ((left^right)&(left^uint16(result))&0x8000) != 0)
			value := c.nzw(uint16(result))
			c.a = byte(value)
			c.y = byte(value >> 8)
		case 0xba:
			c.a = byte(right)
			c.y = byte(right >> 8)
			c.nzw(right)
		}
	case 0xda: // MOVW dp,YA
		dp := c.fetch()
		c.apu.write(c.direct(dp), c.a)
		c.apu.write(c.directNext(dp), c.y)
	case 0x5d:
		c.x = c.nz(c.a)
	case 0x7d:
		c.a = c.nz(c.x)
	case 0x9d:
		c.x = c.nz(c.sp)
	case 0xbd:
		c.sp = c.x
	case 0xdd:
		c.a = c.nz(c.y)
	case 0xfd:
		c.y = c.nz(c.a)
	case 0x8d:
		c.y = c.nz(c.fetch())
	case 0xcd:
		c.x = c.nz(c.fetch())
	case 0xe8:
		c.a = c.nz(c.fetch())
	case 0xc4:
		c.apu.write(c.direct(c.fetch()), c.a)
	case 0xc5:
		c.apu.write(c.fetchWord(), c.a)
	case 0xc6:
		c.apu.write(c.direct(c.x), c.a)
	case 0xc7:
		c.apu.write(c.directPointer(c.fetch()+c.x), c.a)
	case 0xd4:
		c.apu.write(c.direct(c.fetch()+c.x), c.a)
	case 0xd5:
		c.apu.write(c.fetchWord()+uint16(c.x), c.a)
	case 0xd6:
		c.apu.write(c.fetchWord()+uint16(c.y), c.a)
	case 0xd7:
		c.apu.write(c.directPointer(c.fetch())+uint16(c.y), c.a)
	case 0xd8:
		c.apu.write(c.direct(c.fetch()), c.x)
	case 0xd9:
		c.apu.write(c.direct(c.fetch()+c.y), c.x)
	case 0xcb:
		c.apu.write(c.direct(c.fetch()), c.y)
	case 0xdb:
		c.apu.write(c.direct(c.fetch()+c.x), c.y)
	case 0xc9:
		c.apu.write(c.fetchWord(), c.x)
	case 0xcc:
		c.apu.write(c.fetchWord(), c.y)
	case 0xe4:
		c.a = c.nz(c.apu.read(c.direct(c.fetch())))
	case 0xe5:
		c.a = c.nz(c.apu.read(c.fetchWord()))
	case 0xe6:
		c.a = c.nz(c.apu.read(c.direct(c.x)))
	case 0xe7:
		c.a = c.nz(c.apu.read(c.directPointer(c.fetch() + c.x)))
	case 0xf4:
		c.a = c.nz(c.apu.read(c.direct(c.fetch() + c.x)))
	case 0xf5:
		c.a = c.nz(c.apu.read(c.fetchWord() + uint16(c.x)))
	case 0xf6:
		c.a = c.nz(c.apu.read(c.fetchWord() + uint16(c.y)))
	case 0xf7:
		c.a = c.nz(c.apu.read(c.directPointer(c.fetch()) + uint16(c.y)))
	case 0xe9:
		c.x = c.nz(c.apu.read(c.fetchWord()))
	case 0xf8:
		c.x = c.nz(c.apu.read(c.direct(c.fetch())))
	case 0xf9:
		c.x = c.nz(c.apu.read(c.direct(c.fetch() + c.y)))
	case 0xeb:
		c.y = c.nz(c.apu.read(c.direct(c.fetch())))
	case 0xec:
		c.y = c.nz(c.apu.read(c.fetchWord()))
	case 0xfb:
		c.y = c.nz(c.apu.read(c.direct(c.fetch() + c.x)))
	case 0x8f: // MOV dp,#imm
		value := c.fetch()
		c.apu.write(c.direct(c.fetch()), value)
	case 0xfa: // MOV dp,dp
		value := c.apu.read(c.direct(c.fetch()))
		c.apu.write(c.direct(c.fetch()), value)
	case 0xaf: // MOV (X)+,A
		c.apu.write(c.direct(c.x), c.a)
		c.x++
	case 0xbf: // MOV A,(X)+
		c.a = c.nz(c.apu.read(c.direct(c.x)))
		c.x++
	case 0x9e: // DIV YA,X
		ya := uint16(c.a) | uint16(c.y)<<8
		divisor := uint16(c.x)
		c.setFlag(flagH, c.y&0x0f >= c.x&0x0f)
		c.setFlag(flagV, uint16(c.y) >= divisor)
		if divisor == 0 {
			c.a = 0xff
			c.y = byte(ya)
		} else if uint16(c.y) < divisor*2 {
			c.a = byte(ya / divisor)
			c.y = byte(ya % divisor)
		} else {
			denominator := uint16(256) - divisor
			numerator := ya - divisor*256
			c.a = byte(255 - uint16(numerator)/denominator)
			c.y = byte(divisor + uint16(numerator)%denominator)
		}
		c.nz(c.a)
	case 0x9f:
		c.a = c.nz(c.a<<4 | c.a>>4)
	case 0xbe: // DAS
		if c.psw&flagC == 0 || c.a > 0x99 {
			c.a -= 0x60
			c.psw &^= flagC
		}
		if c.psw&flagH == 0 || c.a&0x0f > 9 {
			c.a -= 6
		}
		c.nz(c.a)
	case 0xdf: // DAA
		if c.psw&flagC != 0 || c.a > 0x99 {
			c.a += 0x60
			c.psw |= flagC
		}
		if c.psw&flagH != 0 || c.a&0x0f > 9 {
			c.a += 6
		}
		c.nz(c.a)
	case 0xcf:
		product := uint16(c.a) * uint16(c.y)
		c.a = byte(product)
		c.y = byte(product >> 8)
		c.nz(c.y)
	case 0xef: // SLEEP
		c.stopped = true
	case 0xff: // STOP
		c.stopped = true
	}
	cycles := int(opcodeCycles[opcode]) + extra
	c.cycles += uint64(cycles)
	return cycles
}
