package check

import (
	"fmt"

	"voila/internal/ast"
	"voila/internal/diag"
)

// ownEdge is a scalar owning edge in the type graph: a direct field of a
// named type, or shared[T]/cell[T]. Collection and optional edges are
// tree-shaped by construction (§6.3) and excluded; weak[T] never owns.
type ownEdge struct {
	from, to  string
	fieldName string
	span      diag.Span
	typeText  string
}

// checkAcyclicity enforces the Acyclicity Rule (§6.3): following scalar
// owning edges, the type graph must be a DAG.
func (c *checker) checkAcyclicity() {
	edges := map[string][]ownEdge{}
	addEdge := func(from string, f *ast.Field, te ast.TypeExpr) {
		to := ""
		switch ty := te.(type) {
		case *ast.NamedType:
			to = ty.Name
		case *ast.BoxType:
			if ty.Kind == ast.WeakBox {
				return
			}
			if nt, ok := ty.Elem.(*ast.NamedType); ok {
				to = nt.Name
			}
		default:
			return
		}
		if _, isStruct := c.structs[to]; !isStruct {
			if _, isEnum := c.enums[to]; !isEnum {
				return
			}
		}
		text := typeString(te)
		edges[from] = append(edges[from], ownEdge{
			from: from, to: to, fieldName: f.Name,
			span:     diag.Span{Start: te.Pos(), End: diag.Pos{Line: te.Pos().Line, Col: te.Pos().Col + len(text)}},
			typeText: text,
		})
	}

	for name, sd := range c.structs {
		for _, f := range sd.Fields {
			addEdge(name, f, f.Type)
		}
	}
	for name, ed := range c.enums {
		for _, v := range ed.Variants {
			for _, f := range v.Fields {
				addEdge(name, f, f.Type)
			}
		}
	}

	// DFS cycle detection; report the edge that closes each cycle, once.
	reported := map[string]bool{}
	var visit func(start, cur string, path map[string]bool)
	visit = func(start, cur string, path map[string]bool) {
		for _, e := range edges[cur] {
			if e.to == start {
				key := e.from + "." + e.fieldName
				if reported[key] {
					continue
				}
				reported[key] = true
				c.bag.Add(diag.Diagnostic{
					Severity: diag.Error,
					Message:  fmt.Sprintf("type `%s` forms an ownership cycle through field `%s`", start, e.fieldName),
					Span:     e.span,
					Label:    fmt.Sprintf("owning edge back to `%s`", start),
					Notes:    []string{"owning cycles can leak; Voilà forbids them"},
					Help:     []string{fmt.Sprintf("use `weak[%s]` for a back-edge", e.to)},
				})
				continue
			}
			if !path[e.to] {
				path[e.to] = true
				visit(start, e.to, path)
				delete(path, e.to)
			}
		}
	}
	for name := range edges {
		visit(name, name, map[string]bool{name: true})
	}
}

// typeString renders a type expression for messages.
func typeString(te ast.TypeExpr) string {
	switch ty := te.(type) {
	case *ast.NamedType:
		s := ty.Name
		if ty.Pkg != "" {
			s = ty.Pkg + "." + s
		}
		if len(ty.Args) > 0 {
			s += "["
			for i, a := range ty.Args {
				if i > 0 {
					s += ", "
				}
				s += typeString(a)
			}
			s += "]"
		}
		return s
	case *ast.OptionType:
		return "?" + typeString(ty.Elem)
	case *ast.ResultType:
		return typeString(ty.Elem) + "!"
	case *ast.SliceType:
		return "[]" + typeString(ty.Elem)
	case *ast.MapType:
		return "map[" + typeString(ty.Key) + "]" + typeString(ty.Value)
	case *ast.SetType:
		return "set[" + typeString(ty.Elem) + "]"
	case *ast.ChanType:
		return "chan[" + typeString(ty.Elem) + "]"
	case *ast.BoxType:
		kind := map[ast.BoxKind]string{ast.SharedBox: "shared", ast.CellBox: "cell", ast.WeakBox: "weak"}[ty.Kind]
		return kind + "[" + typeString(ty.Elem) + "]"
	case *ast.BorrowType:
		return "&" + typeString(ty.Elem)
	case *ast.FuncType:
		s := "fn("
		for i, p := range ty.Params {
			if i > 0 {
				s += ", "
			}
			s += typeString(p)
		}
		s += ")"
		if ty.Result != nil {
			s += " " + typeString(ty.Result)
		}
		return s
	case *ast.TupleType:
		s := "("
		for i, e := range ty.Elems {
			if i > 0 {
				s += ", "
			}
			s += typeString(e)
		}
		return s + ")"
	}
	return "?"
}
