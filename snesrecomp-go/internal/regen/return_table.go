package regen

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/emitter"
)

type NativeReturnTableSite struct {
	SitePC, HelperPC, TablePC, ReturnPC uint32
	Targets                             []uint32
	Modes                               [][2]uint8
}

func nativeReturnTableSites(results map[byte][]*emitter.FunctionResult) []NativeReturnTableSite {
	byPC := map[uint32]*NativeReturnTableSite{}
	for _, bank := range results {
		for _, result := range bank {
			if result == nil || result.Graph == nil {
				continue
			}
			for _, decoded := range result.Graph.Instructions {
				ins := decoded.Instruction
				r := ins.NativeReturnTable
				if r == nil {
					continue
				}
				site := byPC[ins.Address]
				if site == nil {
					site = &NativeReturnTableSite{SitePC: ins.Address, HelperPC: ins.Operand, TablePC: r.TablePC, ReturnPC: r.ReturnPC, Targets: append([]uint32(nil), r.Targets...)}
					byPC[ins.Address] = site
				}
				mode := [2]uint8{ins.M, ins.X}
				found := false
				for _, old := range site.Modes {
					found = found || old == mode
				}
				if !found {
					site.Modes = append(site.Modes, mode)
				}
			}
		}
	}
	var sites []NativeReturnTableSite
	for _, site := range byPC {
		sort.Slice(site.Modes, func(i, j int) bool {
			if site.Modes[i][0] != site.Modes[j][0] {
				return site.Modes[i][0] < site.Modes[j][0]
			}
			return site.Modes[i][1] < site.Modes[j][1]
		})
		sites = append(sites, *site)
	}
	sort.Slice(sites, func(i, j int) bool { return sites[i].SitePC < sites[j].SitePC })
	return sites
}

// Native helper contracts describe ROM instructions, not authored HLE/body
// substitutions. Snapshot all banks before discovery (also protecting direct
// calls across banks). Named entry roots alone do not change native semantics.
func nativeReturnConfigBarriers(bank byte, cfg *config.Config) [][2]uint32 {
	var ranges [][2]uint32
	point := func(pc uint16) {
		address := uint32(bank)<<16 | uint32(pc)
		ranges = append(ranges, [2]uint32{address, address})
	}
	for pc := range cfg.HLEFunctions {
		point(pc)
	}
	for pc := range cfg.HLEFunctionsIf {
		point(pc)
	}
	for pc := range cfg.HLEDispatch {
		point(pc)
	}
	for _, pc := range cfg.HLESPCUpload {
		point(pc)
	}
	for _, entry := range cfg.Entries {
		if entry.End != nil || entry.TailCallPC != nil || entry.EntrySOffset != 0 || entry.ExitMX != nil {
			point(entry.Start)
		}
	}
	for _, r := range cfg.ExcludeRanges {
		if r.End > r.Start {
			ranges = append(ranges, [2]uint32{uint32(bank)<<16 | uint32(r.Start), uint32(bank)<<16 | uint32(r.End-1)})
		}
	}
	for _, d := range cfg.IndirectDispatch {
		point(d.SitePC)
	}
	for _, d := range cfg.RTSDispatch {
		point(d.SitePC)
	}
	for _, e := range cfg.ExitMXAt {
		ranges = append(ranges, [2]uint32{e.Address, e.Address})
	}
	for pc := range cfg.ForceVariantAt {
		ranges = append(ranges, [2]uint32{pc, pc})
	}
	sort.Slice(ranges, func(i, j int) bool {
		if ranges[i][0] != ranges[j][0] {
			return ranges[i][0] < ranges[j][0]
		}
		return ranges[i][1] < ranges[j][1]
	})
	return ranges
}
