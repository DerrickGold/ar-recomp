package tooling

import (
	"fmt"
	"io"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/analysis"
	romimage "github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// ShadowDispatchSite describes analysis evidence, not an emitted trap. In
// particular, an HLE route does not prove the helper's target coverage, and a
// census describes the captured build, not the current generation's registry.
// None of these records participates in static-fact selection or regeneration.
type ShadowDispatchSite struct {
	ShadowUnresolvedSite
	Routing            string                   `json:"routing"`
	HLEFunction        string                   `json:"hle_function,omitempty"`
	StaticStatus       string                   `json:"static_status"`
	TargetSetStatus    string                   `json:"target_set_status"`
	AuthoredTargets    []uint32                 `json:"authored_targets,omitempty"`
	StaticTargets      []uint32                 `json:"static_targets,omitempty"`
	TargetCandidates   []uint32                 `json:"target_candidates,omitempty"`
	DecodedOccurrences int                      `json:"decoded_occurrences"`
	PointerProducers   []ShadowPointerProducer  `json:"pointer_producers,omitempty"`
	StoredTargetFlows  []ShadowStoredTargetFlow `json:"stored_target_flows,omitempty"`
}

type ShadowDispatchSummary struct {
	UniqueSites              int `json:"unique_sites"`
	HLERoutedSites           int `json:"hle_routed_sites"`
	HLEUnprovenTargetSites   int `json:"hle_unproven_target_sites"`
	ObservedSites            int `json:"observed_sites"`
	UnobservedSites          int `json:"unobserved_sites"`
	ObservedOnlySites        int `json:"observed_only_sites"`
	ObservedMissingBodySites int `json:"observed_missing_body_sites"`
	ObservedTrappedSites     int `json:"observed_trapped_sites"`
	PointerProducerSites     int `json:"pointer_producer_sites"`
	StoredTargetSites        int `json:"stored_target_sites"`
	StoredTargetWriters      int `json:"stored_target_writer_sites"`
	StoredTargetCandidates   int `json:"stored_target_candidate_addresses"`
	StoredTargetAuthored     int `json:"stored_target_authored_addresses"`
}

func newShadowDispatchSite(pc uint32) ShadowDispatchSite {
	return ShadowDispatchSite{
		ShadowUnresolvedSite: ShadowUnresolvedSite{
			SitePC: pc & 0xffffff, Classification: shadowUnresolvedGeneric,
			Priority: shadowPriorityNormal, Reachability: "not_established",
			Reason: "target coverage has not been established",
		},
		Routing: "unknown", StaticStatus: "not_analyzed", TargetSetStatus: "unproven",
	}
}

func collectShadowDispatchInventory(image romimage.Image, banks []shadowBank, results []shadowDecodeResult, report ShadowReport) []ShadowDispatchSite {
	sites := make(map[uint32]*ShadowDispatchSite)
	get := func(pc uint32) *ShadowDispatchSite {
		pc &= 0xffffff
		if sites[pc] == nil {
			site := newShadowDispatchSite(pc)
			sites[pc] = &site
		}
		return sites[pc]
	}
	for _, unresolved := range report.Unresolved {
		site := get(unresolved.SitePC)
		site.ShadowUnresolvedSite = unresolved
		site.Callers = append([]ShadowCaller(nil), unresolved.Callers...)
		site.Routing, site.StaticStatus = "compiler", "unresolved"
	}
	for _, comparison := range report.Comparisons {
		site := get(comparison.SitePC)
		site.Routing, site.StaticStatus = "compiler", string(comparison.Status)
		if fact := comparison.Authored; fact != nil {
			site.InstructionBytes, site.Mnemonic = fact.InstructionBytes, fact.Mnemonic
			site.AuthoredTargets = append([]uint32(nil), fact.Targets...)
			site.Reachability = "authored_only_not_decoded"
			if fact.TargetSetClosed && !fact.FieldUnknown("targets") {
				site.TargetSetStatus = "authored_closed"
			}
		}
		if fact := comparison.Inferred; fact != nil {
			site.InstructionBytes, site.Mnemonic = fact.InstructionBytes, fact.Mnemonic
			site.StaticTargets = append([]uint32(nil), fact.Targets...)
			site.TargetCandidates = append([]uint32(nil), fact.TargetCandidates...)
			if fact.TargetSetClosed && !fact.FieldUnknown("targets") && hasOnlyStaticProof(fact.Evidence) {
				site.TargetSetStatus = "statically_closed"
			} else if site.TargetSetStatus == "unproven" {
				site.TargetSetStatus = "open"
			}
		}
		if comparison.Status == analysis.ComparisonConflict {
			site.TargetSetStatus = "conflict"
		}
	}
	// Keep authored hooks even when no analyzed root reaches the site. Do not
	// manufacture a function, decode width, or reachability claim for the hook.
	for _, bank := range banks {
		for pc, name := range bank.Config.HLEDispatch {
			site := get(uint32(bank.ID)<<16 | uint32(pc))
			site.Routing, site.HLEFunction = "hle", name
			if site.Reachability == "not_established" {
				site.Reachability = "authored_only_not_decoded"
			}
		}
	}
	attachShadowStoredTargets(image, banks, results, get)
	// Inspect the already decoded instructions. Changing decoder options here
	// would change the fixed point and the proven-analysis overlay's output.
	for _, result := range results {
		if result.issue != nil {
			continue
		}
		for _, producer := range result.pointerProducers {
			// Findings only annotate inventory sites; they never create roots.
			site := get(producer.TrampolinePC)
			site.PointerProducers = append(site.PointerProducers, producer)
		}
	}
	attachShadowPointerIndexEvidence(image, banks, results, sites)
	for _, result := range results {
		if result.issue != nil {
			continue
		}
		for _, decoded := range result.instructions {
			site := sites[decoded.PC&0xffffff]
			if site == nil {
				continue
			}
			site.DecodedOccurrences++
			site.Reachability = "configuration_or_static_call_rooted"
			site.Mnemonic = decoded.Instruction.Mnemonic
			site.AddressingMode = decoded.Instruction.Mode.String()
			site.Operand = decoded.Instruction.Operand
			site.InstructionBytes = shadowInstructionBytes(image, byte(decoded.PC>>16), uint16(decoded.PC), decoded.Instruction.Length)
			site.Callers = append(site.Callers, ShadowCaller{
				FunctionEntry: decoded.FunctionEntry, LiveMX: analysis.MXState{M: decoded.M, X: decoded.X},
			})
		}
	}
	var inventory []ShadowDispatchSite
	for _, site := range sites {
		site.PointerProducers = normalizeShadowPointerProducers(site.PointerProducers)
		sort.Slice(site.StoredTargetFlows, func(i, j int) bool {
			return shadowStoredJSONKey(site.StoredTargetFlows[i]) < shadowStoredJSONKey(site.StoredTargetFlows[j])
		})
		if site.Routing == "hle" && site.StaticStatus == "not_analyzed" && site.DecodedOccurrences > 0 {
			site.StaticStatus = "unresolved"
			classifyShadowUnresolved(image, &site.ShadowUnresolvedSite)
			if pattern := site.StreamDispatch; pattern != nil {
				for _, result := range results {
					for _, instruction := range result.instructions {
						if instruction.PC == pattern.StreamPointerLoadPC && (pattern.InterpreterEntryPC == 0 || instruction.FunctionEntry < pattern.InterpreterEntryPC) {
							pattern.InterpreterEntryPC = instruction.FunctionEntry
						}
					}
				}
				site.StructuralHandlerCandidates = removeShadowAddress(site.StructuralHandlerCandidates, pattern.InterpreterEntryPC)
			}
		}
		if site.Routing == "hle" && site.TargetSetStatus == "unproven" {
			site.Reason = "routed to a project HLE helper; its target set and generated-handler coverage are unproven"
		} else if site.TargetSetStatus != "unproven" {
			site.Reason = "target-set evidence does not establish reachability or generated-handler coverage; see static_status and target_set_status"
		}
		sort.Slice(site.Callers, func(i, j int) bool {
			a, b := site.Callers[i], site.Callers[j]
			if a.FunctionEntry != b.FunctionEntry {
				return a.FunctionEntry < b.FunctionEntry
			}
			if a.LiveMX.M != b.LiveMX.M {
				return a.LiveMX.M < b.LiveMX.M
			}
			return a.LiveMX.X < b.LiveMX.X
		})
		site.Callers = dedupeShadowCallers(site.Callers)
		inventory = append(inventory, *site)
	}
	sort.Slice(inventory, func(i, j int) bool { return inventory[i].SitePC < inventory[j].SitePC })
	return inventory
}

func summarizeShadowDispatchInventory(sites []ShadowDispatchSite) ShadowDispatchSummary {
	summary := ShadowDispatchSummary{UniqueSites: len(sites)}
	for _, site := range sites {
		if len(site.StoredTargetFlows) > 0 {
			summary.StoredTargetSites++
		}
		if len(site.PointerProducers) > 0 {
			summary.PointerProducerSites++
		}
		if site.Routing == "hle" {
			summary.HLERoutedSites++
			if site.TargetSetStatus != "statically_closed" && site.TargetSetStatus != "authored_closed" {
				summary.HLEUnprovenTargetSites++
			}
		}
		if site.Reachability == "observed_only" {
			summary.ObservedOnlySites++
		}
		if site.RuntimeStatus == shadowRuntimeUnobserved {
			summary.UnobservedSites++
		} else if site.RuntimeStatus != "" {
			summary.ObservedSites++
		}
		missing, trapped := false, false
		for _, observation := range site.RuntimeObservations {
			missing = missing || (!observation.Found && !observation.Continuation)
			trapped = trapped || observation.Trapped
		}
		if missing {
			summary.ObservedMissingBodySites++
		}
		if trapped {
			summary.ObservedTrappedSites++
		}
	}
	writers, targets, authored := make(map[uint32]bool), make(map[uint32]bool), make(map[uint32]bool)
	for _, site := range sites {
		for _, flow := range site.StoredTargetFlows {
			for _, writer := range flow.Writers {
				writers[writer.StorePC] = true
			}
			for _, target := range flow.Targets {
				targets[target.PC] = true
				if target.AuthoredEntry {
					authored[target.PC] = true
				}
			}
		}
	}
	summary.StoredTargetWriters, summary.StoredTargetCandidates, summary.StoredTargetAuthored = len(writers), len(targets), len(authored)
	return summary
}

func attachShadowRuntimeObservations(site *ShadowUnresolvedSite, observations []DispatchObservation) {
	site.RuntimeObservations = append([]DispatchObservation(nil), observations...)
	site.RuntimeObservationCount = 0
	site.RuntimeStatus = shadowRuntimeUnobserved
	for _, observation := range observations {
		site.RuntimeObservationCount += observation.ObservationCount
		status := shadowRuntimeObservedResolved
		switch {
		case observation.Trapped && !observation.Found && !observation.Continuation:
			status = shadowRuntimeObservedTrappedMissing
		case !observation.Found && !observation.Continuation:
			status = shadowRuntimeObservedMissing
		case observation.Trapped:
			status = shadowRuntimeObservedTrapped
		}
		if shadowUnresolvedRuntimeRank(ShadowUnresolvedSite{RuntimeStatus: status}) < shadowUnresolvedRuntimeRank(*site) {
			site.RuntimeStatus = status
		}
	}
}

func attachShadowDispatchInventoryEvidence(report *ShadowReport, bySite map[uint32][]DispatchObservation) {
	known := make(map[uint32]bool)
	for index := range report.DispatchSites {
		site := &report.DispatchSites[index]
		known[site.SitePC] = true
		attachShadowRuntimeObservations(&site.ShadowUnresolvedSite, bySite[site.SitePC])
	}
	// Observations outside the static inventory must remain actionable. They
	// do not become static roots or get a guessed HLE/compiler routing policy.
	for pc, observations := range bySite {
		if known[pc] {
			continue
		}
		site := newShadowDispatchSite(pc)
		site.Reachability = "observed_only"
		site.Reason = "runtime source has no matching static inventory record; routing and target coverage require investigation"
		attachShadowRuntimeObservations(&site.ShadowUnresolvedSite, observations)
		report.DispatchSites = append(report.DispatchSites, site)
	}
	sort.Slice(report.DispatchSites, func(i, j int) bool {
		a, b := report.DispatchSites[i], report.DispatchSites[j]
		ar, br := shadowUnresolvedRuntimeRank(a.ShadowUnresolvedSite), shadowUnresolvedRuntimeRank(b.ShadowUnresolvedSite)
		if ar != br {
			return ar < br
		}
		if a.RuntimeObservationCount != b.RuntimeObservationCount {
			return a.RuntimeObservationCount > b.RuntimeObservationCount
		}
		return a.SitePC < b.SitePC
	})
	report.DispatchSummary = summarizeShadowDispatchInventory(report.DispatchSites)
}

func writeShadowDispatchInventory(output io.Writer, report ShadowReport, verbose bool) {
	summary := report.DispatchSummary
	fmt.Fprintf(output, "dispatch inventory: %d unique source sites; HLE-routed=%d (target set unproven=%d); observed-only=%d\n",
		summary.UniqueSites, summary.HLERoutedSites, summary.HLEUnprovenTargetSites, summary.ObservedOnlySites)
	fmt.Fprintf(output, "  caller-side pointer producers: %d site(s), report-only; --verbose lists source chains and remaining proof obligations\n", summary.PointerProducerSites)
	fmt.Fprintf(output, "  memory-fed RTS targets: %d site(s), %d same-expression writer site(s), %d candidate addresses (%d authored-address overlaps); aliases, code ownership and M/X are not proven\n",
		summary.StoredTargetSites, summary.StoredTargetWriters, summary.StoredTargetCandidates, summary.StoredTargetAuthored)
	if report.DispatchEvidence != nil {
		fmt.Fprintf(output, "dispatch coverage in captured build: observed=%d unobserved=%d missing-body-sites=%d trapped-sites=%d\n",
			summary.ObservedSites, summary.UnobservedSites, summary.ObservedMissingBodySites, summary.ObservedTrappedSites)
	}
	fmt.Fprintln(output, "  No classified blockers is not a coverage certificate; HLE routing and observed successes do not prove a complete target set.")
	for _, site := range report.DispatchSites {
		if !verbose && site.Routing != "hle" && site.Reachability != "observed_only" &&
			site.RuntimeStatus != shadowRuntimeObservedMissing && site.RuntimeStatus != shadowRuntimeObservedTrappedMissing && site.RuntimeStatus != shadowRuntimeObservedTrapped {
			continue
		}
		fmt.Fprintf(output, "[DISPATCH-SITE] %s routing=%s", shadowAddress(site.SitePC), site.Routing)
		if site.HLEFunction != "" {
			fmt.Fprintf(output, ":%s", site.HLEFunction)
		}
		fmt.Fprintf(output, " static=%s target_set=%s runtime=%s hits=%d reachability=%s class=%s priority=%s\n",
			site.StaticStatus, site.TargetSetStatus, site.RuntimeStatus, site.RuntimeObservationCount, site.Reachability, site.Classification, site.Priority)
		if verbose {
			fmt.Fprintf(output, "  %s %s %s operand=$%X decoded-occurrences=%d reason=%s\n",
				site.InstructionBytes, site.Mnemonic, site.AddressingMode, site.Operand, site.DecodedOccurrences, site.Reason)
			for _, caller := range site.Callers {
				fmt.Fprintf(output, "  caller=%s M%dX%d\n", shadowAddress(caller.FunctionEntry), caller.LiveMX.M, caller.LiveMX.X)
			}
			for _, flow := range site.StoredTargetFlows {
				fmt.Fprintf(output, "  stored-target (report-only): decoded-contexts=%d load=%s field=%s $%X push=%s target=(word%+d)&$FFFF M%dX%d writers=%d candidates=%d\n",
					len(flow.Contexts), shadowAddress(flow.LoadPC), flow.Field.Mode, flow.Field.Operand,
					shadowAddress(flow.PushPC), flow.TargetAddend, flow.LiveMX.M, flow.LiveMX.X, len(flow.Writers), len(flow.Targets))
				fmt.Fprintf(output, "    obligations=%v\n", flow.ProofObligations)
				for _, writer := range flow.Writers {
					writeShadowStoredWriter(output, writer)
				}
				for _, source := range flow.SourceFields {
					var incoming []string
					for _, pc := range source.IncomingStorePCs {
						incoming = append(incoming, shadowAddress(pc))
					}
					fmt.Fprintf(output, "    source-field (conditional alias): depth=%d field=%s $%X consumed-by=%v writer-candidates=%d\n", source.Depth, source.Field.Mode, source.Field.Operand, incoming, len(source.Writers))
					for _, writer := range source.Writers {
						writeShadowStoredWriter(output, writer)
					}
				}
				if flow.SourcesTruncated {
					fmt.Fprintf(output, "    source-field depth limit=%d reached; deeper relationships remain unresolved\n", shadowStoredSourceDepth)
				}
				for _, target := range flow.Targets {
					fmt.Fprintf(output, "    candidate=%s writer=%s call=%s entry-kind=%s authored-address=%t ownership=%s\n", shadowAddress(target.PC), shadowAddress(target.StorePC), shadowAddress(target.CallPC), target.EntryKind, target.AuthoredEntry, target.Ownership)
				}
			}
			for _, producer := range site.PointerProducers {
				fmt.Fprintf(output, "  pointer-producer (report-only): caller=%s call=%s M%dX%d load=%s %s base=$%04X index=%s store=%s dp=$%02X -> slot=$%04X alias=%s required-D=$%04X\n",
					shadowAddress(producer.CallerEntryPC), shadowAddress(producer.CallPC), producer.CallMX.M, producer.CallMX.X,
					shadowAddress(producer.LoadPC), producer.LoadMode, producer.TableOffset, producer.IndexRegister,
					shadowAddress(producer.StorePC), producer.StoreOperand, producer.PointerAddress, producer.PointerAlias, producer.RequiredD)
				if producer.IndexOrigin != nil {
					origin := producer.IndexOrigin
					fmt.Fprintf(output, "    index-origin=%s %s %s operand=$%X (not a bound)\n", shadowAddress(origin.PC), origin.Mnemonic, origin.Mode, origin.Operand)
				}
				if evidence := producer.IndexEvidence; evidence != nil {
					fmt.Fprintf(output, "    index-source (report-only): match=%s values=%s writers=%d literal-values=%d samples=%d truncated=%t; no alias/domain/entry proof\n", evidence.WriterMatch, evidence.ValueSetStatus, len(evidence.Writers), len(evidence.Values), len(evidence.Reads), evidence.Truncated)
					for _, initializer := range evidence.Initializers {
						fmt.Fprintf(output, "      table-initializer (report-only): writer=%s word-addend=%d (not promoted)\n", shadowAddress(initializer.StorePC), initializer.ValueAddend)
						writeShadowInitializer(output, &initializer.Read, "        ")
					}
					for _, writer := range evidence.Writers {
						writeShadowStoredWriter(output, writer)
					}
					for _, value := range evidence.Values {
						var pcs []string
						for _, pc := range value.WriterPCs {
							pcs = append(pcs, shadowAddress(pc))
						}
						fmt.Fprintf(output, "      literal-index-candidate=$%04X writer-sites=%v\n", value.Value, pcs)
					}
					for _, sample := range evidence.Reads {
						fmt.Fprintf(output, "      ROM-read-sample index=$%04X DB=$%02X read=%s status=%s", sample.IndexValue, sample.DataBank, shadowAddress(sample.ReadPC), sample.Status)
						if sample.Word != nil {
							fmt.Fprintf(output, " word=$%04X", *sample.Word)
						}
						if sample.TargetPC != nil {
							fmt.Fprintf(output, " conditional-target=%s authored-address=%t ROM-mapped=%t", shadowAddress(*sample.TargetPC), sample.AuthoredEntry, sample.TargetROM)
						}
						fmt.Fprintln(output)
					}
				}
				for _, definition := range producer.DataBank.Constants {
					fmt.Fprintf(output, "    load-bank-definition=$%02X at=%s\n", definition.Value, shadowAddress(definition.DefinitionPC))
				}
				for _, base := range producer.ROMBaseCandidates {
					fmt.Fprintf(output, "    ROM-base-candidate=%s (not a table extent)\n", shadowAddress(base))
				}
				fmt.Fprintf(output, "    load-bank-unknown-paths=%t obligations=%v\n", producer.DataBank.UnknownPaths, producer.ProofObligations)
			}
		}
		for _, observation := range site.RuntimeObservations {
			if !verbose && observation.Found && !observation.Trapped {
				continue
			}
			fmt.Fprintf(output, "  observed-target=%s M%dX%d E%t hits=%d generated=%t continuation=%t trapped=%t\n",
				shadowAddress(observation.TargetPC), observation.M, observation.X, observation.Emulation,
				observation.ObservationCount, observation.Found, observation.Continuation, observation.Trapped)
		}
	}
}

func writeShadowStoredWriter(output io.Writer, writer ShadowStoredTargetWriter) {
	origin := "unknown"
	if writer.SourceMode != "" {
		origin = shadowAddress(writer.SourcePC)
	}
	fmt.Fprintf(output, "    writer=%s width=%d kind=%s origin=%s mode=%s operand=$%X addend=%d\n", shadowAddress(writer.StorePC), writer.Width, writer.Kind, origin, writer.SourceMode, writer.SourceOperand, writer.ValueAddend)
	if expression := writer.Expression; expression != nil {
		source := expression.Source
		fmt.Fprintf(output, "      value-expression (report-only): status=%s source=%s at=%s register=%s mode=%s operand=$%X reason=%s\n", expression.Status, source.Kind, shadowAddress(source.PC), source.Register, source.Mode, source.Operand, source.Reason)
		flag := func(evidence *ShadowStoredFlag) string {
			if evidence.Value == nil {
				return "unknown(" + evidence.Reason + ")"
			}
			return fmt.Sprintf("%d@%s", *evidence.Value, shadowAddress(evidence.DefinitionPC))
		}
		for _, operation := range expression.Operations {
			fmt.Fprintf(output, "      operation=%s %s operand=$%04X", shadowAddress(operation.PC), operation.Mnemonic, operation.Operand)
			if operation.Carry != nil && operation.Decimal != nil {
				fmt.Fprintf(output, " carry=%s decimal=%s", flag(operation.Carry), flag(operation.Decimal))
			}
			fmt.Fprintln(output)
		}
		if expression.ExactAddend != nil {
			fmt.Fprintf(output, "      exact-local-word-addend=%d (not a memory-alias or target-set proof)\n", *expression.ExactAddend)
		}
		if expression.Constant != nil {
			fmt.Fprintf(output, "      local-constant-word=$%04X (not promoted to a dispatch target)\n", *expression.Constant)
		}
	}
}
