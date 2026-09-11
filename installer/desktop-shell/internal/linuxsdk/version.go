package linuxsdk

import "strings"

// Same Debian version ordering as the independent snesbuild SDL resolver.
// Debian orders '~' before end-of-string, letters before punctuation, and
// digit runs numerically. This matters when two rebuilds share an SDL version.
func compareVersion(a, b string) int {
	split := func(s string) (epoch, upstream, revision string) {
		epoch = "0"
		if e, rest, found := strings.Cut(s, ":"); found {
			epoch, s = e, rest
		}
		upstream, revision = s, "0"
		if i := strings.LastIndex(s, "-"); i >= 0 {
			upstream, revision = s[:i], s[i+1:]
		}
		return
	}
	ae, au, ar := split(a)
	be, bu, br := split(b)
	for _, pair := range [][2]string{{ae, be}, {au, bu}, {ar, br}} {
		if d := compareDebianPart(pair[0], pair[1]); d != 0 {
			return d
		}
	}
	return 0
}

func compareDebianPart(a, b string) int {
	order := func(c byte) int {
		if c == '~' {
			return -1
		}
		if c == 0 || c >= '0' && c <= '9' {
			return 0
		}
		if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' {
			return int(c)
		}
		return int(c) + 256
	}
	digit := func(s string) bool { return len(s) > 0 && s[0] >= '0' && s[0] <= '9' }
	for a != "" || b != "" {
		for a != "" && !digit(a) || b != "" && !digit(b) {
			var ac, bc byte
			if a != "" {
				ac = a[0]
			}
			if b != "" {
				bc = b[0]
			}
			if d := order(ac) - order(bc); d != 0 {
				return d
			}
			if a != "" {
				a = a[1:]
			}
			if b != "" {
				b = b[1:]
			}
		}
		for strings.HasPrefix(a, "0") {
			a = a[1:]
		}
		for strings.HasPrefix(b, "0") {
			b = b[1:]
		}
		first := 0
		for digit(a) && digit(b) {
			if first == 0 {
				first = int(a[0]) - int(b[0])
			}
			a = a[1:]
			b = b[1:]
		}
		if digit(a) {
			return 1
		}
		if digit(b) {
			return -1
		}
		if first != 0 {
			return first
		}
	}
	return 0
}
