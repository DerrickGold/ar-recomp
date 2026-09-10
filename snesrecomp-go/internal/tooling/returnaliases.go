package tooling

import (
	"fmt"
	"io"
	"slices"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	"github.com/DerrickGold/snesrecomp-go/internal/cpu65816"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

const returnAliasWriteLimit = 64

// Footprints are context observations, not universal summaries for a store PC.
// Ranges include every candidate byte and are inclusive, BEFORE 24-bit wrap.
type ShadowReturnWrite struct {
	PC               uint32                 `json:"site_pc"`
	InstructionBytes string                 `json:"instruction_bytes"`
	Mnemonic         string                 `json:"mnemonic"`
	Mode             string                 `json:"addressing_mode"`
	LiveMX           analysis.MXState       `json:"live_mx"`
	DB               *uint8                 `json:"known_DB,omitempty"`
	Width            int                    `json:"width_bytes,omitempty"`
	Index            string                 `json:"index_register,omitempty"`
	IndexRange       *[2]uint32             `json:"index_range_inclusive,omitempty"`
	IndexBits        *ShadowReturnIndexBits `json:"index_known_bits,omitempty"`
	ByteRange        *[2]uint32             `json:"unwrapped_bus_byte_range_inclusive,omitempty"`
	Status           string                 `json:"status"`
	Reason           string                 `json:"reason"`
}

type ShadowReturnIndexBits struct {
	Mask  uint16 `json:"mask"`
	Value uint16 `json:"value"`
}

func auditReturnWrite(s returnValueState, i *cpu65816.Instruction) ShadowReturnWrite {
	r := ShadowReturnWrite{PC: s.key.PC, Mnemonic: i.Mnemonic, Mode: i.Mode.String(), LiveMX: analysis.MXState{M: s.key.M, X: s.key.X}, Status: "may_alias_or_unmodeled", Reason: "write_effect_not_modeled"}
	r.InstructionBytes = fmt.Sprintf("%02X", i.Opcode)
	for b := uint8(1); b < i.Length && b < 4; b++ {
		r.InstructionBytes += fmt.Sprintf("%02X", byte(i.Operand>>(8*(b-1))))
	}
	if s.db.kind == returnValueConstant {
		db := uint8(s.db.value)
		r.DB = &db
	}
	// Only ordinary stores have no register/status or hidden memory effects.
	// RMW, block moves, direct/indirect addressing, and hardware are barriers.
	switch i.Mnemonic {
	case "STA", "STZ":
		r.Width = 2 - int(s.key.M)
	case "STX", "STY":
		r.Width = 2 - int(s.key.X)
	default:
		return r
	}
	var base uint32
	switch i.Mode {
	case cpu65816.LONG, cpu65816.LONGX:
		base = i.Operand & 0xffffff
	case cpu65816.ABS, cpu65816.ABSX, cpu65816.ABSY:
		if r.DB == nil {
			r.Reason = "absolute_store_DB_unknown"
			return r
		}
		base = uint32(*r.DB)<<16 | i.Operand&0xffff
	default:
		r.Reason = "direct_or_indirect_store_address_unproven"
		return r
	}
	var lo, hi uint32
	reg := 0
	switch i.Mode {
	case cpu65816.LONGX, cpu65816.ABSX:
		reg, r.Index = 1, "X"
	case cpu65816.ABSY:
		reg, r.Index = 2, "Y"
	}
	if reg != 0 {
		width := 2 - int(s.key.X)
		mask, value := returnWordBits(s.regs[reg], width)
		lo, hi = uint32(value), uint32(value|(^mask&returnWidthMask(width)))
		if mask != 0 && mask != returnWidthMask(width) {
			r.IndexBits = &ShadowReturnIndexBits{Mask: mask, Value: value}
		}
		r.IndexRange = &[2]uint32{lo, hi}
	}
	r.ByteRange = &[2]uint32{base + lo, base + hi + uint32(r.Width-1)}
	// Standard SNES system-bus property, independent of cartridge ROM layout:
	// stack operations use bank 00; only WRAM offsets 0000-1FFF alias bank 00.
	// Accept ONLY the remaining physical WRAM, not arbitrary nonzero banks.
	// Do not mask overflowing additions: crossing 7F:FFFF can reach an alias.
	if r.ByteRange[0] >= 0x7e2000 && r.ByteRange[1] <= 0x7fffff {
		r.Status, r.Reason = "disjoint_unmirrored_WRAM", "every_candidate_byte_outside_bank_zero_WRAM_mirror"
	} else {
		r.Reason = "byte_range_not_wholly_unmirrored_WRAM"
	}
	return r
}

func collectShadowReturnAliases(image romimage.Image, banks []shadowBank, results []shadowDecodeResult, previous ShadowReturnCalls) ShadowReturnCalls {
	r := ShadowReturnCalls{Scope: "report_only_standard_SNES_bus_return_stack_alias_contracts", Statuses: make(map[string]int), Obligations: []string{
		"selected_previous_return_call_queries_blocked_by_memory_writes_not_all_code",
		"standard_SNES_system_bus_only_WRAM_7E2000_through_7FFFFF_has_no_bank_zero_alias",
		"native_entry_frame_MX_and_writable_nonaliasing_stack_window_contracts",
		"interrupt_and_hardware_writes_preserve_the_guest_frame_and_tracked_registers",
		"ordinary_store_full_byte_ranges_only_unknown_DB_indices_and_wrap_remain_conservative",
		"immediate_logic_and_accumulator_shifts_preserve_known_bits_not_branch_or_memory_value_guesses",
		"other_memory_reads_remain_unknown_no_forwarded_WRAM_values",
		"existing_exact_variants_only_HLE_and_unmodeled_effects_remain_barriers",
		"context_specific_conditional_returns_not_termination_or_universal_callee_summaries",
		"independent_program_budget_no_changes_to_previous_query_answers",
		"no_inline_data_exit_facts_roots_cfg_or_generation_changes",
	}}
	var roots []decoder.Variant
	for _, entry := range previous.Entries {
		if entry.hasBlockedWrite || slices.ContainsFunc(entry.Blockers, func(b ShadowReturnValueBlocker) bool { return b.Reason == "memory_write_may_alias_return_frame" }) {
			roots = append(roots, decoder.Variant{Address: entry.EntryPC, M: entry.EntryMX.M, X: entry.EntryMX.X})
		}
	}
	return collectShadowReturnQueries(image, banks, results, roots, r, true)
}

func writeShadowReturnAliases(output io.Writer, r ShadowReturnCalls, verbose bool) {
	fmt.Fprintf(output, "return-stack aliases (report-only): %d requested entry variants, %d omitted, %d loaded programs; conditional preserved=%d adjusted=%d path-dependent=%d unproven=%d; program-budget=%t (not new roots or runtime debt)\n", r.RequestedEntries, r.EntriesOmitted, r.Programs, r.Statuses["conditional_incoming_PC_preserved"], r.Statuses["conditional_constant_adjustment"], r.Statuses["path_dependent_adjustment"], r.Statuses["unproven"], r.ProgramBudgetHit)
	if !verbose {
		return
	}
	for _, entry := range r.Entries {
		fmt.Fprintf(output, "[RETURN-ALIAS] %s M%dX%d %s blockers=%v write-contexts=%v writes-omitted=%d\n", shadowAddress(entry.EntryPC), entry.EntryMX.M, entry.EntryMX.X, entry.Status, entry.Blockers, entry.WriteStatuses, entry.WritesOmitted)
		for _, w := range entry.Writes {
			db, span, index := "unknown", "unknown", "none"
			if w.DB != nil {
				db = fmt.Sprintf("$%02X", *w.DB)
			}
			if w.ByteRange != nil {
				span = fmt.Sprintf("$%06X..$%06X", w.ByteRange[0], w.ByteRange[1])
			}
			if w.IndexRange != nil {
				index = fmt.Sprintf("%s[$%04X..$%04X]", w.Index, w.IndexRange[0], w.IndexRange[1])
			}
			if w.IndexBits != nil {
				index += fmt.Sprintf(" (bits & $%04X = $%04X)", w.IndexBits.Mask, w.IndexBits.Value)
			}
			fmt.Fprintf(output, "  write=%s bytes=%s %s %s M%dX%d width=%d DB=%s index=%s range=%s %s: %s\n", shadowAddress(w.PC), w.InstructionBytes, w.Mnemonic, w.Mode, w.LiveMX.M, w.LiveMX.X, w.Width, db, index, span, w.Status, w.Reason)
		}
	}
}
