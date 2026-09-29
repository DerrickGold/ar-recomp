package project

import (
	"context"
	"fmt"
	"path/filepath"
	"sort"
	"sync"
)

// Hermetic compile scheduling.
//
// Units start largest-first, so the longest compiles begin while the most
// other work remains to overlap them, and the source path breaks ties, so the
// start order is a function of the inputs alone. Scheduling changes only when
// units START: objects are still archived and linked in source order.
//
// A unit starts once a --jobs slot is free and its estimated peak memory fits
// beside the estimates of the units already running. The budget bounds the
// sum of ESTIMATES; nothing measures or limits actual resident memory. A unit
// whose estimate alone exceeds the budget waits for every running unit to
// finish and then runs alone, so no unit is ever refused and the queue cannot
// deadlock. Units start strictly in order: a later, smaller unit never passes
// one that is waiting for memory.

// The estimate is a fixed base plus a per-source-byte slope, fitted to the
// peak RSS of pinned `zig cc -O2 -g` compiles of real generated units (16
// units from 476 B to 26.8 MB, calibrated 2026-09-29): within -6%..+24% of
// the measurement for units over 1 MB, and an overestimate for small ones.
const (
	compileMemoryBaseBytes     = 128 << 20
	compileMemoryPerSourceByte = 76
)

func estimatedCompileMemory(sourceBytes int64) int64 {
	if sourceBytes < 0 {
		sourceBytes = 0
	}
	return compileMemoryBaseBytes + compileMemoryPerSourceByte*sourceBytes
}

// reportCompileMemoryBudget resolves options.MemoryBudget for jobs, which
// are already in start order, and says in the build log what bounds
// concurrency, naming every unit that will compile alone.
func reportCompileMemoryBudget(options HermeticOptions, jobs []compileJob) int64 {
	budget, physical := options.MemoryBudget, int64(0)
	if budget == 0 {
		budget, physical = defaultCompileMemoryBudget()
	}
	largest := ""
	if len(jobs) > 0 {
		largest = fmt.Sprintf("; largest unit %s, estimated %s",
			filepath.Base(jobs[0].source), humanBytes(jobs[0].estimate))
	}
	switch {
	case options.MemoryBudget < 0:
		fmt.Fprintf(options.Stdout, "hermetic: compile memory budget off; --jobs alone bounds concurrency%s\n", largest)
		return 0
	case budget <= 0:
		fmt.Fprintf(options.Stdout, "hermetic: physical memory unknown, so no compile memory budget; --jobs alone bounds concurrency%s\n", largest)
		return 0
	case physical > 0:
		fmt.Fprintf(options.Stdout, "hermetic: compile memory budget %s of estimates (half of %s physical)%s\n",
			humanBytes(budget), humanBytes(physical), largest)
	default:
		fmt.Fprintf(options.Stdout, "hermetic: compile memory budget %s of estimates%s\n", humanBytes(budget), largest)
	}
	for _, item := range jobs {
		if item.estimate > budget {
			fmt.Fprintf(options.Stdout, "hermetic: %s is estimated at %s, over the budget, so it compiles alone\n",
				item.source, humanBytes(item.estimate))
		}
	}
	return budget
}

type compileJob struct {
	source, object, flags string
	runner                bool
	estimate              int64
}

// orderCompileJobs sorts jobs largest estimate first, then by source path.
func orderCompileJobs(jobs []compileJob) {
	sort.SliceStable(jobs, func(i, j int) bool {
		if jobs[i].estimate != jobs[j].estimate {
			return jobs[i].estimate > jobs[j].estimate
		}
		return jobs[i].source < jobs[j].source
	})
}

// compileAdmission is the admission policy: at most Jobs units at once, and
// their summed estimates within Budget bytes unless one runs alone. A Budget
// of zero or less means no memory budget.
type compileAdmission struct {
	Jobs   int
	Budget int64
	// onAdmit, when set, is called in admission order as each unit is
	// admitted, before its run starts.
	onAdmit func(index int)
}

func (admission compileAdmission) admits(running int, reserved, estimate int64) bool {
	if running >= admission.Jobs {
		return false
	}
	return running == 0 || admission.Budget <= 0 || reserved+estimate <= admission.Budget
}

type admissionStats struct {
	Started       int
	PeakRunning   int
	PeakEstimated int64
}

// runAdmitted calls run(ctx, index) for each index of estimates, in order,
// each on its own goroutine once admission allows it. After the first error,
// or once ctx is done, no further unit starts; units already running finish
// and runAdmitted returns that first error, or ctx's error. Errors a run
// returns after ctx is done are consequences of the cancellation and are not
// reported as the cause.
func runAdmitted(ctx context.Context, estimates []int64, admission compileAdmission,
	run func(ctx context.Context, index int) error) (admissionStats, error) {
	if admission.Jobs < 1 {
		admission.Jobs = 1
	}
	var (
		mutex     sync.Mutex
		changed   = sync.NewCond(&mutex)
		running   int
		reserved  int64
		firstErr  error
		stats     admissionStats
		waitGroup sync.WaitGroup
	)
	// A cancellation must wake a dispatcher that is waiting for admission.
	stopWaking := context.AfterFunc(ctx, func() {
		mutex.Lock()
		changed.Broadcast()
		mutex.Unlock()
	})
	defer stopWaking()
	for index, estimate := range estimates {
		mutex.Lock()
		for firstErr == nil && ctx.Err() == nil && !admission.admits(running, reserved, estimate) {
			changed.Wait()
		}
		if firstErr != nil || ctx.Err() != nil {
			mutex.Unlock()
			break
		}
		running++
		reserved += estimate
		stats.Started++
		stats.PeakRunning = max(stats.PeakRunning, running)
		stats.PeakEstimated = max(stats.PeakEstimated, reserved)
		if admission.onAdmit != nil {
			admission.onAdmit(index)
		}
		mutex.Unlock()
		waitGroup.Add(1)
		go func(index int, estimate int64) {
			defer waitGroup.Done()
			err := run(ctx, index)
			mutex.Lock()
			running--
			reserved -= estimate
			if err != nil && firstErr == nil && ctx.Err() == nil {
				firstErr = err
			}
			changed.Broadcast()
			mutex.Unlock()
		}(index, estimate)
	}
	waitGroup.Wait()
	if firstErr != nil {
		return stats, firstErr
	}
	return stats, ctx.Err()
}
