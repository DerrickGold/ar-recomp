package tooling

import (
	"bytes"
	"strings"
	"testing"
)

func TestDispatchCensusUsesCumulativeMilestoneMaximum(t *testing.T) {
	trace := strings.Join([]string{
		`{"ch":"call","site":"00B543"}`,
		`{"ch":"dispatch","site":"05DB84","target":"05C123","m":0,"x":1,"e":0,"found":0,"mirrored":0,"trapped":1,"hits":1,"final":0}`,
		`{"ch":"dispatch","site":"05DB84","target":"05C123","m":0,"x":1,"e":0,"found":0,"mirrored":0,"trapped":1,"hits":1048576,"final":0}`,
		`{"ch":"dispatch","site":"05DB84","target":"05C123","m":0,"x":1,"e":0,"found":0,"mirrored":0,"trapped":1,"hits":20000000,"final":1}`,
		`{"ch":"dispatch","site":"069B48","target":"069CE3","m":0,"x":0,"e":0,"found":1,"mirrored":0,"hits":4,"final":1}`,
		`{"ch":"dispatch","site":"069CF4","target":"069B46","m":0,"x":0,"e":0,"found":0,"mirrored":0,"continuation":1,"hits":8,"final":1}`,
	}, "\n")
	report, err := ParseDispatchCensus(strings.NewReader(trace))
	if err != nil {
		t.Fatal(err)
	}
	if report.Version != 2 || report.Provenance != "snesrecomp-runtime-dispatch-census-v2" || report.RawRecords != 5 || len(report.Observations) != 3 || report.MissingBodies != 1 || report.TrappedSites != 1 {
		t.Fatalf("unexpected report: %+v", report)
	}
	missing := report.Observations[0]
	if missing.SitePC != 0x05db84 || missing.TargetPC != 0x05c123 ||
		missing.M != 0 || missing.X != 1 || missing.ObservationCount != 20000000 || missing.Found || !missing.Trapped {
		t.Fatalf("missing observation = %+v", missing)
	}
	var output bytes.Buffer
	if err := WriteDispatchCensus(&output, report, "text", true); err != nil {
		t.Fatal(err)
	}
	for _, wanted := range []string{
		"$05:DB84 -> $05:C123 M0X1 x20000000 TRAPPED before dispatch, MISSING",
		"adding func alone does not execute the edge",
		"func Observed_05_C123_M0X1 C123 entry_mx:0,1",
		"verify routine/handler/continuation semantics",
	} {
		if !strings.Contains(output.String(), wanted) {
			t.Fatalf("text report missing %q:\n%s", wanted, output.String())
		}
	}
}

func TestDispatchCensusRejectsMalformedAddress(t *testing.T) {
	_, err := ParseDispatchCensus(strings.NewReader(
		`{"ch":"dispatch","site":"not-hex","target":"05C123","hits":1}`))
	if err == nil || !strings.Contains(err.Error(), "site") {
		t.Fatalf("error = %v, want site parse failure", err)
	}
}
