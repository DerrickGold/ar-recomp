// Package spcaudio is a small, audio-only SNES APU implementation. It models
// the SPC700, the S-DSP register interface, timers, BRR voices, and the pieces
// of the ActRaiser upload protocol needed to render local comparison WAVs.
//
// It is an original Go implementation and does not link to the playable C
// runner.
package spcaudio

import "fmt"

// SampleRate matches the native S-DSP cadence used by the project's runtime,
// so preview tempo and pitch line up with authentic in-game playback.
const SampleRate = 32040

type apuTimer struct {
	enabled bool
	target  byte
	divider int
	stage   int
	counter byte
}

type apu struct {
	ram      [65536]byte
	cpu      cpu
	dsp      dsp
	inPorts  [4]byte
	outPorts [4]byte
	dspAddr  byte
	control  byte
	timers   [3]apuTimer
	dspClock int
}

func newAPU() *apu {
	a := &apu{}
	a.cpu.apu = a
	a.cpu.sp = 0xef
	return a
}

func (a *apu) clone() *apu {
	copy := *a
	copy.cpu.apu = &copy
	return &copy
}

func (a *apu) read(address uint16) byte {
	if address < 0xf0 || address > 0xff {
		return a.ram[address]
	}
	switch address {
	case 0xf2:
		return a.dspAddr
	case 0xf3:
		return a.dsp.read(a.dspAddr)
	case 0xf4, 0xf5, 0xf6, 0xf7:
		return a.inPorts[address-0xf4]
	case 0xfd, 0xfe, 0xff:
		index := address - 0xfd
		value := a.timers[index].counter & 0x0f
		a.timers[index].counter = 0
		return value
	default:
		return a.ram[address]
	}
}

func (a *apu) write(address uint16, value byte) {
	if address < 0xf0 || address > 0xff {
		a.ram[address] = value
		return
	}
	switch address {
	case 0xf1:
		old := a.control
		a.control = value
		if value&0x10 != 0 {
			a.inPorts[0], a.inPorts[1] = 0, 0
		}
		if value&0x20 != 0 {
			a.inPorts[2], a.inPorts[3] = 0, 0
		}
		for index := range a.timers {
			mask := byte(1 << index)
			enabled := value&mask != 0
			if enabled && old&mask == 0 {
				a.timers[index].divider = 0
				a.timers[index].stage = 0
				a.timers[index].counter = 0
			}
			a.timers[index].enabled = enabled
		}
	case 0xf2:
		a.dspAddr = value
	case 0xf3:
		a.dsp.write(a.dspAddr, value)
	case 0xf4, 0xf5, 0xf6, 0xf7:
		a.outPorts[address-0xf4] = value
	case 0xfa, 0xfb, 0xfc:
		a.timers[address-0xfa].target = value
	case 0xf8, 0xf9:
		a.ram[address] = value
	default:
		// TEST, timer counters, and the unused locations are read-only or
		// hardware-control registers. Keeping their RAM shadow is useful to
		// drivers which read back an otherwise unspecified value.
		a.ram[address] = value
	}
}

func (a *apu) tickTimer(index int) {
	timer := &a.timers[index]
	if !timer.enabled {
		return
	}
	period := 128
	if index == 2 {
		period = 16
	}
	timer.divider++
	if timer.divider < period {
		return
	}
	timer.divider = 0
	timer.stage++
	target := int(timer.target)
	if target == 0 {
		target = 256
	}
	if timer.stage >= target {
		timer.stage = 0
		timer.counter = (timer.counter + 1) & 0x0f
	}
}

func (a *apu) tick(samples *[]int16) {
	for index := range a.timers {
		a.tickTimer(index)
	}
	a.dspClock++
	if a.dspClock == 32 {
		a.dspClock = 0
		left, right := a.dsp.sample(&a.ram)
		*samples = append(*samples, left, right)
	}
}

func (a *apu) step(samples *[]int16) error {
	if a.cpu.stopped {
		return fmt.Errorf("SPC700 stopped at $%04X", a.cpu.pc)
	}
	cycles := a.cpu.step()
	for index := 0; index < cycles; index++ {
		a.tick(samples)
	}
	return nil
}

func (a *apu) runCycles(limit int) error {
	var discard []int16
	for elapsed := 0; elapsed < limit; {
		before := a.cpu.cycles
		if err := a.step(&discard); err != nil {
			return err
		}
		elapsed += int(a.cpu.cycles - before)
		discard = discard[:0]
	}
	return nil
}

func (a *apu) renderFrames(frames int) ([]int16, error) {
	if frames < 0 {
		return nil, fmt.Errorf("negative frame count")
	}
	samples := make([]int16, 0, frames*2)
	for len(samples) < frames*2 {
		if err := a.step(&samples); err != nil {
			return nil, err
		}
	}
	return samples[:frames*2], nil
}
