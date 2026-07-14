package main

// astdump renders the Go parse tree in voilac's canonical arena format, so the
// two parsers can be diffed node for node. Every shape here mirrors the node
// layout documented in voilac/ast/ast.voi; keep the two in step.

import (
	"fmt"
	"strings"

	"voila/internal/ast"
	"voila/internal/lexer"
)

type dumper struct {
	b strings.Builder
}

func (d *dumper) pad(depth int) { d.b.WriteString(strings.Repeat("  ", depth)) }

func (d *dumper) nilNode(depth int) {
	d.pad(depth)
	d.b.WriteString("-\n")
}

// node writes one line: kind, then the attributes in voilac's order — op, i, f,
// s. Children are the caller's business.
func (d *dumper) node(depth int, kind string, attrs ...string) {
	d.pad(depth)
	d.b.WriteString(kind)
	for _, a := range attrs {
		d.b.WriteString(" ")
		d.b.WriteString(a)
	}
	d.b.WriteString("\n")
}

func iattr(v int) string         { return fmt.Sprintf("i=%d", v) }
func fattr(v int) string         { return fmt.Sprintf("f=%d", v) }
func opattr(k lexer.Kind) string { return "op=" + k.String() }

func sattr(s string) string { return "s=" + quote(s) }

func quote(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for _, c := range s {
		switch c {
		case '\\':
			b.WriteString(`\\`)
		case '"':
			b.WriteString(`\"`)
		case '\n':
			b.WriteString(`\n`)
		case '\t':
			b.WriteString(`\t`)
		case '\r':
			b.WriteString(`\r`)
		default:
			b.WriteRune(c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

// attrs assembles the optional attributes a node carries. A zero Ival/Ival2 and
// an empty Sval are omitted, exactly as the Voilà dumper omits them.
func attrs(ival, ival2 int, sval string) []string {
	var a []string
	if ival != 0 {
		a = append(a, iattr(ival))
	}
	if ival2 != 0 {
		a = append(a, fattr(ival2))
	}
	if sval != "" {
		a = append(a, sattr(sval))
	}
	return a
}

// list emits a `list` container node holding n children rendered by f.
func (d *dumper) list(depth, n int, f func(i, depth int)) {
	d.node(depth, "list")
	for i := 0; i < n; i++ {
		f(i, depth+1)
	}
}

// ---------------------------------------------------------------- file

func (d *dumper) file(f *ast.File) {
	d.node(0, "file", attrs(0, 0, f.Package)...)
	d.list(1, len(f.Uses), func(i, dp int) { d.use(f.Uses[i], dp) })
	d.list(1, len(f.Decls), func(i, dp int) { d.decl(f.Decls[i], dp) })
	d.list(1, len(f.ScriptStmts), func(i, dp int) { d.stmt(f.ScriptStmts[i], dp) })
}

func (d *dumper) use(u *ast.Use, depth int) {
	flags := 0
	if len(u.Names) > 0 {
		flags = 1
	}
	if u.Wildcard {
		flags = 2
	}
	d.node(depth, "use", attrs(0, flags, u.Path+"|"+u.Alias)...)
	for _, n := range u.Names {
		d.node(depth+1, "ident", sattr(n))
	}
}

// ---------------------------------------------------------------- decls

func (d *dumper) decl(n ast.Decl, depth int) {
	switch x := n.(type) {
	case *ast.FuncDecl:
		d.funcDecl(x, depth)
	case *ast.StructDecl:
		d.node(depth, "struct", sattr(x.Name))
		d.list(depth+1, len(x.Fields), func(i, dp int) { d.field(x.Fields[i], dp) })
		d.generics(depth+1, x.Generics)
	case *ast.EnumDecl:
		d.node(depth, "enum", sattr(x.Name))
		d.list(depth+1, len(x.Variants), func(i, dp int) {
			v := x.Variants[i]
			d.node(dp, "variant", sattr(v.Name))
			d.list(dp+1, len(v.Fields), func(j, dp2 int) { d.field(v.Fields[j], dp2) })
		})
		d.generics(depth+1, x.Generics)
	case *ast.TraitDecl:
		d.node(depth, "trait", sattr(x.Name))
		d.list(depth+1, len(x.Methods), func(i, dp int) { d.funcDecl(x.Methods[i], dp) })
	case *ast.ImplDecl:
		d.node(depth, "impl", attrs(0, 0, x.Trait)...)
		d.list(depth+1, len(x.Methods), func(i, dp int) { d.funcDecl(x.Methods[i], dp) })
		d.generics(depth+1, x.Generics)
		d.typ(x.Type, depth+1) // the Ival node ref, dumped last
	case *ast.ExceptionDecl:
		d.node(depth, "exception", sattr(x.Name))
		d.list(depth+1, len(x.Fields), func(i, dp int) { d.field(x.Fields[i], dp) })
		d.list(depth+1, len(x.Methods), func(i, dp int) { d.funcDecl(x.Methods[i], dp) })
	case *ast.TypeDecl:
		alias := 0
		if x.Alias {
			alias = 1
		}
		d.node(depth, "typedecl", attrs(0, alias, x.Name)...)
		if x.Struct != nil {
			d.node(depth+1, "struct", sattr(x.Struct.Name))
			d.list(depth+2, len(x.Struct.Fields), func(i, dp int) { d.field(x.Struct.Fields[i], dp) })
			d.generics(depth+2, nil)
		} else {
			d.nilNode(depth + 1)
		}
		d.typ(x.Target, depth+1)
	case *ast.GlobalDecl:
		d.node(depth, "global")
		d.stmt(x.Stmt, depth+1)
	default:
		d.node(depth, "?decl")
	}
}

func (d *dumper) generics(depth int, gs []ast.GenericParam) {
	d.list(depth, len(gs), func(i, dp int) {
		d.node(dp, "generic", sattr(gs[i].Name+"|"+gs[i].Bound))
	})
}

func (d *dumper) funcDecl(f *ast.FuncDecl, depth int) {
	flags := 0
	if f.HasSelf {
		flags |= 1
		switch f.SelfMode {
		case ast.ModeMut:
			flags |= 2
		case ast.ModeOwn:
			flags |= 4
		}
	}
	if f.NoThrow {
		flags |= 8
	}
	d.node(depth, "func", attrs(0, flags, f.Name)...)
	if f.Body != nil {
		d.block(f.Body, depth+1)
	} else {
		d.nilNode(depth + 1)
	}
	d.params(depth+1, f.Params)
	d.list(depth+1, len(f.Results), func(i, dp int) { d.typ(f.Results[i], dp) })
	d.generics(depth+1, f.Generics)
	d.list(depth+1, len(f.Throws), func(i, dp int) { d.typ(f.Throws[i], dp) })
	d.expr(f.Default, depth+1)
}

func (d *dumper) params(depth int, ps []*ast.Param) {
	d.list(depth, len(ps), func(i, dp int) {
		p := ps[i]
		f := int(p.Mode)
		if p.Variadic {
			f |= 16
		}
		d.node(dp, "param", attrs(0, f, p.Name)...)
		d.expr(p.Default, dp+1)
		d.typ(p.Type, dp+1)
	})
}

func (d *dumper) field(f *ast.Field, depth int) {
	d.node(depth, "fielddecl", sattr(f.Name))
	d.expr(f.Default, depth+1)
	d.typ(f.Type, depth+1)
}

// ---------------------------------------------------------------- types

func (d *dumper) typ(t ast.TypeExpr, depth int) {
	if t == nil {
		d.nilNode(depth)
		return
	}
	switch x := t.(type) {
	case *ast.NamedType:
		name := x.Name
		if x.Pkg != "" {
			name = x.Pkg + "." + x.Name
		}
		d.node(depth, "tname", sattr(name))
		for _, a := range x.Args {
			d.typ(a, depth+1)
		}
	case *ast.OptionType:
		d.node(depth, "topt")
		d.typ(x.Elem, depth+1)
	case *ast.ResultType:
		d.node(depth, "tres")
		d.typ(x.Elem, depth+1)
	case *ast.SliceType:
		d.node(depth, "tslice")
		d.typ(x.Elem, depth+1)
		d.expr(x.Len, depth+1)
	case *ast.MapType:
		d.node(depth, "tmap")
		d.typ(x.Key, depth+1)
		d.typ(x.Value, depth+1)
	case *ast.SetType:
		d.node(depth, "tset")
		d.typ(x.Elem, depth+1)
	case *ast.ChanType:
		d.node(depth, "tchan")
		d.typ(x.Elem, depth+1)
	case *ast.BoxType:
		d.node(depth, "tbox", attrs(0, int(x.Kind), "")...)
		d.typ(x.Elem, depth+1)
	case *ast.FuncType:
		d.node(depth, "tfn")
		d.list(depth+1, len(x.Params), func(i, dp int) { d.typ(x.Params[i], dp) })
		d.typ(x.Result, depth+1)
	case *ast.TupleType:
		d.node(depth, "ttuple")
		for _, e := range x.Elems {
			d.typ(e, depth+1)
		}
	case *ast.BorrowType:
		d.node(depth, "tborrow")
		d.typ(x.Elem, depth+1)
	default:
		d.node(depth, "?type")
	}
}

// ---------------------------------------------------------------- statements

func (d *dumper) block(b *ast.Block, depth int) {
	if b == nil {
		d.nilNode(depth)
		return
	}
	d.node(depth, "block")
	for _, s := range b.Stmts {
		d.stmt(s, depth+1)
	}
}

func (d *dumper) stmt(s ast.Stmt, depth int) {
	if s == nil {
		d.nilNode(depth)
		return
	}
	switch x := s.(type) {
	case *ast.Block:
		d.block(x, depth)
	case *ast.LetStmt:
		c := 0
		if x.Const {
			c = 1
		}
		d.node(depth, "let", attrs(0, c, strings.Join(x.Names, ","))...)
		d.expr(x.Value, depth+1)
		d.typ(x.Type, depth+1)
	case *ast.VarStmt:
		d.node(depth, "var", sattr(x.Name))
		d.expr(x.Value, depth+1)
		d.typ(x.Type, depth+1)
	case *ast.AssignStmt:
		d.node(depth, "assign", opattr(x.Op))
		d.expr(x.LHS[0], depth+1)
		d.expr(x.RHS, depth+1)
	case *ast.ExprStmt:
		d.node(depth, "exprstmt")
		d.expr(x.X, depth+1)
	case *ast.IfStmt:
		d.node(depth, "if")
		if x.Cond != nil {
			d.expr(x.Cond, depth+1)
		} else {
			d.expr(x.Value, depth+1)
		}
		d.block(x.Then, depth+1)
		d.stmt(x.Else, depth+1)
		d.pattern(x.Pattern, depth+1)
	case *ast.ForStmt:
		d.node(depth, "for", sattr(x.Var+"|"+x.Label))
		d.expr(x.Iter, depth+1)
		d.block(x.Body, depth+1)
	case *ast.EachStmt:
		d.node(depth, "each", sattr(x.A+","+x.B+"|"+x.Label))
		d.expr(x.Iter, depth+1)
		d.block(x.Body, depth+1)
	case *ast.WhileStmt:
		d.node(depth, "while", attrs(0, 0, x.Label)...)
		d.expr(x.Cond, depth+1)
		d.block(x.Body, depth+1)
	case *ast.SwitchStmt:
		d.node(depth, "switch")
		d.expr(x.Subject, depth+1)
		d.list(depth+1, len(x.Cases), func(i, dp int) {
			c := x.Cases[i]
			d.node(dp, "case")
			d.list(dp+1, len(c.Values), func(j, dp2 int) { d.expr(c.Values[j], dp2) })
			d.list(dp+1, len(c.Body), func(j, dp2 int) { d.stmt(c.Body[j], dp2) })
		})
	case *ast.MatchStmt:
		d.node(depth, "match")
		d.expr(x.Subject, depth+1)
		d.list(depth+1, len(x.Arms), func(i, dp int) {
			a := x.Arms[i]
			d.node(dp, "arm")
			d.expr(a.Guard, dp+1)
			d.list(dp+1, len(a.Body), func(j, dp2 int) { d.stmt(a.Body[j], dp2) })
			d.pattern(a.Pattern, dp+1)
		})
	case *ast.SelectStmt:
		d.node(depth, "select")
		d.list(depth+1, len(x.Cases), func(i, dp int) {
			c := x.Cases[i]
			d.node(dp, "selcase", attrs(int(c.Kind), 0, c.Bind+","+c.BindOk)...)
			d.expr(c.Ch, dp+1)
			d.expr(c.Send, dp+1)
			d.list(dp+1, len(c.Body), func(j, dp2 int) { d.stmt(c.Body[j], dp2) })
		})
	case *ast.GroupStmt:
		try := 0
		if x.Try {
			try = 1
		}
		d.node(depth, "group", attrs(0, try, "")...)
		d.expr(x.Timeout, depth+1)
		d.block(x.Body, depth+1)
	case *ast.ParseStmt:
		d.node(depth, "parse", attrs(0, 0, x.CaseFold)...)
		d.expr(x.Src, depth+1)
		d.list(depth+1, len(x.Terms), func(i, dp int) {
			t := x.Terms[i]
			sval := t.Name
			if t.Kind == ast.ParseLit {
				sval = t.Lit
			}
			d.node(dp, "term", attrs(int(t.Kind), t.Col, sval)...)
		})
	case *ast.SayStmt:
		d.node(depth, "say")
		for _, a := range x.Args {
			d.expr(a, depth+1)
		}
	case *ast.DeferStmt:
		if x.Block != nil {
			d.node(depth, "defer", fattr(1))
			d.block(x.Block, depth+1)
		} else {
			d.node(depth, "defer")
			d.expr(x.Call, depth+1)
		}
	case *ast.DropStmt:
		d.node(depth, "drop", sattr(x.Name))
	case *ast.ReturnStmt:
		d.node(depth, "return")
		for _, v := range x.Values {
			d.expr(v, depth+1)
		}
	case *ast.BreakStmt:
		d.node(depth, "break", attrs(0, 0, x.Label)...)
	case *ast.ContinueStmt:
		d.node(depth, "continue", attrs(0, 0, x.Label)...)
	case *ast.FallthroughStmt:
		d.node(depth, "fallthrough")
	case *ast.NumericStmt:
		d.node(depth, "numeric", attrs(x.Digits, 0, "")...)
	case *ast.ThrowStmt:
		d.node(depth, "throw")
		d.expr(x.Value, depth+1)
		d.expr(x.From, depth+1)
	case *ast.TryCatchStmt:
		d.node(depth, "trycatch")
		d.block(x.Body, depth+1)
		d.list(depth+1, len(x.Catches), func(i, dp int) {
			c := x.Catches[i]
			d.node(dp, "catch", attrs(0, 0, c.Name)...)
			d.block(c.Body, dp+1)
			d.list(dp+1, len(c.Types), func(j, dp2 int) { d.typ(c.Types[j], dp2) })
		})
		d.block(x.Finally, depth+1)
	default:
		d.node(depth, "?stmt")
	}
}

func (d *dumper) pattern(p ast.Pattern, depth int) {
	if p == nil {
		d.nilNode(depth)
		return
	}
	switch x := p.(type) {
	case *ast.WildcardPat:
		d.node(depth, "pwild")
	case *ast.NilPat:
		d.node(depth, "pnil")
	case *ast.LiteralPat:
		d.node(depth, "plit")
		d.expr(x.Value, depth+1)
	case *ast.BindPat:
		d.node(depth, "pbind", sattr(x.Name))
	case *ast.VariantPat:
		d.node(depth, "pvariant", sattr(x.Name))
		for _, e := range x.Elems {
			d.pattern(e, depth+1)
		}
	default:
		d.node(depth, "?pat")
	}
}

// ---------------------------------------------------------------- expressions

func (d *dumper) expr(e ast.Expr, depth int) {
	if e == nil {
		d.nilNode(depth)
		return
	}
	switch x := e.(type) {
	case *ast.Ident:
		d.node(depth, "ident", sattr(x.Name))
	case *ast.SelfExpr:
		d.node(depth, "self", sattr("self"))
	case *ast.BasicLit:
		switch x.Kind {
		case ast.IntLit:
			d.node(depth, "int", attrs(0, 0, x.Text)...)
		case ast.FloatLit:
			d.node(depth, "float", attrs(0, 0, x.Text)...)
		case ast.DecLit:
			d.node(depth, "dec", attrs(0, 0, x.Text)...)
		case ast.StrLit:
			d.node(depth, "str", attrs(0, 0, x.Text)...)
		case ast.RuneLit:
			d.node(depth, "rune", attrs(0, 0, x.Text)...)
		case ast.BoolLit:
			if x.Text == "true" {
				d.node(depth, "bool", iattr(1), sattr("true"))
			} else {
				d.node(depth, "bool", sattr("false"))
			}
		case ast.NilLit:
			d.node(depth, "nil", sattr("nil"))
		}
	case *ast.InterpStr:
		d.node(depth, "interp")
		for _, p := range x.Parts {
			if p.Expr != nil {
				d.expr(p.Expr, depth+1)
			} else {
				d.node(depth+1, "str", attrs(0, 0, p.Lit)...)
			}
		}
	case *ast.UnaryExpr:
		d.node(depth, "unary", opattr(x.Op))
		d.expr(x.X, depth+1)
	case *ast.BinaryExpr:
		d.node(depth, "binary", opattr(x.Op))
		d.expr(x.X, depth+1)
		d.expr(x.Y, depth+1)
	case *ast.RangeExpr:
		inc := 0
		if x.Inclusive {
			inc = 1
		}
		d.node(depth, "range", attrs(0, inc, "")...)
		d.expr(x.Lo, depth+1)
		d.expr(x.Hi, depth+1)
		d.expr(x.By, depth+1)
	case *ast.CallExpr:
		d.node(depth, "call")
		d.expr(x.Fun, depth+1)
		d.list(depth+1, len(x.Args), func(i, dp int) {
			a := x.Args[i]
			sp := 0
			if a.Spread {
				sp = 1
			}
			d.node(dp, "arg", attrs(sp, 0, a.Name)...)
			d.expr(a.Value, dp+1)
		})
		d.list(depth+1, len(x.TypeArgs), func(i, dp int) { d.typ(x.TypeArgs[i], dp) })
	case *ast.IndexExpr:
		d.node(depth, "index")
		d.expr(x.X, depth+1)
		d.expr(x.Index, depth+1)
	case *ast.FieldExpr:
		d.node(depth, "field", sattr(x.Name))
		d.expr(x.X, depth+1)
	case *ast.CompositeLit:
		name := x.Type.Name
		if x.Type.Pkg != "" {
			name = x.Type.Pkg + "." + x.Type.Name
		}
		d.node(depth, "composite", sattr(name))
		for _, f := range x.Fields {
			d.node(depth+1, "compfield", attrs(0, 0, f.Name)...)
			d.expr(f.Value, depth+2)
		}
		d.typ(x.Type, depth+1)
	case *ast.SliceLit:
		d.node(depth, "slicelit")
		for _, el := range x.Elems {
			d.expr(el, depth+1)
		}
	case *ast.TupleExpr:
		d.node(depth, "tuple")
		for _, el := range x.Elems {
			d.expr(el, depth+1)
		}
	case *ast.ClosureExpr:
		own := 0
		if x.OwnCap {
			own = 1
		}
		d.node(depth, "closure", attrs(0, own, "")...)
		d.block(x.Body, depth+1)
		d.params(depth+1, x.Params)
		d.list(depth+1, len(x.Captures), func(i, dp int) {
			d.node(dp, "ident", sattr(x.Captures[i]))
		})
		d.typ(x.Result, depth+1)
	case *ast.TryExpr:
		d.node(depth, "try")
		d.expr(x.X, depth+1)
	case *ast.MustExpr:
		d.node(depth, "must")
		d.expr(x.X, depth+1)
	case *ast.AttemptExpr:
		d.node(depth, "attempt")
		d.expr(x.X, depth+1)
	case *ast.ElseExpr:
		if x.AltStmt != nil {
			d.node(depth, "else", fattr(1))
			d.expr(x.X, depth+1)
			d.stmt(x.AltStmt, depth+1)
		} else {
			d.node(depth, "else")
			d.expr(x.X, depth+1)
			d.expr(x.Alt, depth+1)
		}
	case *ast.SpawnExpr:
		if x.Block != nil {
			d.node(depth, "spawn", fattr(1))
			d.block(x.Block, depth+1)
		} else {
			d.node(depth, "spawn")
			d.expr(x.Call, depth+1)
		}
	default:
		d.node(depth, "?expr")
	}
}
