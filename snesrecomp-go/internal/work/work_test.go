package work

import (
	"testing"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
)

func TestShardBalancesDominantBank(t *testing.T) {
	banks := []Bank{
		{ID: 0, Entries: make([]config.Entry, 1512)},
		{ID: 1, Entries: make([]config.Entry, 128)},
		{ID: 2, Entries: make([]config.Entry, 1)},
		{ID: 3, Entries: make([]config.Entry, 107)},
	}
	workers := Shard(banks, 4, 32)
	minimum, maximum := workers[0].Weight, workers[0].Weight
	for _, worker := range workers[1:] {
		minimum = min(minimum, worker.Weight)
		maximum = max(maximum, worker.Weight)
	}
	if maximum-minimum > 32 {
		t.Fatalf("imbalanced shards: min=%d max=%d", minimum, maximum)
	}
}
