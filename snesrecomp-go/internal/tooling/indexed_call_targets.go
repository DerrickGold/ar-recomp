package tooling

import (
	"github.com/DerrickGold/snesrecomp-go/internal/config"
	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
	"github.com/DerrickGold/snesrecomp-go/internal/rom"
)

// FiniteStoredIndexedCallTargets retains the indexed-consumer API while using
// the shared cold value engine. Value evidence can also feed saved pointers;
// neither consumer closes a dispatch edge or publishes an M/X/DB proof.
func FiniteStoredIndexedCallTargets(image rom.Image, configs map[byte]*config.Config, graphs, eligible []*decoder.Graph) []uint32 {
	return AnalyzeColdValueProvenance(image, configs, graphs, eligible).IndexedTargets
}
