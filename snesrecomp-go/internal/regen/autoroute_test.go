package regen

import (
	"reflect"
	"strings"
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/emitter"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

func autorouteImage(t *testing.T, writes map[uint32][]byte) rom.Image {
	t.Helper()
	image := make(rom.Image, 0x10000)
	for address, bytes := range writes {
		offset, err := image.Offset(byte(address>>16), uint16(address))
		if err != nil {
			t.Fatalf("offset $%06X: %v", address, err)
		}
		copy(image[offset:], bytes)
	}
	return image
}

func autorouteRepo(image rom.Image, configs ...*config.Config) *repository {
	repo := &repository{image: image, byBank: make(map[byte]*bankState)}
	for _, cfg := range configs {
		bank := &bankState{ID: cfg.Bank, Config: cfg}
		repo.banks = append(repo.banks, bank)
		repo.byBank[cfg.Bank] = bank
		for _, region := range cfg.DataRegions {
			repo.allDataRegions = append(repo.allDataRegions, decoder.DataRegion{Bank: region.Bank, Start: region.Start, End: region.End})
		}
	}
	return repo
}

// The dispatcher at $8010 indexes a three-slot table at $9000: two handlers
// (DEC cancels the RTS increment) around an unused zero slot, then a non-code
// word that ends the table. Both handlers are native returns.
func phaRTSImage(t *testing.T, ldaOpcode byte) rom.Image {
	return autorouteImage(t, map[uint32][]byte{
		0x008010: {ldaOpcode, 0x00, 0x90, 0x3a, 0x48, 0xe2, 0x30, 0x60},
		0x009000: {0x00, 0x81, 0x00, 0x00, 0x00, 0x82, 0x34, 0x12},
		0x008100: {0x60},
		0x008200: {0x60},
	})
}

func phaRTSConfig() *config.Config {
	return &config.Config{Bank: 0, Entries: []config.Entry{
		{Name: "Dispatcher", Start: 0x8000, EntryMX: config.MX{M: 0, X: 0}},
		{Name: "HandlerA", Start: 0x8100, EntryMX: config.MX{M: 1, X: 1}},
		{Name: "HandlerB", Start: 0x8200, EntryMX: config.MX{M: 1, X: 1}},
	}}
}

func TestPHARTSAutorouteGuardsNativeRTS(t *testing.T) {
	for _, test := range []struct {
		opcode byte
		reg    string
	}{{0xb9, "Y"}, {0xbd, "X"}} {
		cfg := phaRTSConfig()
		repo := autorouteRepo(phaRTSImage(t, test.opcode), cfg)
		routes := repo.routePHARTSDispatch()
		want := []PHARTSRoute{{Site: 0x008017, Table: 0x009000, Count: 2, IndexReg: test.reg, Owner: "Dispatcher"}}
		if !reflect.DeepEqual(routes, want) {
			t.Fatalf("idx:%s routes = %#v, want %#v", test.reg, routes, want)
		}
		directive := config.RTSDispatch{SitePC: 0x8017, Targets: []uint16{0x8100, 0x8200}}
		if len(cfg.IndirectDispatch) != 0 || !reflect.DeepEqual(cfg.RTSDispatch, []config.RTSDispatch{directive}) {
			t.Fatalf("idx:%s directive = %#v, collapsed %#v", test.reg, cfg.RTSDispatch, cfg.IndirectDispatch)
		}
		if again := repo.routePHARTSDispatch(); len(again) != 0 {
			t.Fatalf("idx:%s second pass re-routed %#v", test.reg, again)
		}

		// Both M0 and M1 retain the native instructions. M1 pushes just one
		// byte, so its actual RTS address can depend on the older stack byte.
		// A live DB different from PB likewise remains a native memory read.
		for m := uint8(0); m < 2; m++ {
			options := emitter.DecodeOptionsFromConfig(0, cfg)
			graph, err := decoder.DecodeFunction(repo.image, 0, 0x8010, m, 0, options)
			if err != nil {
				t.Fatalf("idx:%s M%d decode: %v", test.reg, m, err)
			}
			for _, step := range []struct {
				pc uint32
				op string
			}{{0x8010, "LDA"}, {0x8013, "DEC"}, {0x8014, "PHA"}, {0x8015, "SEP"}} {
				ins := graph.Instructions[decoder.DecodeKey{PC: step.pc, M: m, X: 0}]
				if ins == nil || ins.Instruction.Mnemonic != step.op || len(ins.Instruction.DispatchEntries) != 0 {
					t.Fatalf("idx:%s M%d native %s was collapsed: %#v", test.reg, m, step.op, ins)
				}
			}
			rts := graph.Instructions[decoder.DecodeKey{PC: 0x8017, M: 1, X: 1}]
			if rts == nil || rts.Instruction.DispatchKind != "rts_trick" ||
				!reflect.DeepEqual(rts.Instruction.DispatchEntries, []uint32{0x8100, 0x8200}) ||
				!reflect.DeepEqual(rts.Successors, []decoder.DecodeKey{{PC: 0x8100, M: 1, X: 1}, {PC: 0x8200, M: 1, X: 1}}) {
				t.Fatalf("idx:%s M%d guarded RTS = %#v", test.reg, m, rts)
			}
			result, err := emitter.EmitFunction(repo.image, 0, 0x8010, m, 0, emitter.FunctionOptions{Name: "Dispatch", Decode: options})
			if err != nil {
				t.Fatal(err)
			}
			for _, want := range []string{"cpu->DB", "CPU_STACK_OP_PHA", "switch (_rts_t)", "case 0x8100:", "if (_rts_mx == 3)", "unknown -> normal return"} {
				if !strings.Contains(result.Source, want) {
					t.Fatalf("M%d missing %q:\n%s", m, want, result.Source)
				}
			}
			for _, forbidden := range []string{"case 0x8101:", "switch (_idx)", "_disp_n"} {
				if strings.Contains(result.Source, forbidden) {
					t.Fatalf("collapsed dispatch %q in M%d", forbidden, m)
				}
			}
		}
	}
}

func TestPHARTSAutorouteDefersToAuthoredAndBoundsTables(t *testing.T) {
	for authoredPC := uint16(0x8010); authoredPC < 0x8018; authoredPC++ {
		cfg := phaRTSConfig()
		cfg.RTSDispatch = []config.RTSDispatch{{SitePC: authoredPC, Targets: []uint16{0x8100}}}
		if routes := autorouteRepo(phaRTSImage(t, 0xb9), cfg).routePHARTSDispatch(); len(routes) != 0 {
			t.Fatalf("authored site $%04X was re-routed: %#v", authoredPC, routes)
		}
	}

	cfg := phaRTSConfig()
	cfg.DataRegions = []config.DataRegion{{Bank: 0, Start: 0x8010, End: 0x8018}}
	if routes := autorouteRepo(phaRTSImage(t, 0xb9), cfg).routePHARTSDispatch(); len(routes) != 0 {
		t.Fatalf("data region was routed: %#v", routes)
	}

	// A declared entry two slots in ends the table there.
	cfg = phaRTSConfig()
	cfg.Entries = append(cfg.Entries, config.Entry{Name: "AfterTable", Start: 0x9004, EntryMX: config.MX{M: 1, X: 1}})
	routes := autorouteRepo(phaRTSImage(t, 0xb9), cfg).routePHARTSDispatch()
	if len(routes) != 1 || routes[0].Count != 1 {
		t.Fatalf("table bounded by next entry: %#v", routes)
	}

	// A slot pointing back into its own table is data, not a handler.
	image := phaRTSImage(t, 0xb9)
	offset, _ := image.Offset(0, 0x9004)
	image[offset], image[offset+1] = 0x01, 0x90
	routes = autorouteRepo(image, phaRTSConfig()).routePHARTSDispatch()
	if len(routes) != 1 || routes[0].Count != 1 {
		t.Fatalf("self-referencing slot: %#v", routes)
	}

	// An empty table (first slot is not code) synthesizes nothing.
	image = phaRTSImage(t, 0xb9)
	offset, _ = image.Offset(0, 0x9000)
	image[offset], image[offset+1] = 0x34, 0x12
	if routes := autorouteRepo(image, phaRTSConfig()).routePHARTSDispatch(); len(routes) != 0 {
		t.Fatalf("empty table routed: %#v", routes)
	}

	// The window must lie inside the owning function's declared range.
	cfg = phaRTSConfig()
	end := uint16(0x8014)
	cfg.Entries[0].End = &end
	if routes := autorouteRepo(phaRTSImage(t, 0xb9), cfg).routePHARTSDispatch(); len(routes) != 0 {
		t.Fatalf("site past end: was routed: %#v", routes)
	}
}

func TestPHARTSAutoroutePreservesAllSiteContracts(t *testing.T) {
	for _, kind := range []string{"indirect", "hle", "width", "exit", "data", "exclude"} {
		for pc := uint16(0x8010); pc < 0x8018; pc++ {
			cfg := phaRTSConfig()
			switch kind {
			case "indirect":
				cfg.IndirectDispatch = []config.IndirectDispatch{{SitePC: pc}}
			case "hle":
				cfg.HLEDispatch = map[uint16]string{pc: "NativeOverride"}
			case "width":
				cfg.ForceVariantAt = map[uint32]config.MX{uint32(pc): {M: 1, X: 0}}
			case "exit":
				cfg.ExitMXAt = []config.ExitMXAt{{Address: uint32(pc), Exit: config.MX{M: 0, X: 0}}}
			case "data":
				cfg.DataRegions = []config.DataRegion{{Bank: 0, Start: pc, End: pc + 1}}
			case "exclude":
				cfg.ExcludeRanges = []config.Range{{Start: pc, End: pc + 1}}
			}
			if routes := autorouteRepo(phaRTSImage(t, 0xbd), cfg).routePHARTSDispatch(); len(routes) != 0 {
				t.Fatalf("%s at $%04X was overridden: %#v", kind, pc, routes)
			}
		}
	}
	cfg := phaRTSConfig()
	cfg.HLEFunctions = map[uint16]string{0x8000: "OwnerOverride"}
	if routes := autorouteRepo(phaRTSImage(t, 0xbd), cfg).routePHARTSDispatch(); len(routes) != 0 {
		t.Fatalf("HLE owner was scanned: %#v", routes)
	}
}

func TestPHARTSTargetInventoryIsBoundedAndNotAClosedTable(t *testing.T) {
	for _, test := range []struct {
		name      string
		words     []uint16
		want      []uint16
		configure func(*config.Config)
	}{
		{name: "duplicates and nulls", words: []uint16{0x8100, 0, 0x8100, 0x8200, 0x1234}, want: []uint16{0x8100, 0x8200}},
		{name: "raw bank end", words: []uint16{0xffff, 0x1234}, want: []uint16{0xffff}},
		{name: "only nulls", words: []uint16{0, 0, 0x1234}},
		{name: "forward table target", words: []uint16{0x9004, 0x8100, 0, 0x1234}, want: []uint16{0x8100}},
		{name: "target is data", words: []uint16{0x8100, 0x8200, 0x1234}, want: []uint16{0x8100}, configure: func(c *config.Config) { c.DataRegions = []config.DataRegion{{Bank: 0, Start: 0x8200, End: 0x8201}} }},
		{name: "target excluded", words: []uint16{0x8100, 0x8200, 0x1234}, want: []uint16{0x8100}, configure: func(c *config.Config) { c.ExcludeRanges = []config.Range{{Start: 0x8200, End: 0x8201}} }},
		{name: "table data extent", words: []uint16{0x8100, 0x8200, 0x1234}, want: []uint16{0x8100}, configure: func(c *config.Config) { c.DataRegions = []config.DataRegion{{Bank: 0, Start: 0x9000, End: 0x9002}} }},
		{name: "table exclusion", words: []uint16{0x8100, 0x8200, 0x1234}, want: []uint16{0x8100}, configure: func(c *config.Config) { c.ExcludeRanges = []config.Range{{Start: 0x9003, End: 0x9004}} }},
	} {
		t.Run(test.name, func(t *testing.T) {
			cfg := phaRTSConfig()
			if test.configure != nil {
				test.configure(cfg)
			}
			image := phaRTSImage(t, 0xb9)
			offset, _ := image.Offset(0, 0x9000)
			for i, word := range test.words {
				image[offset+i*2] = byte(word)
				image[offset+i*2+1] = byte(word >> 8)
			}
			repo := autorouteRepo(image, cfg)
			if got := repo.phaRTSTargets(repo.banks[0], 0x9000, nil); !reflect.DeepEqual(got, test.want) {
				t.Fatalf("targets=%#v want=%#v", got, test.want)
			}
		})
	}
	// Even a fully pointer-shaped suffix stops after 256 slots.
	image := phaRTSImage(t, 0xb9)
	offset, _ := image.Offset(0, 0x9000)
	for i := 0; i <= phaRTSTableLimit; i++ {
		word := uint16(0x8100 + i)
		image[offset+i*2] = byte(word)
		image[offset+i*2+1] = byte(word >> 8)
	}
	repo := autorouteRepo(image, phaRTSConfig())
	if got := repo.phaRTSTargets(repo.banks[0], 0x9000, nil); len(got) != phaRTSTableLimit || got[len(got)-1] != 0x81ff {
		t.Fatalf("unbounded targets: %#v", got)
	}
	// A final complete word is legal; no read wraps into another bank.
	image = autorouteImage(t, map[uint32][]byte{0x00fffe: {0x00, 0x81}})
	repo = autorouteRepo(image, phaRTSConfig())
	if got := repo.phaRTSTargets(repo.banks[0], 0xfffe, nil); !reflect.DeepEqual(got, []uint16{0x8100}) {
		t.Fatalf("last word=%#v", got)
	}
}

// $00:8200 is PHB PHK PLB JSR $8300 PLB RTL in front of the body at $8300.
func wrapperImage(t *testing.T) rom.Image {
	return autorouteImage(t, map[uint32][]byte{
		0x008200: {0x8b, 0x4b, 0xab, 0x20, 0x00, 0x83, 0xab, 0x6b},
	})
}

func TestWrapperAutorouteGivesBypassedAliasItsOwnEntry(t *testing.T) {
	body := &config.Config{Bank: 0, Entries: []config.Entry{{Name: "Body", Start: 0x8300, EntryMX: config.MX{M: 1, X: 1}}}}
	caller := &config.Config{Bank: 1, Names: []config.NameDecl{
		{Address: 0x008200, Name: "Body"},
		{Address: 0x008300, Name: "Body"},
	}}
	repo := autorouteRepo(wrapperImage(t), body, caller)
	routes := repo.routeDBWrappers()
	want := []WrapperRoute{{Wrapper: 0x008200, Body: 0x008300, Name: "Body", Routed: "_AutoWrap_Body__00_8200"}}
	if !reflect.DeepEqual(routes, want) {
		t.Fatalf("routes = %#v, want %#v", routes, want)
	}
	if caller.Names[0].Name != "_AutoWrap_Body__00_8200" || caller.Names[1].Name != "Body" {
		t.Fatalf("aliases = %#v", caller.Names)
	}
	repo.promoteCrossBankNames()
	repo.rebuildNames()
	if repo.names[0x008200] != "_AutoWrap_Body__00_8200" || repo.names[0x008300] != "Body" {
		t.Fatalf("names after promotion: wrapper %q body %q", repo.names[0x008200], repo.names[0x008300])
	}
	found := false
	for _, entry := range body.Entries {
		found = found || (entry.Start == 0x8200 && entry.Name == "_AutoWrap_Body__00_8200")
	}
	if !found {
		t.Fatalf("wrapper entry was not promoted: %#v", body.Entries)
	}
}

func TestWrapperAutorouteLeavesSoundAliasesAlone(t *testing.T) {
	// The alias names a func declared at the wrapper itself.
	own := &config.Config{Bank: 0, Entries: []config.Entry{{Name: "Far", Start: 0x8200, EntryMX: config.MX{M: 1, X: 1}}}}
	ownCaller := &config.Config{Bank: 1, Names: []config.NameDecl{{Address: 0x008200, Name: "Far"}}}
	// The alias names nothing declared, so promotion already emits the wrapper.
	fresh := &config.Config{Bank: 0}
	freshCaller := &config.Config{Bank: 1, Names: []config.NameDecl{{Address: 0x008200, Name: "FarOnly"}}}
	for _, configs := range [][]*config.Config{{own, ownCaller}, {fresh, freshCaller}} {
		if routes := autorouteRepo(wrapperImage(t), configs...).routeDBWrappers(); len(routes) != 0 {
			t.Fatalf("sound alias was rewritten: %#v", routes)
		}
	}
	// Without the PLB/RTL tail this is not a DB wrapper.
	image := wrapperImage(t)
	offset, _ := image.Offset(0, 0x8207)
	image[offset] = 0x60
	body := &config.Config{Bank: 0, Entries: []config.Entry{{Name: "Body", Start: 0x8300, EntryMX: config.MX{M: 1, X: 1}}}}
	caller := &config.Config{Bank: 1, Names: []config.NameDecl{{Address: 0x008200, Name: "Body"}}}
	if routes := autorouteRepo(image, body, caller).routeDBWrappers(); len(routes) != 0 {
		t.Fatalf("non-wrapper was rewritten: %#v", routes)
	}
}

func TestWrapperAutorouteHonorsBodyIdentityAndExplicitContracts(t *testing.T) {
	for _, test := range []struct {
		name      string
		configure func(*config.Config, *config.Config)
	}{
		{"different body", func(body, caller *config.Config) { body.Entries[0].Start = 0x8400 }},
		{"existing independent alias", func(body, caller *config.Config) {
			caller.Names = append(caller.Names, config.NameDecl{Address: 0x008200, Name: "FarOnly"})
		}},
		{"generated name already owned", func(body, caller *config.Config) {
			body.Entries = append(body.Entries, config.Entry{Name: "_AutoWrap_Body__00_8200", Start: 0x8400})
		}},
		{"explicit wrapper", func(body, caller *config.Config) {
			body.Entries = append(body.Entries, config.Entry{Name: "Far", Start: 0x8200, EntryMX: config.MX{M: 0, X: 0}})
		}},
		{"ambiguous body name", func(body, caller *config.Config) {
			body.Entries = append(body.Entries, config.Entry{Name: "Body", Start: 0x8400})
		}},
		{"HLE wrapper", func(body, caller *config.Config) { body.HLEFunctions = map[uint16]string{0x8200: "FarOverride"} }},
		{"HLE inside wrapper", func(body, caller *config.Config) { body.HLEDispatch = map[uint16]string{0x8203: "CallOverride"} }},
		{"data suffix", func(body, caller *config.Config) {
			body.DataRegions = []config.DataRegion{{Bank: 0, Start: 0x8206, End: 0x8208}}
		}},
		{"excluded suffix", func(body, caller *config.Config) { body.ExcludeRanges = []config.Range{{Start: 0x8206, End: 0x8208}} }},
		{"excluded body", func(body, caller *config.Config) { body.ExcludeRanges = []config.Range{{Start: 0x8300, End: 0x8301}} }},
	} {
		t.Run(test.name, func(t *testing.T) {
			body := &config.Config{Bank: 0, Entries: []config.Entry{{Name: "Body", Start: 0x8300, EntryMX: config.MX{M: 1, X: 1}}}}
			caller := &config.Config{Bank: 1, Names: []config.NameDecl{{Address: 0x008200, Name: "Body"}}}
			test.configure(body, caller)
			if routes := autorouteRepo(wrapperImage(t), body, caller).routeDBWrappers(); len(routes) != 0 || caller.Names[0].Name != "Body" {
				t.Fatalf("authored contract changed: %#v %#v", routes, caller.Names)
			}
		})
	}
}
