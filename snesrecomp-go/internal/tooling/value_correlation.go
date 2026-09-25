package tooling

import (
	"slices"
	"sort"

	"github.com/DerrickGold/snesrecomp-go/internal/decoder"
)

// Values read through ONE index definition stay correlated. When a consumer
// combines two of them (a table base and a state published from the same
// record, say), their index values must agree. This is a cold correlation
// hypothesis about decoded paths, not an execution-order or slot-lifetime
// proof. Dropping a binding only widens the inventory.
const coldValueBindingLimit = 2

type coldValueBinding struct {
	key   decoder.DecodeKey // zero: unused
	value uint16
}

type coldValueBindings [coldValueBindingLimit]coldValueBinding

// coldBind merges two binding sets. ok=false means the operands were selected
// through the same definition with different index values.
func coldBind(a, b coldValueBindings) (coldValueBindings, bool) {
	var all []coldValueBinding
	for _, x := range [...]coldValueBinding{a[0], a[1], b[0], b[1]} {
		if x.key == (decoder.DecodeKey{}) {
			continue
		}
		duplicate := false
		for _, y := range all {
			if y.key == x.key {
				if y.value != x.value {
					return coldValueBindings{}, false
				}
				duplicate = true
			}
		}
		if !duplicate {
			all = append(all, x)
		}
	}
	sort.Slice(all, func(i, j int) bool { return coldKeyLess(all[i].key, all[j].key) })
	var out coldValueBindings
	copy(out[:], all) // excess keys are dropped: a wider, still valid superset
	return out, true
}

func coldBindingLess(a, b coldValueBindings) bool {
	for n := range a {
		if a[n].key != b[n].key {
			return coldKeyLess(a[n].key, b[n].key)
		}
		if a[n].value != b[n].value {
			return a[n].value < b[n].value
		}
	}
	return false
}

func coldWithoutBindings(v []coldWordValue) []coldWordValue {
	out := slices.Clone(v)
	for n := range out {
		out[n].binding = coldValueBindings{}
	}
	return coldWordSet(out)
}

type coldReadFamily struct {
	key     decoder.DecodeKey // index definition; zero disables correlation
	context int
}

type coldRecordFamily struct {
	fields map[[2]uint16]bool // bank, word field offset
	bound  uint16
	has    bool
	users  map[int]bool
}

// Record-pointer reads (small field offsets through address-valued indices)
// share one family per index definition, so a bound found through one field
// also limits the others.
func (e *coldValueEngine) noteRecordFamily(f coldReadFamily, banks []byte, field uint16, indices []coldWordValue) {
	if f.key == (decoder.DecodeKey{}) || field >= 0x100 {
		return
	}
	st := e.families[f]
	if st == nil {
		st = &coldRecordFamily{fields: map[[2]uint16]bool{}, users: map[int]bool{}}
		e.families[f] = st
	}
	if e.current >= 0 {
		st.users[e.current] = true
	}
	for _, b := range banks {
		st.fields[[2]uint16{uint16(b), field}] = true
	}
	bound, has := e.recordFamilyBound(st, indices)
	if bound != st.bound || has != st.has {
		st.bound, st.has = bound, has
		var users []int
		for u := range st.users {
			users = append(users, u)
		}
		slices.Sort(users)
		for _, u := range users {
			e.enqueue(u)
		}
	}
	if st.has {
		e.conditions["self_delimiting_record_family"] = true
	}
}

// A record array cannot extend into storage its own records point at. Sweep
// the records in address order. A field word that points forward to a later
// record boundary (aligned with the stride) bounds all remaining records. This
// is a pointed-storage extent hypothesis, not a table length or selector bound.
func (e *coldValueEngine) recordFamilyBound(st *coldRecordFamily, indices []coldWordValue) (uint16, bool) {
	var records []uint16
	for _, v := range indices {
		if !v.origin.valid && v.word >= 0x100 {
			records = append(records, v.word)
		}
	}
	slices.Sort(records)
	records = slices.Compact(records)
	if len(records) < 2 {
		return 0, false
	}
	first := uint32(records[0])
	stride := uint32(0)
	for _, r := range records[1:] {
		stride = coldGCD(stride, uint32(r)-first)
	}
	if stride < 3 {
		return 0, false
	}
	var fields [][2]uint16
	for f := range st.fields {
		fields = append(fields, f)
	}
	sort.Slice(fields, func(i, j int) bool {
		if fields[i][0] != fields[j][0] {
			return fields[i][0] < fields[j][0]
		}
		return fields[i][1] < fields[j][1]
	})
	bound, has := uint32(0), false
	for _, r := range records {
		if has && uint32(r) >= bound {
			break
		}
		for _, f := range fields {
			word, ok := shadowStreamROMWord(e.image, byte(f[0]), uint32(r)+uint32(f[1]))
			w := uint32(word)
			if ok && w > uint32(r) && w >= first+stride && (w-first)%stride == 0 && (!has || w < bound) {
				bound, has = w, true
			}
		}
	}
	return uint16(bound), has
}

func coldGCD(a, b uint32) uint32 {
	for b != 0 {
		a, b = b, a%b
	}
	return a
}
