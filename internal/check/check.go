// Package check implements Voilà's static checks — the pragmatic stage-0
// subset: the acyclicity rule (§6.3), borrow-escape in struct fields (§6.2),
// widening-lattice literal checks (§3.2), match exhaustiveness (§3.5),
// straight-line use-after-move (§6.1), and literal format-string checks
// (§13.1).
package check

import (
	"voila/internal/ast"
	"voila/internal/diag"
)

type checker struct {
	file *ast.File
	src  *diag.Source
	bag  *diag.Bag

	structs map[string]*ast.StructDecl
	enums   map[string]*ast.EnumDecl
	funcs   map[string]*ast.FuncDecl
}

// Check runs all static checks, appending diagnostics to bag.
func Check(file *ast.File, src *diag.Source, bag *diag.Bag) {
	c := &checker{
		file: file, src: src, bag: bag,
		structs: map[string]*ast.StructDecl{},
		enums:   map[string]*ast.EnumDecl{},
		funcs:   map[string]*ast.FuncDecl{},
	}
	for _, d := range file.Decls {
		switch decl := d.(type) {
		case *ast.StructDecl:
			c.structs[decl.Name] = decl
		case *ast.EnumDecl:
			c.enums[decl.Name] = decl
		case *ast.FuncDecl:
			c.funcs[decl.Name] = decl
		case *ast.TypeDecl:
			if decl.Struct != nil {
				c.structs[decl.Name] = decl.Struct
			}
		}
	}
	c.checkBorrowFields()
	c.checkAcyclicity()
	c.checkLattice()
	c.checkExhaustiveness()
	c.checkMoves()
	c.checkFormatStrings()
}

// checkBorrowFields rejects borrows stored in struct fields (§6.2).
func (c *checker) checkBorrowFields() {
	for _, sd := range c.structs {
		for _, f := range sd.Fields {
			if bt, ok := f.Type.(*ast.BorrowType); ok {
				c.bag.Add(diag.Diagnostic{
					Severity: diag.Error,
					Message:  "borrow stored in struct field — borrows cannot escape (§6.2)",
					Span:     diag.SpanOf(bt.Pos(), 1),
					Label:    "borrow in struct field",
					Help:     []string{"own the value (`" + typeString(bt.Elem) + "`) or share it (`shared[" + typeString(bt.Elem) + "]`)"},
				})
			}
		}
	}
}
