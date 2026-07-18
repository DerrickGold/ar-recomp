// Package work creates deterministic, balanced per-function emit shards.
package work

import (
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/config"
)

type Bank struct {
	ID      byte
	Entries []config.Entry
}

type Chunk struct {
	Bank       byte
	FirstEntry int
	Entries    []config.Entry
}

type Worker struct {
	ID     int
	Weight int
	Chunks []Chunk
}

// Shard splits banks into stable, contiguous chunks and greedily balances the
// chunks across workers. Results remain byte-stable because the emitter merges
// each chunk by (bank, FirstEntry), never by completion order.
func Shard(banks []Bank, jobs, chunkSize int) []Worker {
	if jobs < 1 {
		jobs = 1
	}
	if chunkSize < 1 {
		chunkSize = 1
	}
	workers := make([]Worker, jobs)
	for index := range workers {
		workers[index].ID = index
	}
	var chunks []Chunk
	for _, bank := range banks {
		for first := 0; first < len(bank.Entries); first += chunkSize {
			end := min(first+chunkSize, len(bank.Entries))
			chunks = append(chunks, Chunk{Bank: bank.ID, FirstEntry: first, Entries: bank.Entries[first:end]})
		}
	}
	// Large chunks first improves greedy balance; stable tie-breakers preserve
	// deterministic assignments.
	sort.SliceStable(chunks, func(i, j int) bool {
		if len(chunks[i].Entries) != len(chunks[j].Entries) {
			return len(chunks[i].Entries) > len(chunks[j].Entries)
		}
		if chunks[i].Bank != chunks[j].Bank {
			return chunks[i].Bank < chunks[j].Bank
		}
		return chunks[i].FirstEntry < chunks[j].FirstEntry
	})
	for _, chunk := range chunks {
		lightest := 0
		for index := 1; index < len(workers); index++ {
			if workers[index].Weight < workers[lightest].Weight {
				lightest = index
			}
		}
		workers[lightest].Chunks = append(workers[lightest].Chunks, chunk)
		workers[lightest].Weight += len(chunk.Entries)
	}
	return workers
}
