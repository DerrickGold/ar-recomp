// Package cfg builds mode-sensitive basic-block control-flow graphs from the
// v2 decoder. A block is keyed by the complete decoder state, so the same PC
// reached with different M/X modes remains distinct.
package cfg

import "github.com/DerrickGold/snesrecomp-go/internal/decoder"

var blockEnders = map[string]bool{
	"RTS": true, "RTL": true, "RTI": true, "STP": true, "WAI": true, "BRK": true,
	"BRA": true, "BRL": true,
	"BPL": true, "BMI": true, "BVC": true, "BVS": true,
	"BCC": true, "BCS": true, "BNE": true, "BEQ": true,
	"JMP": true, "JSR": true, "JSL": true,
}

type Block struct {
	Entry        decoder.DecodeKey
	Instructions []*decoder.DecodedInstruction
	Successors   []decoder.DecodeKey
	Predecessors []decoder.DecodeKey
}

func (block *Block) Last() *decoder.DecodedInstruction {
	return block.Instructions[len(block.Instructions)-1]
}

type Graph struct {
	Entry             decoder.DecodeKey
	Blocks            map[decoder.DecodeKey]*Block
	Order             []decoder.DecodeKey
	Dominators        map[decoder.DecodeKey]decoder.DecodeKey
	DominanceFrontier map[decoder.DecodeKey]map[decoder.DecodeKey]struct{}
}

func Build(graph *decoder.Graph) *Graph {
	predecessors := make(map[decoder.DecodeKey][]decoder.DecodeKey)
	for _, key := range graph.Order {
		for _, successor := range graph.Instructions[key].Successors {
			predecessors[successor] = append(predecessors[successor], key)
		}
	}
	leaders := identifyLeaders(graph, predecessors)
	blocks, order := buildBlocks(graph, leaders)
	dominators := computeDominators(blocks, graph.Entry)
	return &Graph{
		Entry:             graph.Entry,
		Blocks:            blocks,
		Order:             order,
		Dominators:        dominators,
		DominanceFrontier: computeDominanceFrontier(blocks, order, dominators),
	}
}

func identifyLeaders(graph *decoder.Graph, predecessors map[decoder.DecodeKey][]decoder.DecodeKey) map[decoder.DecodeKey]struct{} {
	leaders := map[decoder.DecodeKey]struct{}{graph.Entry: {}}
	for _, key := range graph.Order {
		instruction := graph.Instructions[key]
		if !blockEnders[instruction.Instruction.Mnemonic] && len(instruction.Successors) == 1 {
			continue
		}
		for _, successor := range instruction.Successors {
			if graph.Instructions[successor] != nil {
				leaders[successor] = struct{}{}
			}
		}
	}
	for key, incoming := range predecessors {
		if len(incoming) > 1 && graph.Instructions[key] != nil {
			leaders[key] = struct{}{}
		}
	}
	return leaders
}

func buildBlocks(graph *decoder.Graph, leaders map[decoder.DecodeKey]struct{}) (map[decoder.DecodeKey]*Block, []decoder.DecodeKey) {
	blocks := make(map[decoder.DecodeKey]*Block)
	order := make([]decoder.DecodeKey, 0, len(leaders))
	// Iterating decoder order makes block order reproducible even though leader
	// membership is represented as a map.
	for _, leader := range graph.Order {
		if _, found := leaders[leader]; !found || graph.Instructions[leader] == nil {
			continue
		}
		var instructions []*decoder.DecodedInstruction
		seen := make(map[decoder.DecodeKey]struct{})
		current := leader
		for {
			instruction := graph.Instructions[current]
			if instruction == nil {
				break
			}
			if _, found := seen[current]; found {
				break
			}
			seen[current] = struct{}{}
			instructions = append(instructions, instruction)
			if blockEnders[instruction.Instruction.Mnemonic] || len(instruction.Successors) != 1 {
				break
			}
			next := instruction.Successors[0]
			if _, found := leaders[next]; found && next != leader {
				break
			}
			current = next
		}
		if len(instructions) == 0 {
			continue
		}
		last := instructions[len(instructions)-1]
		blocks[leader] = &Block{
			Entry:        leader,
			Instructions: instructions,
			Successors:   append([]decoder.DecodeKey(nil), last.Successors...),
		}
		order = append(order, leader)
	}
	for _, entry := range order {
		for _, successor := range blocks[entry].Successors {
			if target := blocks[successor]; target != nil {
				target.Predecessors = append(target.Predecessors, entry)
			}
		}
	}
	return blocks, order
}

// computeDominators implements the iterative Cooper-Harvey-Kennedy algorithm.
func computeDominators(blocks map[decoder.DecodeKey]*Block, entry decoder.DecodeKey) map[decoder.DecodeKey]decoder.DecodeKey {
	if blocks[entry] == nil {
		return map[decoder.DecodeKey]decoder.DecodeKey{}
	}
	var postorder []decoder.DecodeKey
	visited := make(map[decoder.DecodeKey]struct{})
	var visit func(decoder.DecodeKey)
	visit = func(node decoder.DecodeKey) {
		if _, found := visited[node]; found {
			return
		}
		visited[node] = struct{}{}
		block := blocks[node]
		if block == nil {
			return
		}
		for _, successor := range block.Successors {
			if blocks[successor] != nil {
				visit(successor)
			}
		}
		postorder = append(postorder, node)
	}
	visit(entry)
	rpo := make([]decoder.DecodeKey, len(postorder))
	for index := range postorder {
		rpo[index] = postorder[len(postorder)-1-index]
	}
	rpoIndex := make(map[decoder.DecodeKey]int, len(rpo))
	for index, node := range rpo {
		rpoIndex[node] = index
	}
	idom := map[decoder.DecodeKey]decoder.DecodeKey{entry: entry}
	intersect := func(left, right decoder.DecodeKey) decoder.DecodeKey {
		for left != right {
			for rpoIndex[left] > rpoIndex[right] {
				left = idom[left]
			}
			for rpoIndex[right] > rpoIndex[left] {
				right = idom[right]
			}
		}
		return left
	}
	for changed := true; changed; {
		changed = false
		for _, node := range rpo[1:] {
			var processed []decoder.DecodeKey
			for _, predecessor := range blocks[node].Predecessors {
				if _, found := idom[predecessor]; found {
					processed = append(processed, predecessor)
				}
			}
			if len(processed) == 0 {
				continue
			}
			candidate := processed[0]
			for _, predecessor := range processed[1:] {
				candidate = intersect(predecessor, candidate)
			}
			if old, found := idom[node]; !found || old != candidate {
				idom[node] = candidate
				changed = true
			}
		}
	}
	return idom
}

func computeDominanceFrontier(blocks map[decoder.DecodeKey]*Block, order []decoder.DecodeKey, idom map[decoder.DecodeKey]decoder.DecodeKey) map[decoder.DecodeKey]map[decoder.DecodeKey]struct{} {
	frontier := make(map[decoder.DecodeKey]map[decoder.DecodeKey]struct{}, len(blocks))
	for _, key := range order {
		frontier[key] = make(map[decoder.DecodeKey]struct{})
	}
	for _, node := range order {
		block := blocks[node]
		if len(block.Predecessors) < 2 {
			continue
		}
		for _, predecessor := range block.Predecessors {
			if _, found := idom[predecessor]; !found {
				continue
			}
			runner := predecessor
			for runner != idom[node] {
				next, found := idom[runner]
				if !found {
					break
				}
				frontier[runner][node] = struct{}{}
				if next == runner {
					break
				}
				runner = next
			}
		}
	}
	return frontier
}
