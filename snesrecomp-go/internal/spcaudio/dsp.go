package spcaudio

const (
	envelopeRelease = iota
	envelopeAttack
	envelopeDecay
	envelopeSustain
	envelopeGain
)

// S-DSP envelope/noise rates are selected from a shared 32-entry counter.
// Values are output-sample periods; rate zero is disabled.
var ratePeriods = [32]int{
	0, 2048, 1536, 1280, 1024, 768, 640, 512,
	384, 320, 256, 192, 160, 128, 96, 80,
	64, 48, 40, 32, 24, 20, 16, 12,
	10, 8, 6, 5, 4, 3, 2, 1,
}

type dspVoice struct {
	active       bool
	release      bool
	block        uint16
	loop         uint16
	header       byte
	decoded      [16]int
	index        int
	phase        uint32
	previous1    int
	previous2    int
	envelope     int
	envelopeAt   int
	envelopeMode int
	lastOutput   int
}

type dsp struct {
	registers [128]byte
	voices    [8]dspVoice
	keyOn     byte
	keyOff    byte
	noise     uint16
	noiseAt   int
	echoAt    uint16
	firAt     int
	firL      [8]int
	firR      [8]int
}

func (d *dsp) read(address byte) byte {
	return d.registers[address&0x7f]
}

func (d *dsp) write(address, value byte) {
	address &= 0x7f
	switch address {
	case 0x4c:
		d.keyOn |= value
	case 0x5c:
		d.keyOff |= value
	case 0x7c:
		d.registers[0x7c] = 0
	default:
		d.registers[address] = value
	}
}

func signedByte(value byte) int { return int(int8(value)) }

func clamp16(value int) int {
	if value > 32767 {
		return 32767
	}
	if value < -32768 {
		return -32768
	}
	return value
}

func read16(ram *[65536]byte, address uint16) int {
	return int(int16(uint16(ram[address]) | uint16(ram[address+1])<<8))
}

func write16(ram *[65536]byte, address uint16, value int) {
	value = clamp16(value)
	ram[address] = byte(value)
	ram[address+1] = byte(value >> 8)
}

func (d *dsp) keyVoice(index int, ram *[65536]byte) {
	voice := &d.voices[index]
	base := index * 0x10
	directory := uint16(d.registers[0x5d]) << 8
	entry := directory + uint16(d.registers[base+4])*4
	voice.block = uint16(ram[entry]) | uint16(ram[entry+1])<<8
	voice.loop = uint16(ram[entry+2]) | uint16(ram[entry+3])<<8
	voice.active = true
	voice.release = false
	voice.phase = 0
	voice.index = 0
	voice.previous1 = 0
	voice.previous2 = 0
	voice.envelope = 0
	voice.envelopeAt = 0
	voice.envelopeMode = envelopeGain
	if d.registers[base+5]&0x80 != 0 {
		voice.envelopeMode = envelopeAttack
	}
	d.registers[0x7c] &^= 1 << index
	d.decodeBlock(voice, ram)
}

func (d *dsp) decodeBlock(voice *dspVoice, ram *[65536]byte) {
	voice.header = ram[voice.block]
	rangeValue := int(voice.header >> 4)
	filter := (voice.header >> 2) & 3
	for sampleIndex := 0; sampleIndex < 16; sampleIndex++ {
		packed := ram[voice.block+1+uint16(sampleIndex/2)]
		nibble := packed >> 4
		if sampleIndex&1 != 0 {
			nibble = packed & 0x0f
		}
		signed := int(int8(nibble<<4)) >> 4
		value := 0
		if rangeValue <= 12 {
			value = signed << rangeValue
		} else if signed < 0 {
			value = -2048
		}
		switch filter {
		case 1:
			value += voice.previous1 * 15 / 16
		case 2:
			value += voice.previous1*61/32 - voice.previous2*15/16
		case 3:
			value += voice.previous1*115/64 - voice.previous2*13/16
		}
		value = clamp16(value)
		value &= ^1
		voice.decoded[sampleIndex] = value
		voice.previous2 = voice.previous1
		voice.previous1 = value
	}
}

func rateReady(counter *int, rate int) bool {
	if rate <= 0 || rate >= len(ratePeriods) {
		return false
	}
	*counter++
	if *counter < ratePeriods[rate] {
		return false
	}
	*counter = 0
	return true
}

func (d *dsp) updateEnvelope(index int) {
	voice := &d.voices[index]
	base := index * 0x10
	adsr1 := d.registers[base+5]
	adsr2 := d.registers[base+6]
	gain := d.registers[base+7]
	if voice.release {
		voice.envelope -= 8
	} else if adsr1&0x80 != 0 {
		switch voice.envelopeMode {
		case envelopeAttack:
			rate := int(adsr1&0x0f)*2 + 1
			if rateReady(&voice.envelopeAt, rate) {
				if rate == 31 {
					voice.envelope += 1024
				} else {
					voice.envelope += 32
				}
				if voice.envelope >= 0x7ff {
					voice.envelope = 0x7ff
					voice.envelopeMode = envelopeDecay
				}
			}
		case envelopeDecay:
			rate := int((adsr1>>4)&7)*2 + 16
			if rateReady(&voice.envelopeAt, rate) {
				voice.envelope -= ((voice.envelope - 1) >> 8) + 1
				level := (int(adsr2>>5) + 1) * 0x100
				if voice.envelope <= level {
					voice.envelopeMode = envelopeSustain
				}
			}
		case envelopeSustain:
			if rateReady(&voice.envelopeAt, int(adsr2&0x1f)) {
				voice.envelope -= ((voice.envelope - 1) >> 8) + 1
			}
		}
	} else if gain&0x80 == 0 {
		voice.envelope = int(gain&0x7f) << 4
	} else if rateReady(&voice.envelopeAt, int(gain&0x1f)) {
		switch (gain >> 5) & 3 {
		case 0:
			voice.envelope -= 32
		case 1:
			voice.envelope -= ((voice.envelope - 1) >> 8) + 1
		case 2:
			voice.envelope += 32
		case 3:
			if voice.envelope < 0x600 {
				voice.envelope += 32
			} else {
				voice.envelope += 8
			}
		}
	}
	if voice.envelope > 0x7ff {
		voice.envelope = 0x7ff
	}
	if voice.envelope <= 0 {
		voice.envelope = 0
		if voice.release {
			voice.active = false
		}
	}
	d.registers[base+8] = byte(voice.envelope >> 4)
}

func (d *dsp) updateNoise() int {
	rate := int(d.registers[0x6c] & 0x1f)
	if rateReady(&d.noiseAt, rate) {
		feedback := (d.noise ^ (d.noise >> 1)) & 1
		d.noise = (d.noise >> 1) | feedback<<14
	}
	return int(int16(d.noise << 1))
}

func (d *dsp) advanceVoice(index int, pitch uint32, ram *[65536]byte) {
	voice := &d.voices[index]
	voice.phase += pitch
	for voice.phase >= 0x1000 && voice.active {
		voice.phase -= 0x1000
		voice.index++
		if voice.index < 16 {
			continue
		}
		voice.index = 0
		if voice.header&1 != 0 {
			d.registers[0x7c] |= 1 << index
			if voice.header&2 == 0 {
				voice.active = false
				voice.envelope = 0
				break
			}
			voice.block = voice.loop
		} else {
			voice.block += 9
		}
		d.decodeBlock(voice, ram)
	}
}

func (d *dsp) sample(ram *[65536]byte) (int16, int16) {
	if d.registers[0x6c]&0x80 != 0 {
		for index := range d.voices {
			d.voices[index].active = false
			d.voices[index].envelope = 0
		}
	}
	keyOff := d.keyOff
	d.keyOff = 0
	for index := 0; index < 8; index++ {
		if keyOff&(1<<index) != 0 {
			d.voices[index].release = true
			d.voices[index].envelopeMode = envelopeRelease
		}
	}
	keyOn := d.keyOn &^ keyOff
	d.keyOn = 0
	for index := 0; index < 8; index++ {
		if keyOn&(1<<index) != 0 {
			d.keyVoice(index, ram)
		}
	}

	noise := d.updateNoise()
	dryL, dryR := 0, 0
	echoInputL, echoInputR := 0, 0
	previous := 0
	for index := 0; index < 8; index++ {
		voice := &d.voices[index]
		if !voice.active {
			previous = 0
			continue
		}
		d.updateEnvelope(index)
		if !voice.active {
			previous = 0
			continue
		}
		base := index * 0x10
		pitch := uint32(d.registers[base+2]) | uint32(d.registers[base+3]&0x3f)<<8
		if index > 0 && d.registers[0x2d]&(1<<index) != 0 {
			modulated := int(pitch) + (previous * int(pitch) >> 15)
			if modulated < 0 {
				modulated = 0
			}
			if modulated > 0x3fff {
				modulated = 0x3fff
			}
			pitch = uint32(modulated)
		}
		sample := voice.decoded[voice.index]
		// Linear interpolation is intentionally used for preview generation:
		// it keeps the implementation compact while retaining pitch and timbre.
		// The authentic game runtime remains the reference for bit-exact S-DSP
		// Gaussian interpolation.
		next := sample
		if voice.index < 15 {
			next = voice.decoded[voice.index+1]
		}
		sample += (next - sample) * int(voice.phase) / 0x1000
		if d.registers[0x3d]&(1<<index) != 0 {
			sample = noise
		}
		sample = sample * voice.envelope / 0x800
		voice.lastOutput = sample
		previous = sample
		d.registers[base+9] = byte(int8(clamp16(sample) >> 8))
		left := sample * signedByte(d.registers[base]) / 128
		right := sample * signedByte(d.registers[base+1]) / 128
		dryL += left
		dryR += right
		if d.registers[0x4d]&(1<<index) != 0 {
			echoInputL += left
			echoInputR += right
		}
		d.advanceVoice(index, pitch, ram)
	}

	echoLength := uint16(d.registers[0x7d]&0x0f) * 0x800
	if echoLength == 0 {
		echoLength = 4
	}
	echoAddress := uint16(d.registers[0x6d])<<8 + d.echoAt
	echoL := read16(ram, echoAddress)
	echoR := read16(ram, echoAddress+2)
	d.firL[d.firAt], d.firR[d.firAt] = echoL, echoR
	filteredL, filteredR := 0, 0
	for tap := 0; tap < 8; tap++ {
		position := (d.firAt - tap) & 7
		coefficient := signedByte(d.registers[tap*0x10+0x0f])
		filteredL += d.firL[position] * coefficient / 128
		filteredR += d.firR[position] * coefficient / 128
	}
	d.firAt = (d.firAt + 1) & 7
	feedback := signedByte(d.registers[0x0d])
	if d.registers[0x6c]&0x20 == 0 {
		write16(ram, echoAddress, echoInputL+filteredL*feedback/128)
		write16(ram, echoAddress+2, echoInputR+filteredR*feedback/128)
	}
	d.echoAt += 4
	if d.echoAt >= echoLength {
		d.echoAt = 0
	}

	left := dryL*signedByte(d.registers[0x0c])/128 +
		filteredL*signedByte(d.registers[0x2c])/128
	right := dryR*signedByte(d.registers[0x1c])/128 +
		filteredR*signedByte(d.registers[0x3c])/128
	if d.registers[0x6c]&0x40 != 0 {
		left, right = 0, 0
	}
	return int16(clamp16(left)), int16(clamp16(right))
}
