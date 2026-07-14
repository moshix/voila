package interp

import "sort"

// PreludeNames returns the names visible without any `use` (sorted, for
// deterministic consumers like the IR lowering pass).
func PreludeNames() []string {
	p := buildPrelude()
	names := make([]string, 0, len(p))
	for n := range p {
		names = append(names, n)
	}
	sort.Strings(names)
	return names
}

// PackageNames returns the std package names (sorted).
func PackageNames() []string {
	pkgs := buildPackages()
	names := make([]string, 0, len(pkgs))
	for n := range pkgs {
		names = append(names, n)
	}
	sort.Strings(names)
	return names
}
