package project

import (
	"context"
	"errors"
	"fmt"
	"math/rand"
	"reflect"
	"sync"
	"testing"
	"time"
)

// scheduleProbe drives runAdmitted with runs that block until the test
// releases them. It keeps its own account of admitted, unfinished units and
// checks the admission invariants against it as each unit is admitted.
type scheduleProbe struct {
	t         *testing.T
	estimates []int64
	admission compileAdmission
	started   chan int
	release   []chan error

	mutex    sync.Mutex
	live     map[int]bool
	order    []int
	violated []string
}

func newScheduleProbe(t *testing.T, admission compileAdmission, estimates ...int64) *scheduleProbe {
	probe := &scheduleProbe{t: t, estimates: estimates, admission: admission,
		started: make(chan int, len(estimates)), live: make(map[int]bool)}
	for range estimates {
		probe.release = append(probe.release, make(chan error, 1))
	}
	probe.admission.onAdmit = probe.admitted
	return probe
}

func (probe *scheduleProbe) admitted(index int) {
	probe.mutex.Lock()
	defer probe.mutex.Unlock()
	var reserved int64
	for other := range probe.live {
		reserved += probe.estimates[other]
	}
	if len(probe.live) >= probe.admission.Jobs {
		probe.violated = append(probe.violated, fmt.Sprintf("unit %d admitted beside %d running (jobs %d)",
			index, len(probe.live), probe.admission.Jobs))
	}
	if len(probe.live) > 0 && probe.admission.Budget > 0 &&
		reserved+probe.estimates[index] > probe.admission.Budget {
		probe.violated = append(probe.violated, fmt.Sprintf("unit %d (%d) admitted beside %d reserved (budget %d)",
			index, probe.estimates[index], reserved, probe.admission.Budget))
	}
	probe.live[index] = true
	probe.order = append(probe.order, index)
	probe.started <- index
}

func (probe *scheduleProbe) run(ctx context.Context, index int) error {
	var err error
	select {
	case err = <-probe.release[index]:
	case <-ctx.Done():
		err = ctx.Err()
	}
	// Leave the account before runAdmitted releases this unit's share.
	probe.mutex.Lock()
	delete(probe.live, index)
	probe.mutex.Unlock()
	return err
}

// expectStarts waits for exactly these units to start, in this order, and
// then confirms nothing else starts while the test holds the rest.
func (probe *scheduleProbe) expectStarts(indices ...int) {
	probe.t.Helper()
	for _, want := range indices {
		select {
		case got := <-probe.started:
			if got != want {
				probe.t.Fatalf("unit %d started, want unit %d", got, want)
			}
		case <-time.After(5 * time.Second):
			probe.t.Fatalf("unit %d never started", want)
		}
	}
	select {
	case extra := <-probe.started:
		probe.t.Fatalf("unit %d started early", extra)
	case <-time.After(50 * time.Millisecond):
	}
}

func (probe *scheduleProbe) finish(index int, err error) {
	probe.release[index] <- err
}

func (probe *scheduleProbe) start(ctx context.Context) <-chan error {
	done := make(chan error, 1)
	go func() {
		_, err := runAdmitted(ctx, probe.estimates, probe.admission, probe.run)
		done <- err
	}()
	return done
}

func (probe *scheduleProbe) wait(done <-chan error) error {
	probe.t.Helper()
	select {
	case err := <-done:
		probe.mutex.Lock()
		defer probe.mutex.Unlock()
		for _, violation := range probe.violated {
			probe.t.Error(violation)
		}
		return err
	case <-time.After(5 * time.Second):
		probe.t.Fatal("runAdmitted never returned")
		return nil
	}
}

func TestRunAdmittedKeepsEstimatesWithinTheBudget(t *testing.T) {
	probe := newScheduleProbe(t, compileAdmission{Jobs: 4, Budget: 7}, 5, 4, 3, 2, 1)
	done := probe.start(context.Background())
	probe.expectStarts(0) // 5 + 4 would exceed 7
	probe.finish(0, nil)
	probe.expectStarts(1, 2) // 4 + 3 fills the budget exactly
	probe.finish(1, nil)
	probe.expectStarts(3, 4) // 3 + 2 + 1
	probe.finish(2, nil)
	probe.finish(3, nil)
	probe.finish(4, nil)
	if err := probe.wait(done); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(probe.order, []int{0, 1, 2, 3, 4}) {
		t.Fatalf("start order %v", probe.order)
	}
}

func TestRunAdmittedRunsAnOversizedUnitAlone(t *testing.T) {
	probe := newScheduleProbe(t, compileAdmission{Jobs: 4, Budget: 5}, 3, 10, 1, 1)
	done := probe.start(context.Background())
	probe.expectStarts(0)
	// The oversized unit waits for the running one rather than being refused
	// or letting smaller units pass it.
	probe.finish(0, nil)
	probe.expectStarts(1)
	probe.finish(1, nil)
	probe.expectStarts(2, 3)
	probe.finish(2, nil)
	probe.finish(3, nil)
	if err := probe.wait(done); err != nil {
		t.Fatal(err)
	}
}

func TestRunAdmittedJobsIsTheCeilingWithoutABudget(t *testing.T) {
	probe := newScheduleProbe(t, compileAdmission{Jobs: 2}, 1<<40, 1<<40, 1<<40)
	done := probe.start(context.Background())
	probe.expectStarts(0, 1)
	probe.finish(1, nil)
	probe.expectStarts(2)
	probe.finish(0, nil)
	probe.finish(2, nil)
	if err := probe.wait(done); err != nil {
		t.Fatal(err)
	}
}

func TestRunAdmittedStartsNothingAfterAFailure(t *testing.T) {
	failure := errors.New("compile failed")
	probe := newScheduleProbe(t, compileAdmission{Jobs: 2, Budget: 100}, 1, 1, 1, 1)
	done := probe.start(context.Background())
	probe.expectStarts(0, 1)
	probe.finish(0, failure)
	// The unit already running finishes; nothing new starts.
	probe.expectStarts()
	probe.finish(1, nil)
	if err := probe.wait(done); !errors.Is(err, failure) {
		t.Fatalf("got %v, want the first failure", err)
	}
	if !reflect.DeepEqual(probe.order, []int{0, 1}) {
		t.Fatalf("units %v started", probe.order)
	}
}

func TestRunAdmittedStopsOnCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	probe := newScheduleProbe(t, compileAdmission{Jobs: 1}, 1, 1, 1)
	done := probe.start(ctx)
	probe.expectStarts(0)
	cancel()
	// The running unit observes the cancellation; its resulting error is not
	// reported as the cause and no queued unit starts.
	if err := probe.wait(done); !errors.Is(err, context.Canceled) {
		t.Fatalf("got %v, want context.Canceled", err)
	}
	if !reflect.DeepEqual(probe.order, []int{0}) {
		t.Fatalf("units %v started", probe.order)
	}
}

func TestRunAdmittedReportsPeaks(t *testing.T) {
	stats, err := runAdmitted(context.Background(), []int64{4, 3, 2}, compileAdmission{Jobs: 3, Budget: 9},
		func(context.Context, int) error { return nil })
	if err != nil {
		t.Fatal(err)
	}
	if stats.Started != 3 || stats.PeakRunning < 1 || stats.PeakRunning > 3 ||
		stats.PeakEstimated < 4 || stats.PeakEstimated > 9 {
		t.Fatalf("stats %+v", stats)
	}
}

func TestOrderCompileJobsIsLargestFirstAndDeterministic(t *testing.T) {
	want := []compileJob{
		{source: "b.c", estimate: 30},
		{source: "a.c", estimate: 20},
		{source: "c.c", estimate: 20},
		{source: "a2.c", estimate: 10},
		{source: "z.c", estimate: 10},
	}
	random := rand.New(rand.NewSource(1))
	for trial := 0; trial < 20; trial++ {
		jobs := append([]compileJob(nil), want...)
		random.Shuffle(len(jobs), func(i, j int) { jobs[i], jobs[j] = jobs[j], jobs[i] })
		orderCompileJobs(jobs)
		if !reflect.DeepEqual(jobs, want) {
			t.Fatalf("order %v", jobs)
		}
	}
}

func TestEstimatedCompileMemoryGrowsWithSource(t *testing.T) {
	if estimatedCompileMemory(-1) != estimatedCompileMemory(0) {
		t.Fatal("a negative size is not treated as empty")
	}
	small, large := estimatedCompileMemory(1<<20), estimatedCompileMemory(26<<20)
	if small <= compileMemoryBaseBytes || large <= small {
		t.Fatalf("estimates %d and %d", small, large)
	}
}
