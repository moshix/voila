// Package loader resolves a program's user packages and flattens them into a
// single AST.
//
// One directory is one package (§11.2). A `use "pkg/sub"` that resolves to a
// directory under the module root is a USER package; anything else is a std
// package and is left to the runtime.
//
// Flattening: a declaration `Parse` in package `parser` becomes the global
// name `parser.Parse`, and every reference to it — qualified from outside,
// bare from inside — is rewritten to that name. Because the lexer can never
// produce an identifier containing a dot, these names cannot collide with
// anything the user wrote, and the checker, interpreter, IR and C backend need
// no knowledge of packages at all.
package loader

import (
	"os"
	"path/filepath"
	"sort"
	"strings"

	"voila/internal/ast"
	"voila/internal/diag"
	"voila/internal/parser"
)

// Program is a loaded, flattened program.
type Program struct {
	File    *ast.File
	Entry   *diag.Source
	Sources []*diag.Source // every file that took part, for diagnostics
}

type pkg struct {
	name    string // the qualifier: the directory's base name
	dir     string // the resolved directory — the package's identity
	files   []*ast.File
	sources []*diag.Source
	imports map[string]*pkg // qualifier (honouring `as`) → package
	decls   map[string]bool
}

type loader struct {
	root      string
	mod       string // module name from voila.mod, read once
	bag       *diag.Bag
	pkgs      map[string]*pkg   // keyed by DIRECTORY, not by base name
	qualifier map[string]string // base name → the directory that claimed it
	order     []*pkg            // dependency-first
	stack     []string          // directories, for cycle detection
}

// Load parses the entry file, pulls in every user package it reaches, and
// returns one flattened AST.
func Load(entry string, bag *diag.Bag) (*Program, error) {
	absEntry, err := filepath.Abs(entry)
	if err != nil {
		return nil, err
	}
	root := moduleRoot(filepath.Dir(absEntry))
	l := &loader{
		root:      root,
		mod:       moduleName(root),
		bag:       bag,
		pkgs:      map[string]*pkg{},
		qualifier: map[string]string{},
	}

	text, err := os.ReadFile(entry)
	if err != nil {
		return nil, err
	}
	src := diag.NewSource(entry, string(text))
	bag.AddSource(src)
	main := parser.Parse(src, bag)

	prog := &Program{Entry: src, Sources: []*diag.Source{src}}
	if bag.HasErrors() {
		prog.File = main
		return prog, nil
	}

	// Pull in every user package the entry file reaches, and build main's
	// qualifier map (honouring `as` aliases).
	mainImports := l.importsOf(main)
	if bag.HasErrors() {
		prog.File = main
		return prog, nil
	}
	l.checkGlobalNames()

	merged := &ast.File{Package: main.Package, P: main.P}
	for _, p := range l.order {
		for _, f := range p.files {
			r := &rewriter{self: p, imports: p.imports, bag: bag}
			r.file(f)
			merged.Decls = append(merged.Decls, f.Decls...)
			if len(f.ScriptStmts) > 0 {
				bag.Errorf(diag.SpanOf(f.ScriptStmts[0].Pos(), 1), "",
					"a package file may not contain loose statements; put them in a function")
			}
		}
		prog.Sources = append(prog.Sources, p.sources...)
	}

	rm := &rewriter{self: nil, imports: mainImports, bag: bag}
	rm.file(main)
	merged.Decls = append(merged.Decls, main.Decls...)
	merged.ScriptStmts = main.ScriptStmts
	merged.Uses = stdUses(main.Uses, l)

	prog.File = merged
	return prog, nil
}

// importsOf loads every user package a file uses and returns its qualifier map.
func (l *loader) importsOf(f *ast.File) map[string]*pkg {
	imports := map[string]*pkg{}
	for _, u := range f.Uses {
		dir, k := l.resolve(u.Path)
		switch k {
		case kindStd:
			continue
		case kindBad:
			l.bag.Errorf(diag.SpanOf(u.P, len("use")), "",
				"no package `%s`: it is not a std package and no such directory "+
					"exists under the module root (%s)", u.Path, l.root)
			continue
		}
		if len(u.Names) > 0 || u.Wildcard {
			l.bag.Errorf(diag.SpanOf(u.P, len("use")), "",
				"selective and wildcard imports are not supported for user packages; "+
					"write `%s.Name`", pkgName(u.Path))
			continue
		}
		p := l.loadPkg(pkgName(u.Path), dir, u.P)
		if p == nil {
			continue
		}
		q := u.Alias
		if q == "" {
			q = p.name
		}
		imports[q] = p
	}
	return imports
}

// checkGlobalNames rejects the collisions flattening cannot express: enum
// VARIANT names and TRAIT names stay global (patterns and trait bounds have no
// qualified syntax), so two packages may not both define one.
func (l *loader) checkGlobalNames() {
	type owner struct {
		pkg  string
		pos  diag.Pos
		what string
	}
	seen := map[string]owner{}
	claim := func(name, what string, pos diag.Pos, p *pkg) {
		if prev, ok := seen[name]; ok && prev.pkg != p.name {
			l.bag.Errorf(diag.SpanOf(pos, len(name)), "",
				"%s `%s` is declared in both `%s` and `%s`; %ss are global across "+
					"a program because patterns and trait bounds have no qualified form",
				what, name, prev.pkg, p.name, what)
			return
		}
		seen[name] = owner{pkg: p.name, pos: pos, what: what}
	}
	for _, p := range l.order {
		for _, f := range p.files {
			for _, d := range f.Decls {
				switch decl := d.(type) {
				case *ast.EnumDecl:
					for _, v := range decl.Variants {
						claim(v.Name, "variant", v.P, p)
					}
				case *ast.TraitDecl:
					claim(decl.Name, "trait", decl.P, p)
				}
			}
		}
	}
}

// stdUses keeps only the std imports; user packages are gone after flattening.
func stdUses(uses []*ast.Use, l *loader) []*ast.Use {
	var out []*ast.Use
	for _, u := range uses {
		if _, k := l.resolve(u.Path); k == kindStd {
			out = append(out, u)
		}
	}
	return out
}

// moduleRoot walks up to the nearest directory holding voila.mod; failing
// that, the entry file's own directory is the root.
func moduleRoot(dir string) string {
	d := dir
	for {
		if _, err := os.Stat(filepath.Join(d, "voila.mod")); err == nil {
			return d
		}
		parent := filepath.Dir(d)
		if parent == d {
			return dir
		}
		d = parent
	}
}

func pkgName(path string) string { return filepath.Base(path) }

// kind classifies an import path.
type kind int

const (
	kindStd  kind = iota // a std package: left to the runtime
	kindUser             // a directory under the module root
	kindBad              // neither — a typo, or an escape attempt
)

// resolve classifies an import path. An unresolvable path is an ERROR: it must
// never be waved through as "probably std", which would let a broken program
// pass `check` and fail at run time.
func (l *loader) resolve(path string) (string, kind) {
	if strings.HasPrefix(path, "std/") || (!strings.Contains(path, "/") && isStd(path)) {
		return "", kindStd
	}
	if filepath.IsAbs(path) || strings.Contains(path, "\\") {
		return "", kindBad
	}
	clean := strings.TrimPrefix(path, "./")
	if l.mod != "" && (clean == l.mod || strings.HasPrefix(clean, l.mod+"/")) {
		clean = strings.TrimPrefix(strings.TrimPrefix(clean, l.mod), "/")
	}
	dir := filepath.Join(l.root, filepath.FromSlash(clean))

	// The package must live UNDER the module root: `use "../../etc"` is not a
	// package reference, it is an escape.
	rel, err := filepath.Rel(l.root, dir)
	if err != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return "", kindBad
	}
	info, err := os.Stat(dir)
	if err != nil || !info.IsDir() {
		return "", kindBad
	}
	return dir, kindUser
}

func isStd(name string) bool {
	switch name {
	case "fmt", "str", "os", "math", "time", "json", "log", "regex", "http",
		"conv", "sort", "rand", "uuid", "io", "sync", "chan", "test", "vm", "ffi":
		return true
	}
	return false
}

// moduleName reads voila.mod once; resolve() consults the cached value.
func moduleName(root string) string {
	data, err := os.ReadFile(filepath.Join(root, "voila.mod"))
	if err != nil {
		return ""
	}
	for _, line := range strings.Split(string(data), "\n") {
		f := strings.Fields(line)
		if len(f) >= 2 && f[0] == "module" {
			return f[1]
		}
	}
	return ""
}

// loadPkg parses every .voi file in a package directory, recursing into its
// own imports first so the merge order is dependency-first.
func (l *loader) loadPkg(name, dir string, at diag.Pos) *pkg {
	// The cycle check must come FIRST: a package already on the stack is also
	// already in l.pkgs (it is registered before recursing), so an
	// "already loaded" early return would swallow the cycle.
	for _, on := range l.stack {
		if on == dir {
			var path []string
			for _, d := range append(append([]string{}, l.stack...), dir) {
				path = append(path, filepath.Base(d))
			}
			l.bag.Errorf(diag.SpanOf(at, len(name)), "import cycle",
				"package `%s` imports itself (through %s)", name,
				strings.Join(path, " -> "))
			return nil
		}
	}
	if p, done := l.pkgs[dir]; done {
		return p
	}
	// Two different directories may not claim the same qualifier: `a/util` and
	// `b/util` would both flatten to `util.`, and one would silently win.
	if prev, taken := l.qualifier[name]; taken && prev != dir {
		l.bag.Errorf(diag.SpanOf(at, len(name)), "",
			"two packages are both named `%s` (%s and %s); a package's flat name "+
				"must be unique in a program", name, prev, dir)
		return nil
	}
	l.qualifier[name] = dir

	l.stack = append(l.stack, dir)
	defer func() { l.stack = l.stack[:len(l.stack)-1] }()

	entries, err := os.ReadDir(dir)
	if err != nil {
		l.bag.Errorf(diag.SpanOf(at, len(name)), "", "cannot read package `%s`: %v", name, err)
		return nil
	}
	var names []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasSuffix(e.Name(), ".voi") &&
			!strings.HasSuffix(e.Name(), "_test.voi") {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names) // deterministic merge order
	if len(names) == 0 {
		l.bag.Errorf(diag.SpanOf(at, len(name)), "", "package `%s` has no .voi files", name)
		return nil
	}

	p := &pkg{name: name, dir: dir, decls: map[string]bool{}, imports: map[string]*pkg{}}
	// Register BEFORE recursing so a cycle is caught by the stack, not here.
	l.pkgs[dir] = p

	// Parse every file first, so a package's own declarations are all known
	// before its imports are resolved.
	for _, fn := range names {
		path := filepath.Join(dir, fn)
		text, err := os.ReadFile(path)
		if err != nil {
			l.bag.Errorf(diag.SpanOf(at, len(name)), "", "cannot read %s: %v", path, err)
			continue
		}
		src := diag.NewSource(relPath(l.root, path), string(text))
		l.bag.AddSource(src)
		f := parser.Parse(src, l.bag)
		p.files = append(p.files, f)
		p.sources = append(p.sources, src)
		for _, d := range f.Decls {
			for _, n := range declNames(d) {
				p.decls[n] = true
			}
		}
	}
	// Then its imports, honouring `as` aliases exactly as main's are.
	for _, f := range p.files {
		for q, dep := range l.importsOf(f) {
			p.imports[q] = dep
		}
	}
	l.order = append(l.order, p) // dependencies were appended first
	return p
}

func relPath(root, path string) string {
	if r, err := filepath.Rel(root, path); err == nil {
		return r
	}
	return path
}

// declNames lists the package-level names a declaration introduces. Enum
// VARIANTS and TRAIT names stay global (patterns and trait bounds have no
// qualified syntax), so they must be unique across the program.
func declNames(d ast.Decl) []string {
	switch decl := d.(type) {
	case *ast.FuncDecl:
		return []string{decl.Name}
	case *ast.StructDecl:
		return []string{decl.Name}
	case *ast.EnumDecl:
		return []string{decl.Name}
	case *ast.ExceptionDecl:
		return []string{decl.Name}
	case *ast.TypeDecl:
		return []string{decl.Name}
	case *ast.GlobalDecl:
		switch st := decl.Stmt.(type) {
		case *ast.LetStmt:
			return st.Names
		case *ast.VarStmt:
			return []string{st.Name}
		}
	}
	return nil
}

// errorf reports at a position with a caret of the given width.
func errorf(bag *diag.Bag, pos diag.Pos, width int, format string, args ...any) {
	bag.Errorf(diag.SpanOf(pos, width), "", format, args...)
}
